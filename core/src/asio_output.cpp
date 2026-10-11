#include "daphne/engine/asio_output.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "daphne/engine/devices.h"
#include "daphne/engine/text.h"

#ifdef DAPHNE_HAVE_ASIO
#include <iasiodrv.h>
#endif

namespace daphne {

#ifndef DAPHNE_HAVE_ASIO

struct AsioOutput::Impl {};
AsioOutput::AsioOutput() : impl_(nullptr) {}
AsioOutput::~AsioOutput() = default;
bool AsioOutput::open(const AsioOptions&, int, int, std::string* error) {
    if (error) *error = "this build has no ASIO support (put the Steinberg ASIO SDK in third_party/asiosdk and rebuild)";
    return false;
}
bool AsioOutput::start(ByteRing*, const std::atomic<bool>*, std::string* error) {
    if (error) *error = "no ASIO support";
    return false;
}
void AsioOutput::stop() {}
void AsioOutput::close() {}
void AsioOutput::on_buffer(long) {}
void AsioOutput::on_rate_changed(double) {}
long AsioOutput::on_message(long, long) { return 0; }
std::vector<std::string> AsioOutput::take_notes() { return {}; }

#else

namespace {

AsioOutput* g_out = nullptr;   // ASIO callbacks carry no context

void cb_buffer_switch(long index, ASIOBool) {
    if (g_out) g_out->on_buffer(index);
}
void cb_rate_changed(ASIOSampleRate r) {
    if (g_out) g_out->on_rate_changed(r);
}
long cb_message(long selector, long value, void*, double*) { return g_out ? g_out->on_message(selector, value) : 0; }
ASIOTime* cb_buffer_switch_time_info(ASIOTime* p, long index, ASIOBool) {
    if (g_out) g_out->on_buffer(index);
    return p;
}

ASIOCallbacks g_callbacks = {cb_buffer_switch, cb_rate_changed, cb_message, cb_buffer_switch_time_info};

bool asio_ok(ASIOError e) { return e == ASE_OK || e == ASE_SUCCESS; }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// ASIO sample type -> device format (container bits, valid bits, big-endian)
bool map_type(ASIOSampleType t, DeviceFormat& f) {
    f.big_endian = t < ASIOSTInt16LSB;
    const int base = f.big_endian ? t + ASIOSTInt16LSB : t;   // DSD types (32, 33) are above both
    switch (base) {
    case ASIOSTInt16LSB: f.type = SampleType::Int16, f.container_bits = f.valid_bits = 16; return true;
    case ASIOSTInt24LSB: f.type = SampleType::Packed24, f.container_bits = f.valid_bits = 24; return true;
    case ASIOSTInt32LSB: f.type = SampleType::Int32, f.container_bits = f.valid_bits = 32; return true;
    case ASIOSTFloat32LSB: f.type = SampleType::Float32, f.container_bits = f.valid_bits = 32; return true;
    case ASIOSTFloat64LSB: f.type = SampleType::Float64, f.container_bits = f.valid_bits = 64; return true;
    case ASIOSTInt32LSB16: f.type = SampleType::Int32Low, f.container_bits = 32, f.valid_bits = 16; return true;
    case ASIOSTInt32LSB18: f.type = SampleType::Int32Low, f.container_bits = 32, f.valid_bits = 18; return true;
    case ASIOSTInt32LSB20: f.type = SampleType::Int32Low, f.container_bits = 32, f.valid_bits = 20; return true;
    case ASIOSTInt32LSB24: f.type = SampleType::Int32Low, f.container_bits = 32, f.valid_bits = 24; return true;
    case ASIOSTDSDInt8MSB1: f.type = SampleType::Dsd8Msb, f.container_bits = 8, f.valid_bits = 1, f.big_endian = false; return true;
    case ASIOSTDSDInt8LSB1: f.type = SampleType::Dsd8Lsb, f.container_bits = 8, f.valid_bits = 1, f.big_endian = false; return true;
    default: return false;   // ASIOSTDSDInt8NER8 (one DSD bit per byte): not supported
    }
}

// CoCreateInstance, then the driver DLL directly (InprocServer32).
IASIO* create_driver(const CLSID& clsid, std::string* how) {
    IASIO* drv = nullptr;
    if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&drv))) && drv) {
        *how = "COM";
        return drv;
    }
    wchar_t sclsid[64];
    StringFromGUID2(clsid, sclsid, 64);
    std::wstring key = std::wstring(L"CLSID\\") + sclsid + L"\\InprocServer32";
    wchar_t path[MAX_PATH] = {};
    LONG size = sizeof path;
    if (RegQueryValueW(HKEY_CLASSES_ROOT, key.c_str(), path, &size) != ERROR_SUCCESS) return nullptr;
    HMODULE dll = LoadLibraryW(path);
    if (!dll) return nullptr;
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    auto get = reinterpret_cast<GetClassObject>(GetProcAddress(dll, "DllGetClassObject"));
    IClassFactory* cf = nullptr;
    if (get && SUCCEEDED(get(clsid, IID_IClassFactory, reinterpret_cast<void**>(&cf))) && cf) {
        cf->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&drv));
        cf->Release();
    }
    if (drv) *how = "in-proc DLL";
    return drv;   // the DLL stays loaded for the process lifetime
}

