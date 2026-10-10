// daphne: command-line player and test tool.

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <dr_libs/dr_flac.h>
#include <dr_libs/dr_wav.h>

#include "daphne/engine/decoder.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/text.h"

namespace {

constexpr const char* kVersion = "0.1.0";
std::atomic<bool> g_cancel{false};

BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_cancel = true;
        return TRUE;
    }
    return FALSE;
}

void usage() {
    std::cout << "daphne " << kVersion << "\n\n"
              << "usage:\n"
              << "  daphne version\n"
              << "  daphne decode-test [--md5] files...     decode files; FLAC is checked against its STREAMINFO MD5;\n"
              << "                                          --md5 prints the MD5 of the decoded stream as s32le\n"
              << "  daphne render <in> <out.wav> [--start F]\n"
              << "                                          decode to WAV (source bit depth, or float32 for lossy);\n"
              << "                                          --start: seek to frame F first\n";
}

// ---------------------------------------------------------------- MD5 (BCrypt)

class Md5 {
public:
    Md5() {
        BCryptOpenAlgorithmProvider(&alg_, BCRYPT_MD5_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg_, &h_, nullptr, 0, nullptr, 0, 0);
    }
    ~Md5() {
        if (h_) BCryptDestroyHash(h_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void update(const void* p, size_t n) { BCryptHashData(h_, (PUCHAR)p, static_cast<ULONG>(n), 0); }
    std::string hex() {
        unsigned char d[16];
        BCryptFinishHash(h_, d, 16, 0);
        char s[33];
        for (int i = 0; i < 16; ++i) snprintf(s + i * 2, 3, "%02x", d[i]);
        return s;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE h_ = nullptr;
};

void on_flac_meta(void* user, drflac_metadata* m) {
    if (m->type == DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO) std::memcpy(user, m->data.streaminfo.md5, 16);
}

std::string flac_streaminfo_md5(const std::wstring& path) {
    unsigned char md5[16] = {};
    drflac* f = drflac_open_file_with_metadata_w(path.c_str(), on_flac_meta, md5, nullptr);
    if (!f) return {};
    drflac_close(f);
    char s[33];
    for (int i = 0; i < 16; ++i) snprintf(s + i * 2, 3, "%02x", md5[i]);
    return s;
}

std::wstring suffix_lower(const std::wstring& p) {
    auto d = p.find_last_of(L'.');
    std::wstring s = d == std::wstring::npos ? L"" : p.substr(d + 1);
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// ---------------------------------------------------------------- commands

int cmd_decode_test(const std::vector<std::wstring>& args) {
    bool md5 = false;
    std::vector<std::wstring> files;
    for (const auto& a : args) {
        if (a == L"--md5") md5 = true;
        else files.push_back(a);
    }
    int bad = 0;
    for (const auto& f : files) {
        std::string err;
        auto d = daphne::open_decoder(f, &err);
        std::cout << daphne::to_utf8(f) << "\n";
        if (!d) {
            std::cout << "  error: " << err << "\n";
            ++bad;
            continue;
        }
        const auto& in = d->info();
        std::cout << "  " << in.codec << " " << in.rate << " Hz, " << in.channels << " ch"
                  << (in.bits ? ", " + std::to_string(in.bits) + "-bit" : "") << ", frames " << in.frames << "\n";
        const bool is_flac = suffix_lower(f) == L"flac" || suffix_lower(f) == L"fla";
        Md5 flac_md5, s32_md5;
        const int ch = in.channels, bps = in.bits;
        std::vector<double> buf(4096 * static_cast<size_t>(ch));
        std::vector<unsigned char> bytes;
        int64_t total = 0;
        double peak = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            const int n = d->read(buf.data(), 4096);
            if (n <= 0) break;
            total += n;
            const size_t ns = static_cast<size_t>(n) * ch;
            for (size_t i = 0; i < ns; ++i) peak = std::max(peak, std::abs(buf[i]));
            if (is_flac && bps > 0) {
                // FLAC MD5: signed samples at the source width, little-endian, ceil(bps/8) bytes each
                const int w = (bps + 7) / 8;
                bytes.resize(ns * w);
                for (size_t i = 0; i < ns; ++i) {
                    const int64_t s = static_cast<int64_t>(std::llround(buf[i] * 2147483648.0)) >> (32 - bps);
                    for (int b = 0; b < w; ++b) bytes[i * w + b] = static_cast<unsigned char>(s >> (8 * b));
                }
                flac_md5.update(bytes.data(), bytes.size());
            }
            if (md5) {
                bytes.resize(ns * 4);
                for (size_t i = 0; i < ns; ++i) {
                    double v = std::nearbyint(buf[i] * 2147483648.0);
                    v = std::min(2147483647.0, std::max(-2147483648.0, v));
                    const int32_t s = static_cast<int32_t>(v);
                    std::memcpy(bytes.data() + i * 4, &s, 4);
                }
                s32_md5.update(bytes.data(), bytes.size());
            }
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "  decoded " << total << " frames, peak " << peak << ", "
                  << (in.rate && secs > 0 ? total / double(in.rate) / secs : 0) << "x realtime\n";
        if (in.frames >= 0 && total != in.frames) std::cout << "  WARNING: frame count " << total << " != header " << in.frames << "\n";
        if (is_flac && bps > 0) {
            const std::string want = flac_streaminfo_md5(f), got = flac_md5.hex();
            const bool unset = want == std::string(32, '0');
            std::cout << "  FLAC MD5 " << got << (unset ? " (STREAMINFO MD5 not set)" : got == want ? " OK" : " MISMATCH (want " + want + ")")
                      << "\n";
            if (!unset && got != want) ++bad;
        }
        if (md5) std::cout << "  s32le MD5 " << s32_md5.hex() << "\n";
    }
    return bad ? 2 : 0;
}

int cmd_render(const std::wstring& in_path, const std::wstring& out_path, int64_t start, int dsd_lowpass) {
    std::string err;
    std::unique_ptr<daphne::Decoder> d;
    if (dsd_lowpass > 0 && daphne::is_dsd_path(in_path)) {
        d = daphne::make_dsd_pcm_decoder(0, dsd_lowpass);
        if (!d->open(in_path, &err)) d.reset();
    } else {
        d = daphne::open_decoder(in_path, &err);
    }
    if (!d) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    if (start > 0 && !d->seek(start)) {
        std::cerr << "error: cannot seek to frame " << start << "\n";
        return 1;
    }
    const auto& in = d->info();
    const bool as_int = in.lossless_int && in.bits > 0;
    const int bits = as_int ? (in.bits <= 16 ? 16 : in.bits <= 24 ? 24 : 32) : 32;
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = as_int ? DR_WAVE_FORMAT_PCM : DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = static_cast<drwav_uint32>(in.channels);
    fmt.sampleRate = static_cast<drwav_uint32>(in.rate);
    fmt.bitsPerSample = static_cast<drwav_uint32>(bits);
    drwav w;
    if (!drwav_init_file_write_w(&w, out_path.c_str(), &fmt, nullptr)) {
        std::cerr << "error: cannot write " << daphne::to_utf8(out_path) << "\n";
        return 1;
    }
    const int ch = in.channels;
    std::vector<double> buf(4096 * static_cast<size_t>(ch));
    std::vector<unsigned char> bytes(4096 * static_cast<size_t>(ch) * 4);
    int64_t total = 0;
    const int w_bytes = bits / 8;
    for (;;) {
        const int n = d->read(buf.data(), 4096);
        if (n <= 0) break;
        const size_t ns = static_cast<size_t>(n) * ch;
        for (size_t i = 0; i < ns; ++i) {
            if (as_int) {
                const int64_t s = static_cast<int64_t>(std::llround(buf[i] * 2147483648.0)) >> (32 - bits);
                for (int b = 0; b < w_bytes; ++b) bytes[i * w_bytes + b] = static_cast<unsigned char>(s >> (8 * b));
            } else {
                const float v = static_cast<float>(buf[i]);
                std::memcpy(bytes.data() + i * 4, &v, 4);
            }
        }
        drwav_write_pcm_frames(&w, n, bytes.data());
        total += n;
    }
    drwav_uninit(&w);
    std::cout << "rendered " << total << " frames (" << in.codec << " -> "
              << (as_int ? std::to_string(bits) + "-bit PCM" : "float32") << ")\n";
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    std::vector<std::wstring> a(argv + 1, argv + argc);
    if (a.empty() || a[0] == L"help" || a[0] == L"--help" || a[0] == L"-h") {
        usage();
        return a.empty() ? 1 : 0;
    }
    const std::vector<std::wstring> rest(a.begin() + 1, a.end());
    if (a[0] == L"version") {
        std::cout << "daphne " << kVersion << "\n";
        return 0;
    }
    if (a[0] == L"decode-test") return cmd_decode_test(rest);
    if (a[0] == L"render") {
        std::vector<std::wstring> r = rest;
        int64_t start = 0;
        int dsd_lowpass = 0;
        for (size_t i = 0; i + 1 < r.size();)
            if (r[i] == L"--start" || r[i] == L"--dsd-lowpass") {
                if (r[i] == L"--start") start = std::stoll(r[i + 1]);
                else dsd_lowpass = std::stoi(r[i + 1]);
                r.erase(r.begin() + static_cast<std::ptrdiff_t>(i), r.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            } else {
                ++i;
            }
        if (r.size() != 2) {
            usage();
            return 1;
        }
        return cmd_render(r[0], r[1], start, dsd_lowpass);
    }
    usage();
    return 1;
}
