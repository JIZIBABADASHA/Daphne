#include "daphne/engine/devices.h"

// clang-format off
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <wrl/client.h>
// clang-format on

#include <cstdio>

#include "daphne/engine/text.h"

using Microsoft::WRL::ComPtr;

namespace daphne {
namespace {

// Defined here instead of via <initguid.h> so the GUIDs need no extra link library.
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr DWORD kFrontLeft = 0x1, kFrontRight = 0x2, kFrontCenter = 0x4;

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    // RPC_E_CHANGED_MODE: the thread already uses another apartment, which is fine.
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

WAVEFORMATEXTENSIBLE make_pcm(int rate, int bits, int valid_bits, int channels) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = static_cast<DWORD>(rate);
    f.Format.wBitsPerSample = static_cast<WORD>(bits);
    f.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = static_cast<WORD>(valid_bits);
    f.dwChannelMask = channels == 2 ? (kFrontLeft | kFrontRight) : kFrontCenter;
    f.SubFormat = kSubtypePcm;
    return f;
}

void probe(IMMDevice* dev, WasapiDevice& out) {
    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))))
        return;
    static const int kRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
    static const int kBits[][2] = {{16, 16}, {24, 24}, {32, 24}, {32, 32}};
    for (int rate : kRates)
        for (const auto& b : kBits) {
            auto f = make_pcm(rate, b[0], b[1], 2);
            if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) == S_OK)
                out.exclusive.push_back({rate, b[0], b[1]});
        }
}

}  // namespace

std::vector<WasapiDevice> list_wasapi_devices(bool probe_exclusive, std::string* error) {
    std::vector<WasapiDevice> result;
    ComInit com;
    if (!com.ok()) {
        if (error) *error = hr_text("CoInitializeEx", com.hr);
        return result;
    }
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) {
        if (error) *error = hr_text("MMDeviceEnumerator", hr);
        return result;
    }

    std::wstring default_id;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            default_id = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> col;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) {
        if (error) *error = hr_text("EnumAudioEndpoints", hr);
        return result;
    }
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        WasapiDevice d;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            d.id = to_utf8(id);
            d.is_default = default_id == id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) d.name = to_utf8(v.pwszVal);
            PropVariantClear(&v);
        }
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.GetAddressOf())))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
                d.mix_rate = static_cast<int>(mix->nSamplesPerSec);
                d.mix_channels = mix->nChannels;
                d.mix_bits = mix->wBitsPerSample;
                CoTaskMemFree(mix);
            }
        }
        if (probe_exclusive) probe(dev.Get(), d);
        result.push_back(std::move(d));
    }
    return result;
}

}  // namespace daphne