// The driver DLL stays loaded for the process lifetime. Released through COM, it is unloaded at
// CoUninitialize, and a driver thread that is still winding down then runs in freed code (seen as a
// crash in a vendor driver when a device restart left native DSD).
void pin_driver_module(IASIO* drv) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(*reinterpret_cast<void**>(drv)), &m);
}

}  // namespace

struct AsioOutput::Impl {
    IASIO* drv = nullptr;
    bool com_init = false;
    bool buffers = false;
    bool running = false;
    bool output_ready = false;
    bool dsd_started = false;   // a native DSD stream ran: close() returns the DAC to PCM
    bool pcm_flush = false;     // the callback writes zeros (PCM silence session)
    std::vector<size_t> flush_bytes;
    std::vector<ASIOBufferInfo> infos;
    LARGE_INTEGER freq{}, last{}, started{};
};

AsioOutput::AsioOutput() : impl_(new Impl) {}
AsioOutput::~AsioOutput() {
    close();
    delete impl_;
}

bool AsioOutput::open(const AsioOptions& opt, int rate, int channels, std::string* error) {
    close();
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        close();
        return false;
    };
    if (g_out) return fail("another ASIO stream is open");
    impl_->com_init = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    // pick the driver
    std::string lerr;
    const auto drivers = list_asio_drivers(&lerr);
    if (drivers.empty()) return fail(lerr.empty() ? "no ASIO drivers installed" : lerr);
    const AsioDriver* d = nullptr;
    if (opt.driver.empty()) d = &drivers[0];
    else if (std::all_of(opt.driver.begin(), opt.driver.end(), ::isdigit)) {
        const size_t i = std::stoul(opt.driver);
        if (i < drivers.size()) d = &drivers[i];
    } else {
        for (const auto& x : drivers)
            if (lower(x.name) == lower(opt.driver) || lower(x.clsid) == lower(opt.driver)) d = &x;
        if (!d)
            for (const auto& x : drivers)
                if (lower(x.name).find(lower(opt.driver)) != std::string::npos) d = &x;
    }
    if (!d) return fail("ASIO driver not found: " + opt.driver);
    CLSID clsid{};
    if (FAILED(CLSIDFromString(to_wide(d->clsid).c_str(), &clsid))) return fail("invalid CLSID for " + d->name);
    std::string how;
    impl_->drv = create_driver(clsid, &how);
    if (!impl_->drv) return fail("cannot load ASIO driver " + d->name);
    pin_driver_module(impl_->drv);
    IASIO* drv = impl_->drv;

    if (!drv->init(GetDesktopWindow())) {
        char msg[128] = {};
        drv->getErrorMessage(msg);
        return fail(std::string("ASIO init failed: ") + msg);
    }
    char dname[64] = {};
    drv->getDriverName(dname);
    name_ = dname[0] ? dname : d->name;

    long hw_in = 0, hw_out = 0;
    if (!asio_ok(drv->getChannels(&hw_in, &hw_out)) || hw_out <= 0) return fail("driver reports 0 physical output channels");
    const int nch = std::min(channels, static_cast<int>(hw_out));   // clamp: the chain downmixes

    ASIOIoFormat io{};
    io.FormatType = opt.dsd ? kASIODSDFormat : kASIOPCMFormat;
    const ASIOError fr = drv->future(kAsioSetIoFormat, &io);
    if (opt.dsd && !asio_ok(fr)) return fail("the ASIO driver has no DSD mode");

    rates_.clear();
    if (!opt.dsd)
        for (int r : {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000})
            if (drv->canSampleRate(static_cast<ASIOSampleRate>(r)) == ASE_OK) rates_.push_back(r);

    // rate lock: set, then read back up to 3 times 5 ms apart, re-setting each time
    const double want = rate;
    Sleep(5);
    ASIOError r = drv->setSampleRate(want);
    if (!asio_ok(r) && drv->canSampleRate(want) == ASE_OK) r = drv->setSampleRate(want);
    bool locked = false;
    ASIOSampleRate hw = 0;
    for (int k = 0; k < 3 && !locked; ++k) {
        Sleep(5);
        hw = 0;
        if (asio_ok(drv->getSampleRate(&hw)) && std::abs(hw - want) < 1.0) locked = true;
        else r = drv->setSampleRate(want);
    }
    if (!locked && !asio_ok(r)) return fail("hardware PLL failed to lock " + std::to_string(rate) + " Hz");
    if (!locked && !asio_ok(drv->getSampleRate(&hw))) hw = want;
    fmt_.rate = static_cast<int>(std::lround(hw));   // the rate the hardware reports

    long bmin = 0, bmax = 0, bpref = 0, bgran = 0;
    if (!asio_ok(drv->getBufferSize(&bmin, &bmax, &bpref, &bgran)) || bpref <= 0) bpref = 512;
    buffer_frames_ = static_cast<int>(bpref);

    // sample type: every channel must use the same one
    fmt_.channels = nch;
    for (int c = 0; c < nch; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = c;
        ci.isInput = ASIOFalse;
        if (!asio_ok(drv->getChannelInfo(&ci))) return fail("getChannelInfo failed");
        DeviceFormat f = fmt_;
        if (!map_type(ci.type, f)) return fail("unsupported ASIO sample type " + std::to_string(ci.type));
        if (is_dsd(f.type) != opt.dsd)
            return fail(opt.dsd ? "the ASIO driver did not switch to DSD buffers" : "the ASIO driver is still in DSD mode");
        if (c == 0) fmt_ = f;
        else if (f.type != fmt_.type || f.valid_bits != fmt_.valid_bits || f.big_endian != fmt_.big_endian)
            return fail("output channels use different sample types");
    }

    impl_->infos.assign(static_cast<size_t>(nch), ASIOBufferInfo{});
    for (int c = 0; c < nch; ++c) {
        impl_->infos[c].isInput = ASIOFalse;
        impl_->infos[c].channelNum = c;
    }
    g_out = this;
    r = drv->createBuffers(impl_->infos.data(), nch, bpref, &g_callbacks);
    if (!asio_ok(r)) {
        g_out = nullptr;
        char msg[128] = {};
        drv->getErrorMessage(msg);
        return fail("createBuffers failed: " + std::to_string(r) + " (" + msg + ")");
    }
    impl_->buffers = true;
    long lin = 0, lout = 0;
    if (asio_ok(drv->getLatencies(&lin, &lout)) && fmt_.rate > 0) latency_ms_ = 1000.0 * lout / fmt_.rate;
    impl_->output_ready = drv->outputReady() == ASE_OK;
    // DSD mode: ASIO buffer sizes count 1-bit samples; our frames are bytes (8 samples per channel)
    if (is_dsd(fmt_.type)) buffer_frames_ = static_cast<int>(bpref / 8);
    tmp_.assign(static_cast<size_t>(buffer_frames_) * fmt_.block_align(), 0);
    period_us_ = fmt_.frame_rate() > 0 ? static_cast<int64_t>(buffer_frames_) * 1000000 / fmt_.frame_rate() : 0;
    matched_ = fmt_.rate == rate && fmt_.channels == channels;
    QueryPerformanceFrequency(&impl_->freq);
    (void)how;
    return true;
}

bool AsioOutput::start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) {
    if (!impl_->drv || !impl_->buffers) {
        if (error) *error = "ASIO stream not open";
        return false;
    }
    ring_ = ring;
    eof_ = eof;
    drained_ = false;
    failed_ = false;
    impl_->last.QuadPart = 0;
    reset_ = false;
    stats_.callbacks = 0, stats_.underruns = 0, stats_.max_interval_us = 0, stats_.late_callbacks = 0;   // a new stream
    QueryPerformanceCounter(&impl_->started);
    feeder_.configure(fmt_);
    const ASIOError r = impl_->drv->start();
    if (!asio_ok(r)) {
        char msg[128] = {};
        impl_->drv->getErrorMessage(msg);
        if (error) *error = std::string("ASIO start failed: ") + msg;
        return false;
    }
    impl_->running = true;
    if (is_dsd(fmt_.type)) impl_->dsd_started = true;
    return true;
}

void AsioOutput::on_buffer(long index) {
    if (impl_->pcm_flush) {
        for (size_t c = 0; c < impl_->infos.size(); ++c) std::memset(impl_->infos[c].buffers[index], 0, impl_->flush_bytes[c]);
        if (impl_->output_ready) impl_->drv->outputReady();
        return;
    }
    if (!ring_) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (impl_->last.QuadPart) {
        const int64_t us = (now.QuadPart - impl_->last.QuadPart) * 1000000 / impl_->freq.QuadPart;
        if (us > stats_.max_interval_us) stats_.max_interval_us = us;
        if (period_us_ > 0 && us * 2 > period_us_ * 3) stats_.late_callbacks++;
    }
    impl_->last = now;

    const int block = fmt_.block_align();
    if (feeder_.fill(tmp_.data(), buffer_frames_, ring_, eof_->load(), stats_)) drained_ = true;
    const int nch = fmt_.channels;
    const int ss = fmt_.container_bits / 8;
    for (int c = 0; c < nch; ++c) {
        auto* dst = static_cast<uint8_t*>(impl_->infos[c].buffers[index]);
        const uint8_t* src = tmp_.data() + static_cast<size_t>(c) * ss;
        if (!fmt_.big_endian) {
            for (int i = 0; i < buffer_frames_; ++i, src += block, dst += ss) std::memcpy(dst, src, ss);
        } else {
            for (int i = 0; i < buffer_frames_; ++i, src += block, dst += ss)
                for (int b = 0; b < ss; ++b) dst[b] = src[ss - 1 - b];
        }
    }
    if (impl_->output_ready) impl_->drv->outputReady();
    stats_.callbacks++;
}

void AsioOutput::note(const std::string& s) {
    std::lock_guard<std::mutex> l(notes_m_);
    if (notes_.size() < 64) notes_.push_back(s);
}

std::vector<std::string> AsioOutput::take_notes() {
    std::lock_guard<std::mutex> l(notes_m_);
    std::vector<std::string> v;
    v.swap(notes_);
    return v;
}

void AsioOutput::on_rate_changed(double rate) {
    note("asio: the driver reports the sample rate " + std::to_string(std::lround(rate)) + " Hz (opened at " +
         std::to_string(fmt_.rate) + " Hz)");
    if (std::abs(rate - fmt_.rate) >= 1.0) {
        failure_ = "the ASIO driver changed the sample rate to " + std::to_string(static_cast<int>(rate)) + " Hz";
        failed_ = true;
    }
}

long AsioOutput::on_message(long selector, long value) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest || value == kAsioBufferSizeChange || value == kAsioEngineVersion ||
                       value == kAsioResyncRequest || value == kAsioLatenciesChanged
                   ? 1L
                   : 0L;
    case kAsioEngineVersion: return 2;
    case kAsioResetRequest:      // hardware clock reset request
    case kAsioBufferSizeChange: {
        // reopen the device. The driver must not be reopened inside its own callback: the player
        // does it. Drivers send one while starting up: ignored.
        const char* what = selector == kAsioResetRequest ? "reset request" : "buffer size change";
        if (impl_->pcm_flush || !impl_->running) {
            note(std::string("asio: ") + what + " while not playing: ignored");
            return 1;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double since = static_cast<double>(now.QuadPart - impl_->started.QuadPart) / impl_->freq.QuadPart;
        char b[160];
        std::snprintf(b, sizeof b, "asio: %s (value %ld) %.2f s after start", what, value, since);
        if (since < 0.5) {
            note(std::string(b) + ": ignored");
        } else {
            note(std::string(b) + ": reopening the device");
            reset_ = true;
        }
        return 1;
    }
    case kAsioResyncRequest:
        note("asio: resync request (the driver lost or repeated samples)");
        return 1;
    case kAsioLatenciesChanged: {
        long lin = 0, lout = 0;
        if (impl_->drv && asio_ok(impl_->drv->getLatencies(&lin, &lout)) && fmt_.rate > 0)
            latency_ms_ = 1000.0 * lout / fmt_.rate;
        note("asio: latencies changed (output " + std::to_string(lout) + " samples)");
        return 1;
    }
    default:
        note("asio: message " + std::to_string(selector) + " value " + std::to_string(value) + " (not handled)");
        return 0;
    }
}

void AsioOutput::stop() {
    if (impl_->drv && impl_->running) {
        impl_->drv->stop();
        impl_->running = false;
    }
}

// After native DSD some DACs mishandle what follows until they receive PCM (a DoP stream right after
// it made one channel beep without end): a short PCM 44.1 kHz session that writes only zeros,
// through the normal callback path.
void AsioOutput::pcm_silence_session() {
    IASIO* drv = impl_->drv;
    ASIOIoFormat io{};
    io.FormatType = kASIOPCMFormat;
    drv->future(kAsioSetIoFormat, &io);
    drv->setSampleRate(44100.0);
    long bmin = 0, bmax = 0, bpref = 0, bgran = 0;
    if (!asio_ok(drv->getBufferSize(&bmin, &bmax, &bpref, &bgran)) || bpref <= 0) bpref = 512;
    const size_t nch = impl_->infos.size();
    impl_->flush_bytes.assign(nch, static_cast<size_t>(bpref) * 4);
    for (size_t c = 0; c < nch; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = static_cast<long>(c);
        ci.isInput = ASIOFalse;
        DeviceFormat f;
        if (asio_ok(drv->getChannelInfo(&ci)) && map_type(ci.type, f))
            impl_->flush_bytes[c] = static_cast<size_t>(bpref) * f.container_bits / 8;
        impl_->infos[c].isInput = ASIOFalse;
        impl_->infos[c].channelNum = static_cast<long>(c);
    }
    impl_->pcm_flush = true;
    g_out = this;
    if (asio_ok(drv->createBuffers(impl_->infos.data(), static_cast<long>(nch), bpref, &g_callbacks))) {
        impl_->output_ready = drv->outputReady() == ASE_OK;
        if (asio_ok(drv->start())) {
            Sleep(200);
            drv->stop();
        }
        drv->disposeBuffers();
    }
    impl_->pcm_flush = false;
}

void AsioOutput::close() {
    stop();
    if (impl_->drv) {
        if (impl_->buffers) impl_->drv->disposeBuffers();
        if (impl_->buffers && impl_->dsd_started) pcm_silence_session();
        impl_->dsd_started = false;
        impl_->buffers = false;
        impl_->drv->Release();
        impl_->drv = nullptr;
    }
    if (g_out == this) g_out = nullptr;
    if (impl_->com_init) CoUninitialize();
    impl_->com_init = false;
    ring_ = nullptr;
}

#endif  // DAPHNE_HAVE_ASIO

}  // namespace daphne
