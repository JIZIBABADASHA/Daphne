#include "daphne/engine/player.h"

#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#include <xmmintrin.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "daphne/engine/chain.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/dsd_output.h"
#include "daphne/engine/filters.h"
#include "daphne/dsp/progress.h"
#include "daphne/engine/ring_buffer.h"

namespace daphne {

namespace {

constexpr int kChunk = 4096;
constexpr double kFadeSeconds = 0.05;
constexpr double kPi = 3.141592653589793;

std::unique_ptr<AudioOutput> open_output(const OutputChoice& out, int rate, int channels, std::string* error, bool dsd = false) {
    if (out.api == OutputApi::Asio) {
        auto a = std::make_unique<AsioOutput>();
        if (!a->open({out.device, dsd}, rate, channels, error)) return nullptr;
        return a;
    }
    auto w = std::make_unique<WasapiOutput>();
    if (!w->open({out.device, out.api != OutputApi::WasapiShared}, rate, channels, error)) return nullptr;
    return w;
}

// The device rate asked for: pcm core, the output rate for the source; dsd core, the DoP rate
// (DSD clock / 16).
int requested_rate(const Settings& s, int fs_in) {
    if (s.core == CoreMode::Dsd) return dop_rate(dsd_clock(s.modulator.osr));
    return pcm_output_rate(fs_in, s.output.rate);
}

// Whether a chain for a source at `rate` plays on the output now running (chain output rate
// chain_rate) without reopening the device: pcm, a device rate the settings give the new source
// (stepped down by powers of two as open_and_start does); dsd, the same modulator input rate (the
// DSD clock does not depend on the source).
bool same_output(const Settings& cs, int rate, int chain_rate) {
    if (cs.core == CoreMode::Dsd) return modulator_input_rate(rate) == chain_rate;
    for (int r = pcm_output_rate(rate, cs.output.rate); r >= rate && r % rate == 0; r /= 2)
        if (r == chain_rate) return true;
    return false;
}

// Modulator settings that need a new modulator (a device restart).
std::string modulator_key(const Settings& s) {
    if (s.core != CoreMode::Dsd) return "";
    return settings_to_json(s)["dsd_modulator"].dump() + std::to_string(static_cast<int>(s.output.dsd)) + "m" +
           std::to_string(s.output.max_dsd);
}

// Settings as the chain sees them. osr > 0: the DSD multiple the device runs (the dsd core may have
// stepped down; the chain's limiter follows the modulator's stable range).
Settings chain_settings(const Settings& s, int osr = 0) {
    Settings c = s;
    if (osr > 0) c.modulator.osr = osr;
    return c;
}

// Realtime setup: 1 ms timer resolution, no EcoQoS throttling, MMCSS for the processing thread,
// denormals flushed (FTZ / DAZ).
struct RealtimeScope {
    HANDLE mmcss = nullptr;
    unsigned int csr = 0;
    RealtimeScope() {
        timeBeginPeriod(1);
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        pt.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
        DWORD task = 0;
        mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        csr = _mm_getcsr();
        _mm_setcsr(csr | 0x8040);   // FTZ (bit 15) + DAZ (bit 6)
    }
    ~RealtimeScope() {
        _mm_setcsr(csr);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        timeEndPeriod(1);
    }
};

// Interleaved frame FIFO for the crossfade.
struct Fifo {
    std::vector<double> d;
    size_t off = 0;   // values
    int ch = 1;
    int frames() const { return static_cast<int>((d.size() - off) / ch); }
    void push(const double* x, int n) { d.insert(d.end(), x, x + static_cast<size_t>(n) * ch); }
    void push_zeros(int n) { d.insert(d.end(), static_cast<size_t>(n) * ch, 0.0); }
    const double* data() const { return d.data() + off; }
    void pop(int n) {
        off += static_cast<size_t>(n) * ch;
        if (off >= d.size()) d.clear(), off = 0;
        else if (off > (1u << 20)) d.erase(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(off)), off = 0;
    }
    void clear() { d.clear(), off = 0; }
};

}  // namespace

struct Player::Impl {
    // setup
    std::wstring path;
    Settings s;
    OutputChoice out;
    PlayCallbacks cb;

    // threading
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool start_done = false, start_ok = false;
    std::string start_err;
    std::optional<std::pair<Settings, OutputChoice>> pending;
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<int64_t> pos{0};
    PlayReport rep;
    bool ok = false;
    std::string err;

    // playback (DSP thread only)
    std::unique_ptr<Decoder> dec;
    std::unique_ptr<AudioOutput> dev;
    std::unique_ptr<DspChain> cur, next;
    std::future<std::unique_ptr<DspChain>> building;
    std::optional<std::pair<Settings, OutputChoice>> build_target, queued;
    std::unique_ptr<ByteRing> ring;
    std::vector<double> pcm, dsp, tmp_c, tmp_n, hist_tmp;
    std::vector<uint8_t> bytes;
    std::vector<double> hist;
    int hist_cap = 1 << 17;
    int64_t in_total = 0;    // source frames read since the start of the track
    int64_t abs_out = 0;     // device frames of the current chain's timeline
    int ratio = 1, ch = 0, dch = 0, block = 0, want = 0;
    // chain output rate / format (= the device's, except for the dsd core: the modulator's PCM)
    int crate = 0;
    DeviceFormat chain_fmt{};
    std::unique_ptr<DsdEncoder> dsdenc;
    std::unique_ptr<DsdFile> dsdf;   // DSD file played directly (no chain)
    std::vector<uint8_t> dsd_in;
    int dsd_pcm_rate = 0;
    double start_sec = 0.0;          // Player::start position
    // transport requests (under m) and state
    std::atomic<bool> want_pause{false}, ended_flag{false};
    std::optional<double> seek_req;
    std::wstring next_path;          // gapless candidate (set_next)
    std::optional<double> next_headroom;   // its headroom_db (ReplayGain), applied at the decode boundary
    std::wstring cur_path;           // the track being heard
    struct Switch {
        int64_t at_in = 0;           // stream input frame where the track starts
        AudioInfo info;
        std::wstring path;
        // gapless across sample rates (the same output, cross_switch): the new chain's output starts at device
        // frame at_out and its first sample is heard new_delay later. Until then the hearing side stays on the
        // old segment, whose parameters are kept here; a seek or restart before it goes back to old_chain.
        bool cross = false;
        std::optional<double> old_headroom;   // the headroom before this track's own (set_next), restored on undo
        int64_t at_out = 0;
        int new_delay = 0;
        int old_ratio = 1, old_delay = 0;
        int64_t old_base = 0;
        std::unique_ptr<DspChain> old_chain;
    };
    std::deque<Switch> switches;     // gapless tracks decoded but not yet heard
    int64_t h_base = 0;              // device frame of the heard segment's input frame 0 (gapless across rates)
    // The next track (set_next) opened ahead on a background thread, and for another sample rate on the same
    // output its chain built too, so the boundary does not wait for a slow disk or a filter design.
    struct Prep {
        std::wstring path;
        std::atomic<bool> opened{false}, done{false}, cancel{false};
        std::unique_ptr<Decoder> dec;
        std::unique_ptr<DsdFile> dsdf;
        std::unique_ptr<DspChain> chain;
        std::string key;             // what the chain was built for (chain_key)
    };
    std::shared_ptr<Prep> prep;
    std::vector<std::pair<std::shared_ptr<Prep>, std::thread>> prep_threads;
    int64_t track_origin_in = 0;     // stream input frame of the current track's frame 0
    int64_t hist_valid_from = 0;     // first input frame the crossfade history holds
    std::atomic<double> pos_sec{0.0};
    std::atomic<double> meter_l{0.0}, meter_r{0.0};
    bool end_queued = false;         // the stream end (DSD silence) is queued
    std::atomic<bool> device_fail{false};
    int run_osr = 0;                 // dsd core: the DSD multiple the device runs
    // diagnostics, written by the DSP thread
    mutable std::mutex diag_m;
    std::string diag;
    std::vector<int> asio_rates;
    std::chrono::steady_clock::time_point diag_at{};
    void update_diag() {
        const auto now = std::chrono::steady_clock::now();
        if (now - diag_at < std::chrono::milliseconds(500)) return;
        diag_at = now;
        char b[512];
        const OutputStats* st = dev ? &dev->stats() : nullptr;
        std::snprintf(b, sizeof b,
                      "{\"ring\":%zu,\"ring_cap\":%zu,\"pend\":%zu,\"eof\":%d,\"fading\":%d,\"building\":%d,\"paused\":%d,"
                      "\"dev_paused\":%d,\"callbacks\":%lld,\"underruns\":%lld,\"drained\":%d,\"failed\":%d,\"late\":%lld,\"abs_out\":%lld,\"in\":%lld}",
                      ring ? ring->readable() : 0, ring ? ring->capacity() : 0, pend - pend_off, eof.load() ? 1 : 0, fading ? 1 : 0,
                      building.valid() ? 1 : 0, want_pause.load() ? 1 : 0, dev && dev->paused() ? 1 : 0,
                      st ? static_cast<long long>(st->callbacks.load()) : -1LL, st ? static_cast<long long>(st->underruns.load()) : -1LL,
                      dev && dev->drained() ? 1 : 0, dev && dev->failed() ? 1 : 0,
                      st ? static_cast<long long>(st->late_callbacks.load()) : -1LL, static_cast<long long>(abs_out), static_cast<long long>(in_total));
        std::lock_guard<std::mutex> l(diag_m);
        diag = b;
    }            // DSD -> PCM fallback at this rate (0 = 176.4 / 192 kHz)
    std::vector<uint8_t> dsd_bytes;
    bool flushed = false;
    std::atomic<bool> eof{false};
    size_t pend = 0, pend_off = 0;
    Fifo fc, fn, carry;
    bool fading = false;
    int64_t fade_pos = 0, fade_len = 0, drop_next = 0;
    int delay_shift = 0;
    // device restart: fade the last chunk out, the first frames after the restart in
    bool want_fade_in = false, fade_out_next = false;
    int64_t ramp_in_left = 0, ramp_in_len = 1, fade_out_len = 1;

    // background trim of the filter cache to its limit (filter_cache.h); a newer one supersedes it,
    // finished threads are joined at the end of playback
    struct PreJob {
        std::thread th;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    std::vector<PreJob> pre_jobs;
    void stop_cache_trim(bool join) {
        for (auto& j : pre_jobs) *j.cancel = true;
        if (!join) return;
        for (auto& j : pre_jobs)
            if (j.th.joinable()) j.th.join();
        pre_jobs.clear();
    }
    void kick_cache_trim() {
        stop_cache_trim(false);
        auto flag = std::make_shared<std::atomic<bool>>(false);
        PreJob j;
        j.cancel = flag;
        j.th = std::thread([cfg = s, flag] {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            if (!*flag) trim_filter_cache(cfg);
        });
        pre_jobs.push_back(std::move(j));
    }
    // the background rebuild can be cancelled when a newer change supersedes it
    std::shared_ptr<std::atomic<bool>> build_cancel;

    std::atomic<double> design_prog{0.0};
    std::chrono::steady_clock::time_point design_report{};

    void report_design(bool force = false) {
        if (!cb.on_design) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - design_report < std::chrono::milliseconds(250)) return;
        design_report = now;
        cb.on_design(design_prog.load());
    }

    // What the resampler is set to, for messages.
    std::string current_filter() const {
        // DSD file converted to PCM: the conversion, then the upsampling filter only when it is used
        std::string pre;
        if (rep.source.dsd_pcm) {
            const int dr = rep.source.dsd_rate;
            pre = "DSD" + std::to_string(dr > 0 ? dr / (dr % 44100 == 0 ? 44100 : 48000) : 0) + " -> PCM " +
                  std::to_string(rep.source.rate) + " Hz (low-pass " + std::to_string(s.output.dsd_pcm_lowpass) + " kHz" +
                  (s.output.dsd_pcm_limiter ? ", limiter" : "") + ")";
            if (s.core == CoreMode::Pcm && rep.device.rate == rep.source.rate && !s.filter.apodize) return pre;
            pre += ", then ";
        }
        std::string d = describe_filter(s);
        if (s.core == CoreMode::Pcm && rep.device.rate == rep.source.rate)
            d += s.filter.apodize ? " (output rate = source rate: 1x, the apodizing filter still applies)"
                                  : " (output rate = source rate: no resampling, the filter is not used)";
        else if (s.core == CoreMode::Pcm)
            d += ", " + std::to_string(rep.source.rate) + " -> " + std::to_string(rep.device.rate) + " Hz";
        else if (dsdenc)
            d += " | " + dsdenc->describe();
        return pre + d;
    }

    static std::string describe_filter(const Settings& x) {
        static const char* phases[] = {"linear", "intermediate", "minimum", "custom"};
        char b[96];
        std::snprintf(b, sizeof b, "%d taps, %s phase (alpha %.2f)", x.filter.taps, phases[static_cast<int>(x.filter.phase)],
                      x.filter.phase_alpha());
        std::string d = b;
        if (x.filter.apodize) d += ", apodize";
        if (x.filter.double_double) d += ", double-double convolution";
        if (x.core == CoreMode::Dsd)
            d = "DSD modulator order " + std::to_string(x.modulator.order) + ", DSD" + std::to_string(x.modulator.osr) + ", pre-filter " + d;
        return d;
    }

    static std::string join(const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) s += (s.empty() ? "" : " -> ") + x;
        return s.empty() ? "none" : s;
    }

    void event(const std::string& e) {
        if (cb.on_event) cb.on_event(e);
    }

    void hist_store(const double* x, int n) {
        for (int i = 0; i < n; ++i) {
            const int64_t f = in_total + i;
            std::copy(x + static_cast<size_t>(i) * ch, x + static_cast<size_t>(i + 1) * ch,
                      hist.begin() + static_cast<std::ptrdiff_t>((f % hist_cap) * ch));
        }
    }

    // Opens the device for the current settings (pcm: stepping the rate down by powers of two
    // when the device cannot open or start it), builds the chain, prefills and starts.
    bool open_and_start(int64_t from, std::string* error) {
        const AudioInfo& src = rep.source;
        // attempts: device rate, and for the dsd core the DSD multiple and transport
        struct Attempt {
            int rate, osr;
            bool native;
        };
        std::vector<Attempt> rates;
        if (s.core == CoreMode::Dsd) {
            if (out.api == OutputApi::WasapiShared) {
                if (error) *error = "DSD output (dsd core) needs WASAPI exclusive or ASIO: shared mode mixes everything to PCM";
                return false;
            }
            // ASIO: native DSD first (output.dsd auto), then DoP; a DSD rate the device refuses steps
            // down to DSD64. output.max_dsd caps the start: a driver may accept a rate its DAC cannot play
            int top = s.modulator.osr;
            if (s.output.max_dsd > 0 && top > s.output.max_dsd) {
                top = s.output.max_dsd;
                event("DSD" + std::to_string(s.modulator.osr) + " is above output.max_dsd: modulating to DSD" + std::to_string(top));
            }
            for (int osr = top; osr >= 64; osr /= 2) {
                if (out.api == OutputApi::Asio && s.output.dsd != DsdMode::Dop) rates.push_back({dsd_clock(osr), osr, true});
                if (s.output.dsd != DsdMode::Native) rates.push_back({dop_rate(dsd_clock(osr)), osr, false});
            }
            if (rates.empty()) {
                if (error) *error = "output.dsd = native needs an ASIO driver with a DSD mode";
                return false;
            }
        } else {
            rates.push_back({requested_rate(s, src.rate), 0, false});
            if (s.core == CoreMode::Pcm)
                for (int r = rates[0].rate / 2; r >= src.rate && r % src.rate == 0; r /= 2) rates.push_back({r, 0, false});
        }
        want = requested_rate(s, src.rate);
        for (size_t attempt = 0; attempt < rates.size(); ++attempt) {
            const bool last = attempt + 1 == rates.size();
            std::string e;
            auto fail_attempt = [&]() {
                dev.reset();
                if (last && error) *error = e;
                return last;
            };
            dec->seek(from);
            in_total = from;
            flushed = false;
            eof = false;
            pend = pend_off = 0;
            fading = false;
            next.reset();
            fc.clear(), fn.clear(), carry.clear();
            dev = open_output(out, rates[attempt].rate, src.channels, &e, rates[attempt].native);
            if (!dev) {
                if (fail_attempt()) return false;
                continue;
            }
            if (out.api == OutputApi::Asio) {
                // some drivers accept a rate they cannot stream: try it before designing the filter
                // (DoP: DoP silence, so the DAC does not see a stretch of PCM before the stream)
                ByteRing probe(1 << 12);
                std::atomic<bool> probe_eof{false};
                dev->set_dop(s.core == CoreMode::Dsd && !rates[attempt].native);
                if (!dev->start(&probe, &probe_eof, &e)) {
                    if (fail_attempt()) return false;
                    continue;
                }
                dev->stop();
            }
            rep.device = dev->format();
            rep.device_name = dev->device_name();
            rep.latency_ms = dev->latency_ms();
            if (auto* a = dynamic_cast<AsioOutput*>(dev.get())) {
                std::lock_guard<std::mutex> l(diag_m);
                if (!a->supported_rates().empty()) asio_rates = a->supported_rates();
            }
            chain_fmt = rep.device;
            dsdenc.reset();
            run_osr = 0;
            if (s.core == CoreMode::Dsd) {
                const int osr = rates[attempt].osr;
                run_osr = osr;
                const bool native = rates[attempt].native;
                if (rep.device.rate != (native ? dsd_clock(osr) : dop_rate(dsd_clock(osr)))) {
                    e = "the device locked " + std::to_string(rep.device.rate) + " Hz, not the DSD" + std::to_string(osr) + " rate";
                    if (fail_attempt()) return false;
                    continue;
                }
                chain_fmt.rate = modulator_input_rate(src.rate);
                chain_fmt.type = SampleType::Float64;
                chain_fmt.container_bits = chain_fmt.valid_bits = 64;
                dsdenc = std::make_unique<DsdEncoder>();
                if (!dsdenc->init(chain_fmt.rate, osr, s.modulator.order, rep.device, native ? DsdTransport::Native : DsdTransport::Dop, &e)) {
                    dsdenc.reset();
                    if (fail_attempt()) return false;
                    continue;
                }
            }
            crate = chain_fmt.rate;
            if (want_fade_in) ramp_in_left = ramp_in_len = std::max<int64_t>(1, crate / 50);   // 20 ms
            // a filter to design before the first sound: the pcm core's resampler or the modulator's pre-filter
            const bool pre = s.core == CoreMode::Dsd && chain_fmt.rate != src.rate;
            if ((s.core == CoreMode::Pcm && rep.device.rate != src.rate) || pre) {
                const int to = pre ? chain_fmt.rate : rep.device.rate;
                std::vector<double> h;
                const std::string key = filter_cache_key(s.filter, src.rate, to);
                if (!load_cached_filter(key, h))
                    event("designing the filter for " + std::to_string(src.rate) + " -> " + std::to_string(to) +
                          " Hz (long filters can take minutes; the result is cached)");
            }
            cur = std::make_unique<DspChain>();
            bool built = false;
            {
                const Settings cs = chain_settings(s, run_osr);
                DspChain* c = cur.get();
                const DeviceFormat devf = chain_fmt;
                std::string be;
                auto job = std::async(std::launch::async, [&, c, cs, devf] {
                    dsp::ProgressScope scope(&design_prog);
                    dsp::CancelScope cancel_scope(&cancel);
                    try {
                        return c->build(cs, src, devf, kChunk, &be);
                    } catch (const dsp::DesignCancelled&) {
                        be = "cancelled";
                        return false;
                    }
                });
                while (job.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) report_design();
                built = job.get();
                if (!built) e = be;
                report_design(true);
            }
            if (!built) {   // never play at the wrong rate
                if (fail_attempt()) return false;
                continue;
            }
            ratio = std::max(1, crate / src.rate);
            h_base = 0;
            dch = rep.device.channels;
            block = rep.device.block_align();
            abs_out = from * ratio;
            fc.ch = fn.ch = carry.ch = dch;
            ring = std::make_unique<ByteRing>(static_cast<size_t>(rep.device.frame_rate()) * block / 2);   // about 0.5 s
            dsp.assign(static_cast<size_t>(cur->max_output_frames()) * dch, 0.0);
            tmp_c = dsp;
            tmp_n = dsp;
            bytes.assign(dsdenc ? dsdenc->max_bytes(cur->max_output_frames()) : static_cast<size_t>(cur->max_output_frames()) * block, 0);
            // DSD: silence first, so the DAC has switched to DSD and locked before the music (output.dsd_lead_in_ms)
            if (dsdenc) push_dsd_idle(s.output.dsd_lead_in_ms);
            while (!cancel && ring->writable() >= bytes.size()) {   // prefill
                if (!produce()) {
                    eof = true;
                    break;
                }
                push();
            }
            dev->set_dop(dsdenc && dsdenc->dop());
            dev->set_paused(want_pause.load());
            if (!dev->start(ring.get(), &eof, &e)) {
                if (fail_attempt()) return false;
                continue;
            }
            break;
        }
        rep.chain = cur->stages();
        rep.filter = current_filter();
        rep.not_bit_perfect_reason.clear();
        if (src.dsd_pcm) rep.not_bit_perfect_reason = "DSD converted to PCM";
        else if (!src.lossless_int) rep.not_bit_perfect_reason = "source is not integer PCM";
        else if (!cur->transparent()) rep.not_bit_perfect_reason = "DSP active";
        else if (effective_bits(rep.device) < src.bits)
            rep.not_bit_perfect_reason =
                "device resolution " + std::to_string(effective_bits(rep.device)) + "-bit < source " + std::to_string(src.bits) + "-bit";
        rep.bit_perfect = rep.not_bit_perfect_reason.empty();
        if (cb.on_start) cb.on_start(rep);
        kick_cache_trim();
        return true;
    }

    // Mix what both chains have produced during a crossfade into dsp; returns frames.
    int mix() {
        const int n = std::min({fc.frames(), fn.frames(), static_cast<int>(dsp.size() / dch)});
        const double* a = fc.data();
        const double* b = fn.data();
        for (int i = 0; i < n; ++i) {
            const double t = std::min(1.0, (static_cast<double>(fade_pos + i) + 0.5) / static_cast<double>(fade_len));
            // raised cosine (gains sum to 1): the two chains carry the same, aligned content
            const double gb = 0.5 - 0.5 * std::cos(t * kPi), ga = 1.0 - gb;
            for (int c = 0; c < dch; ++c) {
                const size_t k = static_cast<size_t>(i) * dch + c;
                dsp[k] = a[k] * ga + b[k] * gb;
            }
        }
        fc.pop(n);
        fn.pop(n);
        fade_pos += n;
        return n;
    }

    void finish_fade() {
        // the next chain becomes current; what it has produced beyond the mixed part plays next
        abs_out += delay_shift;
        cur = std::move(next);
        carry.clear();
        if (fn.frames() > 0) carry.push(fn.data(), fn.frames());
        fc.clear(), fn.clear();
        fading = false;
        rep.chain = cur->stages();
        rep.filter = current_filter();
        event("now playing with filter: " + rep.filter + " | dsp: " + join(rep.chain));
        if (cb.on_changed) cb.on_changed(rep);
    }

    void push_next(const double* x, int n) {
        const int drop = static_cast<int>(std::min<int64_t>(drop_next, n));
        drop_next -= drop;
        fn.push(x + static_cast<size_t>(drop) * dch, n - drop);
    }

    // Starts the crossfade to nx: warm it up on the recent input and align it to the current
    // chain by content delay.
    void begin_crossfade(std::unique_ptr<DspChain> nx) {
        const int64_t have = std::min<int64_t>(in_total - hist_valid_from, hist_cap);
        const int64_t start = in_total - have;
        fn.clear();
        hist_tmp.resize(static_cast<size_t>(kChunk) * ch);
        std::vector<double> o(static_cast<size_t>(nx->max_output_frames()) * dch);
        const int dc = cur->content_delay(), dn = nx->content_delay();
        const int64_t target = abs_out - dc + dn;   // next-chain index of the content the current chain plays next
        drop_next = target - start * ratio;
        if (drop_next < 0) {
            fn.push_zeros(static_cast<int>(-drop_next));
            drop_next = 0;
        }
        for (int64_t f = start; f < in_total;) {
            const int n = static_cast<int>(std::min<int64_t>(kChunk, in_total - f));
            for (int i = 0; i < n; ++i) {
                const int64_t k = (f + i) % hist_cap;
                std::copy(hist.begin() + static_cast<std::ptrdiff_t>(k * ch), hist.begin() + static_cast<std::ptrdiff_t>((k + 1) * ch),
                          hist_tmp.begin() + static_cast<std::ptrdiff_t>(i) * ch);
            }
            const int got = nx->process(hist_tmp.data(), n, o.data());
            push_next(o.data(), got);
            f += n;
        }
        // frames of the current chain still queued for output count as already played
        fc.clear();
        if (carry.frames() > 0) {
            fc.push(carry.data(), carry.frames());
            carry.clear();
        }
        delay_shift = dn - dc;
        next = std::move(nx);
        tmp_n.assign(static_cast<size_t>(next->max_output_frames()) * dch, 0.0);
        tmp_c.assign(static_cast<size_t>(cur->max_output_frames()) * dch, 0.0);
        const size_t need = std::max(tmp_n.size(), tmp_c.size());
        if (dsp.size() < need) {
            dsp.resize(need);
            bytes.resize(dsdenc ? dsdenc->max_bytes(static_cast<int>(need / dch)) : need / dch * block);
        }
        fade_len = std::max<int64_t>(1, static_cast<int64_t>(crate * kFadeSeconds));
        fade_pos = 0;
        fading = true;
    }

    // One chunk into bytes; false at the end of the stream.
    bool produce() {
        if (pend_off < pend) return true;
        int n = 0;
        while (n == 0) {
            if (!fading && carry.frames() > 0) {
                n = std::min(carry.frames(), static_cast<int>(dsp.size() / dch));
                std::copy(carry.data(), carry.data() + static_cast<size_t>(n) * dch, dsp.begin());
                carry.pop(n);
                break;
            }
            const int got = dec->read(pcm.data(), kChunk);
            if (got > 0) {
                hist_store(pcm.data(), got);
                in_total += got;
                if (!fading) {
                    n = cur->process(pcm.data(), got, dsp.data());
                } else {
                    const int a = cur->process(pcm.data(), got, tmp_c.data());
                    fc.push(tmp_c.data(), a);
                    const int b = next->process(pcm.data(), got, tmp_n.data());
                    push_next(tmp_n.data(), b);
                    n = mix();
                    if (fade_pos >= fade_len) finish_fade();
                }
            } else {
                if (!flushed && try_gapless()) continue;   // the next track follows without a gap
                if (fading) finish_fade();
                if (carry.frames() > 0) continue;
                if (flushed) return false;
                flushed = true;
                n = cur->flush(dsp.data());
                if (n == 0) return false;
            }
        }
        if (ramp_in_left > 0) {
            for (int i = 0; i < n && ramp_in_left > 0; ++i, --ramp_in_left) {
                const double g = static_cast<double>(ramp_in_len - ramp_in_left) / static_cast<double>(ramp_in_len);
                for (int c = 0; c < dch; ++c) dsp[static_cast<size_t>(i) * dch + c] *= g;
            }
        }
        if (fade_out_next) {
            fade_out_next = false;
            for (int i = 0; i < n; ++i) {
                const double g = i < fade_out_len ? 1.0 - static_cast<double>(i) / static_cast<double>(fade_out_len) : 0.0;
                for (int c = 0; c < dch; ++c) dsp[static_cast<size_t>(i) * dch + c] *= g;
            }
        }
        if (dsdenc) {
            dsd_bytes.clear();
            dsdenc->encode(dsp.data(), n, dsd_bytes);
            if (bytes.size() < dsd_bytes.size()) bytes.resize(dsd_bytes.size());
            std::copy(dsd_bytes.begin(), dsd_bytes.end(), bytes.begin());
            pend = dsd_bytes.size();
        } else {
            rep.clipped += convert_to_device(dsp.data(), static_cast<size_t>(n) * dch, rep.device, bytes.data());
            pend = static_cast<size_t>(n) * block;
        }
        pend_off = 0;
        abs_out += n;
        rep.frames_played += n;
        return true;
    }

    // DSD only: queue about `ms` of DSD silence (keeps the DoP marker sequence going).
    void push_dsd_idle(int ms) {
        if (!dsdenc) return;
        dsd_bytes.clear();
        dsdenc->idle(rep.device.frame_rate() * ms / 1000, dsd_bytes);
        if (bytes.size() < dsd_bytes.size()) bytes.resize(dsd_bytes.size());
        std::copy(dsd_bytes.begin(), dsd_bytes.end(), bytes.begin());
        pend = dsd_bytes.size();
        pend_off = 0;
        while (!push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // DoP only: about `ms` of plain PCM zeros (no DoP markers) so the DAC leaves its DoP-detected DSD
    // mode before the stream stops; otherwise some DACs beep on one channel
    // when native DSD follows.
    void push_pcm_silence(int ms) {
        if (!dsdenc || !dsdenc->dop()) return;
        const size_t n = static_cast<size_t>(rep.device.frame_rate()) * ms / 1000 * block;
        if (bytes.size() < n) bytes.resize(n);
        std::fill(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n), 0);
        pend = n;
        pend_off = 0;
        while (!push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    bool push() {   // false when the ring is full
        while (pend_off < pend) {
            const size_t n = ring->write(bytes.data() + pend_off, pend - pend_off);
            pend_off += n;
            if (n == 0) return false;
        }
        return true;
    }

    // Chain frame being heard (the device's queue counted back from abs_out).
    int64_t heard_out() const {
        int64_t q = ring ? static_cast<int64_t>(ring->readable() / std::max(1, block)) + static_cast<int64_t>((pend - pend_off) / std::max(1, block)) : 0;
        if (rep.device.rate > 0 && crate != rep.device.frame_rate()) q = q * crate / rep.device.frame_rate();   // DoP frames -> chain frames
        return abs_out - q;
    }
    // Device frame of the newest segment's input frame 0 (the last pending cross switch, else the heard one).
    int64_t latest_base() const {
        for (auto it = switches.rbegin(); it != switches.rend(); ++it)
            if (it->cross) return it->at_out;
        return h_base;
    }
    bool cross_pending() const {
        for (const auto& w : switches)
            if (w.cross) return true;
        return false;
    }
    // Stream input frame being heard (the stream starts at the first track's frame 0; after a gapless switch to
    // another sample rate, at that track's frame 0).
    int64_t heard_input() const {
        const int64_t ho = heard_out();
        for (const auto& w : switches)   // still before a cross switch: that segment's parameters
            if (w.cross && ho < w.at_out + w.new_delay) return (ho - w.old_base - w.old_delay) / std::max(1, w.old_ratio);
        const int64_t d = dsdf ? 0 : cur ? cur->content_delay() : 0;
        return (ho - latest_base() - d) / std::max(1, ratio);
    }
    int64_t heard_position() const { return std::max<int64_t>(0, heard_input() - track_origin_in); }

    // gapless: report tracks that are now heard
    void check_switch() {
        while (!switches.empty()) {
            Switch& w = switches.front();
            if (w.cross) {
                if (heard_out() < w.at_out + w.new_delay) break;
                h_base = w.at_out;
                track_origin_in = 0;
                want = requested_rate(s, w.info.rate);
            } else {
                if (heard_input() < w.at_in) break;
                track_origin_in = w.at_in;
            }
            const bool cross = w.cross;
            rep.source = w.info;
            cur_path = w.path;
            switches.pop_front();
            if (cross) {
                rep.chain = cur->stages();
                rep.filter = current_filter();
            }
            event(cross ? "gapless: next track (another sample rate on the same output)" : "gapless: next track");
            if (cb.on_track_started) cb.on_track_started(rep);
        }
    }

    // The source of the track being decoded (the last switched-to one, or the heard one).
    const AudioInfo& decoding_info() const { return switches.empty() ? rep.source : switches.back().info; }

    std::string chain_key(const Settings& cs, const DeviceFormat& cf) const {
        return settings_to_json(cs).dump() + "|" + describe(cf);
    }

    // Opens the next track (set_next) on a background thread as soon as it is known; for a PCM track at another
    // sample rate whose chain would play on the same output, also builds that chain. The boundary uses what is
    // ready (try_gapless) and otherwise behaves as without it.
    void kick_prepare() {
        for (auto it = prep_threads.begin(); it != prep_threads.end();) {   // reap the finished ones
            if (it->first->done && it->first != prep) {
                it->second.join();
                it = prep_threads.erase(it);
            } else {
                ++it;
            }
        }
        std::wstring np;
        {
            std::lock_guard<std::mutex> l(m);
            np = next_path;
        }
        if (prep && prep->path == np) return;
        drop_prep();
        if (np.empty()) return;
        auto p = std::make_shared<Prep>();
        p->path = np;
        const bool dsd = dsdf != nullptr;
        const Settings cs = chain_settings(s, run_osr);
        const DeviceFormat cf = chain_fmt;
        const int cr = crate, cch = ch, cur_rate = decoding_info().rate;
        p->key = chain_key(cs, cf);
        std::thread worker([p, dsd, cs, cf, cr, cch, cur_rate] {
            std::string e;
            if (dsd) {
                auto f = std::make_unique<DsdFile>();
                if (f->open(p->path, &e)) p->dsdf = std::move(f);
                p->opened = true;
                p->done = true;
                return;
            }
            auto d = open_decoder(p->path, &e);
            const bool have = d != nullptr;
            const AudioInfo src = have ? d->info() : AudioInfo{};
            p->dec = std::move(d);   // try_gapless may take it as soon as opened is set
            p->opened = true;
            if (have && !p->cancel) {
                if (src.rate != cur_rate && src.channels == cch && same_output(cs, src.rate, cr)) {
                    try {
                        dsp::CancelScope cancel_scope(&p->cancel);
                        auto c = std::make_unique<DspChain>();
                        std::string be;
                        if (c->build(cs, src, cf, kChunk, &be)) p->chain = std::move(c);
                    } catch (...) {   // cancelled (or failed): no chain, the boundary is not gapless
                    }
                }
            }
            p->done = true;
        });
        prep_threads.emplace_back(p, std::move(worker));
        prep = p;
    }

    void drop_prep() {
        if (prep) prep->cancel = true;
        prep.reset();
    }

    // Gapless to another sample rate on the same output, joined as one filter would: zeros through the old chain
    // bring out A's last samples (its content delay) and their ringing; B's start through the new chain (built
    // ahead) is overlap-added so that B's first sample comes exactly one source period after A's last, its
    // pre-ringing over A's end. The hearing side follows when B is heard (check_switch).
    void cross_switch(std::unique_ptr<Decoder> nd, std::unique_ptr<DspChain> nc, const std::wstring& np) {
        const size_t need = static_cast<size_t>(std::max(cur->max_output_frames(), nc->max_output_frames())) * dch;
        if (dsp.size() < need) dsp.resize(need);
        if (tmp_c.size() < need) tmp_c.resize(need);
        if (tmp_n.size() < need) tmp_n.resize(need);
        const size_t nb = dsdenc ? dsdenc->max_bytes(static_cast<int>(need / dch)) : need / dch * block;
        if (bytes.size() < nb) bytes.resize(nb);

        const int64_t start = abs_out + carry.frames();   // device frame where the joined part begins
        const int64_t a_end = latest_base() + in_total * ratio + cur->content_delay();   // where A's frame K is heard
        std::vector<double> tail;
        auto put = [&](std::vector<double>& v, int n) { v.insert(v.end(), dsp.begin(), dsp.begin() + static_cast<std::ptrdiff_t>(n) * dch); };
        std::fill(pcm.begin(), pcm.end(), 0.0);
        const int64_t zeros = 2 * static_cast<int64_t>(cur->content_delay()) / std::max(1, ratio) + 1;
        for (int64_t z = 0; z < zeros;) {
            const int n = static_cast<int>(std::min<int64_t>(kChunk, zeros - z));
            put(tail, cur->process(pcm.data(), n, dsp.data()));
            z += n;
        }
        put(tail, cur->flush(dsp.data()));

        // B through the new chain until its first sample (new-chain frame dn) is out
        const int dn = nc->content_delay();
        in_total = 0;
        hist_valid_from = 0;
        std::vector<double> head;
        int64_t produced = 0;
        while (produced <= dn) {
            const int got = nd->read(pcm.data(), kChunk);
            if (got <= 0) break;
            hist_store(pcm.data(), got);
            in_total += got;
            const int o = nc->process(pcm.data(), got, dsp.data());
            put(head, o);
            produced += o;
        }
        // new-chain frame j lands on tail frame j + off; frames before what has been played already are dropped
        const int64_t off = (a_end - start) - dn;
        for (int64_t j = std::max<int64_t>(0, -off); j < produced; ++j) {
            const int64_t t = j + off;
            if (t >= static_cast<int64_t>(tail.size()) / dch) tail.resize(static_cast<size_t>(t + 1) * dch, 0.0);
            for (int c = 0; c < dch; ++c) tail[static_cast<size_t>(t) * dch + c] += head[static_cast<size_t>(j) * dch + c];
        }
        carry.push(tail.data(), static_cast<int>(tail.size() / dch));

        Switch w;
        w.info = nd->info();
        w.path = np;
        w.cross = true;
        w.at_out = a_end - dn;   // B's frame 0 is heard at at_out + new_delay = a_end
        w.new_delay = dn;
        w.old_ratio = ratio;
        w.old_delay = cur->content_delay();
        w.old_base = latest_base();
        w.old_chain = std::move(cur);
        switches.push_back(std::move(w));
        cur = std::move(nc);
        dec = std::move(nd);
        ratio = std::max(1, crate / dec->info().rate);
    }

    // The track just switched to (switches.back()) has its own headroom (ReplayGain per track): applied from its
    // first decoded sample on (the gain stage ramps over 20 ms); the old value is kept for an undo.
    void apply_headroom(std::optional<double> h) {
        if (!h || switches.empty() || *h == s.headroom_db) return;
        switches.back().old_headroom = s.headroom_db;
        Settings ns = s;
        ns.headroom_db = *h;
        if (cur->update(chain_settings(ns, run_osr))) {
            s = ns;
            return;
        }
        // the chain's structure changes (e.g. bit-perfect -> a quantizer): an ordinary settings change, a little late
        std::lock_guard<std::mutex> l(m);
        if (!pending) pending = std::make_pair(ns, out);
    }

    // A seek or restart before a pending switch is heard: the heard track's headroom back.
    void undo_headroom() {
        for (const auto& w : switches)
            if (w.old_headroom) {
                Settings ns = s;
                ns.headroom_db = *w.old_headroom;
                if (cur && cur->update(chain_settings(ns, run_osr))) s = ns;
                break;
            }
    }

    // A seek or restart before a pending cross switch is heard: back to the heard segment's chain.
    void undo_cross() {
        for (auto& w : switches)
            if (w.cross) {
                cur = std::move(w.old_chain);
                ratio = w.old_ratio;
                break;
            }
    }

    // gapless: continue with the next track when it has the same format (called at the end of
    // the decoder's data)
    bool try_gapless() {
        std::wstring np;
        std::optional<double> nh;
        {
            std::lock_guard<std::mutex> l(m);
            np = next_path;
            nh = next_headroom;
        }
        if (np.empty() || is_dsd_path(np) != (dsdf != nullptr)) return false;
        // opened ahead (kick_prepare): wait for the open if it is still running
        std::shared_ptr<Prep> p = prep && prep->path == np ? prep : nullptr;
        if (p) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!p->opened && !cancel && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!p->opened) p.reset();
        }
        drop_prep();   // consumed (or stale): a new next track starts a new one
        std::string e;
        if (dsdf) {
            std::unique_ptr<DsdFile> f = p ? std::move(p->dsdf) : nullptr;
            if (!f) {
                f = std::make_unique<DsdFile>();
                if (!f->open(np, &e)) return false;
            }
            if (f->info().dsd_rate != dsdf->info().dsd_rate || f->info().channels != dsdf->info().channels) return false;
            AudioInfo ai{};
            ai.rate = f->info().dsd_rate / 8;
            ai.channels = f->info().channels;
            ai.bits = 1;
            ai.lossless_int = true;
            ai.frames = f->info().frames;
            ai.channel_mask = f->info().channel_mask;
            ai.codec = f->info().codec;
            ai.dsd_rate = f->info().dsd_rate;
            Switch w;
            w.at_in = abs_out;
            w.info = ai;
            w.path = np;
            switches.push_back(std::move(w));
            dsdf = std::move(f);
        } else {
            std::unique_ptr<Decoder> nd = p ? std::move(p->dec) : nullptr;
            if (!nd) nd = open_decoder(np, &e);
            if (!nd || nd->info().channels != ch) return false;
            if (nd->info().rate == decoding_info().rate) {
                Switch w;
                w.at_in = in_total;
                w.info = nd->info();
                w.path = np;
                switches.push_back(std::move(w));
                dec = std::move(nd);
                apply_headroom(nh);
            } else {
                // another sample rate: gapless only on the same output, with the new chain built ahead for the
                // current settings; otherwise the track ends and the host loads the next one
                if (!p || !p->done || !p->chain || fading || building.valid() || p->key != chain_key(chain_settings(s, run_osr), chain_fmt))
                    return false;
                cross_switch(std::move(nd), std::move(p->chain), np);
                apply_headroom(nh);
            }
        }
        std::lock_guard<std::mutex> l(m);
        if (next_path == np) next_path.clear(), next_headroom.reset();
        return true;
    }

    // The end of the stream: DSD silence, then (DoP) PCM silence so the DAC leaves DSD mode;
    // queued without blocking, the loop pushes it.
    void queue_end() {
        end_queued = true;
        eof = true;
        if (!dsdenc) return;
        dsd_bytes.clear();
        dsdenc->idle(rep.device.frame_rate() / 10, dsd_bytes);
        if (dsdenc->dop()) dsd_bytes.resize(dsd_bytes.size() + static_cast<size_t>(rep.device.frame_rate()) / 5 * block, 0);
        if (bytes.size() < dsd_bytes.size()) bytes.resize(dsd_bytes.size());
        std::copy(dsd_bytes.begin(), dsd_bytes.end(), bytes.begin());
        pend = dsd_bytes.size();
        pend_off = 0;
    }

    // Seek with the device open: pause it (PCM fades out), drop what is queued, refill, resume.
    void do_seek(double sec) {
        if (!dev) return;
        dev->set_paused(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!dev->paused() && !dev->failed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ring->clear();
        pend = pend_off = 0;
        std::string e;
        check_switch();   // a track heard just now is the one the seek is in
        if (!switches.empty()) {
            // the decoder already moved on to a gapless track that is not heard yet: back to this one
            {
                std::lock_guard<std::mutex> l(m);
                if (next_path.empty()) next_path = switches.front().path;
            }
            undo_cross();
            undo_headroom();
            switches.clear();
            if (dsdf) {
                auto f = std::make_unique<DsdFile>();
                if (f->open(cur_path, &e)) dsdf = std::move(f);
            } else if (auto d = open_source(cur_path, s, &e)) {
                dec = std::move(d);
            }
        }
        int64_t from = static_cast<int64_t>(std::max(0.0, sec) * rep.source.rate);
        if (rep.source.frames > 0) from = std::min(from, rep.source.frames);
        if (dsdf) {
            dsdf->seek(from);
            abs_out = from;
        } else {
            dec->seek(from);
            in_total = from;
            hist_valid_from = from;
            flushed = false;
            carry.clear(), fc.clear(), fn.clear();
            if (fading) {   // a crossfade was running: the new chain takes over
                cur = std::move(next);
                fading = false;
                rep.chain = cur->stages();
                rep.filter = current_filter();
            }
            cur->reset();
            abs_out = from * ratio;
        }
        h_base = 0;
        track_origin_in = 0;
        eof = false;
        end_queued = false;
        dev->rearm();
        while (!cancel && ring->writable() >= bytes.size()) {   // refill
            if (!(dsdf ? produce_dsd() : produce())) {
                if (!try_gapless()) {
                    queue_end();
                    break;
                }
                continue;
            }
            push();
        }
        push();
        dev->set_paused(want_pause.load());
        pos = heard_position();
        event("seek to " + std::to_string(sec) + " s");
    }

    // Before the device closes for a restart / at the end: what is queued plays out, or, when
    // paused, is dropped (nothing would consume it).
    void drop_if_paused() {
        if (dev && dev->paused()) {
            ring->clear();
            pend = pend_off = 0;
            dev->set_paused(false);
        }
    }

    // One pass of the transport requests (DSP thread).
    void service_transport() {
        std::optional<double> sk;
        {
            std::lock_guard<std::mutex> l(m);
            sk.swap(seek_req);
        }
        if (sk) do_seek(*sk);
        if (dev) {
            dev->set_paused(want_pause.load());
            double l = 0, r = 0;
            dev->meter(l, r);
            if (l > meter_l.load()) meter_l = l;
            if (r > meter_r.load()) meter_r = r;
        }
        check_switch();
        update_diag();
    }

    void launch_build(const Settings& ns, const OutputChoice& no) {
        build_target = std::make_pair(ns, no);
        const Settings cs = chain_settings(ns, run_osr);
        const AudioInfo src = rep.source;
        design_prog = 0.0;
        build_cancel = std::make_shared<std::atomic<bool>>(false);
        building = std::async(std::launch::async, [cs, src, devf = chain_fmt, this, flag = build_cancel]() -> std::unique_ptr<DspChain> {
            // playback is running: stay below it (the design workers take this priority)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            dsp::ProgressScope scope(&design_prog);
            dsp::CancelScope cancel_scope(flag.get());
            auto c = std::make_unique<DspChain>();
            std::string e;
            try {
                if (!c->build(cs, src, devf, kChunk, &e)) return nullptr;
            } catch (const dsp::DesignCancelled&) {
                return nullptr;
            }
            return c;
        });
        event("designing " + describe_filter(ns) + " in the background (playback continues)");
    }

    // The PCM decoder for a file: DSD files reaching this point are converted to PCM.
    std::unique_ptr<Decoder> open_source(const std::wstring& file, const Settings& st, std::string* e) {
        if (!is_dsd_path(file)) return open_decoder(file, e);
        auto d = make_dsd_pcm_decoder(dsd_pcm_rate, st.output.dsd_pcm_lowpass);
        if (!d->open(file, e)) d.reset();
        return d;
    }

    // Settings the decoder is built from.
    bool decoder_changes(const Settings& ns) const {
        return is_dsd_path(cur_path) && ns.output.dsd_pcm_lowpass != s.output.dsd_pcm_lowpass;
    }

    bool needs_restart(const Settings& ns, const OutputChoice& no) const {
        return !(no == out) || ns.core != s.core || requested_rate(ns, rep.source.rate) != want || modulator_key(ns) != modulator_key(s) ||
               decoder_changes(ns);
    }

    // drain = false: the device is reopened as it is (a driver reset): what is queued is dropped.
    bool restart(const Settings& ns, const OutputChoice& no, bool drain = true) {
        check_switch();
        if (cross_pending()) drain = false;   // the queue holds the next track at another rate: not played out
        int64_t from = dsdf ? heard_position() : std::max<int64_t>(0, heard_input());   // PCM: stream timeline
        drop_if_paused();
        drop_prep();   // built for the old settings / device
        if (dev && !eof && !dev->failed() && drain) {
            // play what is queued, fade the next chunk out, let the device drain, resume there
            while (!push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            from = std::max<int64_t>(0, (abs_out - (cur ? cur->content_delay() : 0)) / std::max(1, ratio));
            fade_out_len = std::max<int64_t>(1, crate / 50);   // 20 ms
            fade_out_next = true;
            if (produce())
                while (!push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            push_dsd_idle(50);
            push_pcm_silence(200);
            eof = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!dev->drained() && !dev->failed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        want_fade_in = true;
        // `from` is on the stream timeline. With gapless tracks the decoder may have moved on: resume in
        // the track that holds `from`, at its own frame (a later track that was decoded but not reached
        // goes back to being the next track)
        bool reopen_dec = decoder_changes(ns);
        if (!dsdf) {
            while (!switches.empty() && !switches.front().cross && from >= switches.front().at_in) {
                track_origin_in = switches.front().at_in;
                rep.source = switches.front().info;
                cur_path = switches.front().path;
                switches.pop_front();
                if (cb.on_track_started) cb.on_track_started(rep);
            }
            if (!switches.empty()) {
                std::lock_guard<std::mutex> l(m);
                if (next_path.empty()) next_path = switches.front().path;
                undo_cross();
                undo_headroom();
                switches.clear();
                reopen_dec = true;
            }
            from = std::max<int64_t>(0, from - track_origin_in);
            track_origin_in = 0;
        }
        if (dev) dev->stop();
        dev.reset();
        s = ns;
        out = no;
        if (reopen_dec) {
            std::string e;
            auto d = open_source(cur_path, s, &e);
            if (!d) {
                err = e;
                return false;
            }
            dec = std::move(d);
        }
        event(drain ? "restarting the device for the new core / rate / output" : "reopening the device");
        std::string e;
        if (!open_and_start(from, &e)) {
            err = e;
            return false;
        }
        return true;
    }

    // Applies a pending settings change; false on a fatal error.
    bool handle(const Settings& ns, const OutputChoice& no) {
        drop_prep();   // a chain prepared for the next track was built for the old settings
        if (fading || building.valid() || cross_pending()) {   // one change at a time (and not across a rate switch); the latest waits
            queued = std::make_pair(ns, no);
            if (building.valid() && build_cancel) *build_cancel = true;   // superseded: stop designing it
            return true;
        }
        dsp::set_design_threads(ns.design.threads);
        if (needs_restart(ns, no)) return restart(ns, no);
        if (cur->update(chain_settings(ns, run_osr))) {
            s = ns;
            event("applied in place (EQ / crossfeed / volume)");
            // the stages that are active now (EQ switched on / off, volume at 0 dB ...)
            const auto stages = cur->stages();
            if (stages != rep.chain) {
                rep.chain = stages;
                if (cb.on_changed) cb.on_changed(rep);
            }
            return true;
        }
        launch_build(ns, no);
        return true;
    }

    bool poll_build() {
        if (!building.valid()) return true;
        if (building.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            report_design();
            return true;
        }
        report_design(true);
        auto nx = building.get();
        auto target = *build_target;
        build_target.reset();
        if (queued) {   // superseded while designing
            auto q = *queued;
            queued.reset();
            return handle(q.first, q.second);
        }
        if (!nx) {
            event("the new settings could not be built; keeping the current chain");
            return true;
        }
        s = target.first;
        begin_crossfade(std::move(nx));
        kick_cache_trim();
        return true;
    }

    // ---- DSD files: straight to the device, every processing stage bypassed.
    // ASIO with output.dsd none / native: native DSD; otherwise DoP at the DSD rate / 16.
    bool open_dsd(int64_t from, std::string* error) {
        const DsdInfo& di = dsdf->info();
        struct Attempt {
            int rate;
            bool native;
        };
        std::vector<Attempt> at;
        const int multiple = di.dsd_rate / (di.dsd_rate % 44100 == 0 ? 44100 : 48000);
        if (s.output.max_dsd > 0 && multiple > s.output.max_dsd) {
            if (error) *error = "DSD" + std::to_string(multiple) + " is above output.max_dsd (DSD" + std::to_string(s.output.max_dsd) + ")";
            return false;
        }
        if (out.api == OutputApi::Asio && s.output.dsd != DsdMode::Dop) at.push_back({di.dsd_rate, true});
        if (s.output.dsd != DsdMode::Native) at.push_back({di.dsd_rate / 16, false});
        std::string e = at.empty() ? "output.dsd = native needs an ASIO driver with a DSD mode" : "";
        for (const auto& a : at) {
            dev = open_output(out, a.rate, di.channels, &e, a.native);
            if (!dev) continue;
            if (dev->format().rate != a.rate || dev->format().channels != di.channels) {
                e = "the device locked " + describe(dev->format()) + " instead of " + std::to_string(a.rate) + " Hz, " +
                    std::to_string(di.channels) + " ch";
                dev.reset();
                continue;
            }
            dsdenc = std::make_unique<DsdEncoder>();
            if (!dsdenc->init_direct(di.dsd_rate, dev->format(), a.native ? DsdTransport::Native : DsdTransport::Dop, &e)) {
                dev.reset();
                continue;
            }
            rep.device = dev->format();
            rep.device_name = dev->device_name();
            rep.latency_ms = dev->latency_ms();
            crate = di.dsd_rate / 8;
            ratio = 1;
            dch = rep.device.channels;
            block = rep.device.block_align();
            dsdf->seek(from);
            abs_out = from;
            eof = false;
            pend = pend_off = 0;
            ring = std::make_unique<ByteRing>(static_cast<size_t>(rep.device.frame_rate()) * block / 2);
            dsd_in.assign(static_cast<size_t>(kChunk) * 4 * di.channels, 0);
            bytes.assign(static_cast<size_t>(kChunk) * 4 * block, 0);
            // silence first, so the DAC has switched to DSD and locked before the music (output.dsd_lead_in_ms;
            // at least 30 ms after a restart)
            push_dsd_idle(std::max(s.output.dsd_lead_in_ms, want_fade_in ? 30 : 0));
            while (!cancel && ring->writable() >= bytes.size()) {
                if (!produce_dsd()) {
                    eof = true;
                    break;
                }
                push();
            }
            dev->set_dop(dsdenc->dop());
            dev->set_paused(want_pause.load());
            if (!dev->start(ring.get(), &eof, &e)) {
                dev.reset();
                continue;
            }
            rep.chain.clear();
            rep.filter = "none (DSD file sent directly) | " + dsdenc->describe();
            rep.bit_perfect = true;
            rep.not_bit_perfect_reason.clear();
            return true;
        }
        if (error) *error = e;
        return false;
    }

    bool produce_dsd() {
        if (pend_off < pend) return true;
        const int n = dsdf->read(dsd_in.data(), kChunk * 4);
        if (n <= 0) return false;
        dsd_bytes.clear();
        dsdenc->encode_dsd(dsd_in.data(), n, dsd_bytes);
        if (bytes.size() < dsd_bytes.size()) bytes.resize(dsd_bytes.size());
        std::copy(dsd_bytes.begin(), dsd_bytes.end(), bytes.begin());
        pend = dsd_bytes.size();
        pend_off = 0;
        abs_out += n;
        rep.frames_played += n;
        return true;
    }

    // DSD direct: only the output can change (no processing applies to DSD sent directly).
    // reset: a driver reset reopens the device as it is (nothing more is queued for it).
    bool handle_dsd(const Settings& ns, const OutputChoice& no, bool reset = false) {
        const bool reopen = reset || !(no == out) || ns.output.dsd != s.output.dsd;
        s = ns;
        if (!reopen) {
            event("DSD file played directly: DSP settings do not apply");
            return true;
        }
        const int64_t from = heard_position();
        drop_if_paused();
        if (!reset) {
            push_dsd_idle(50);
            push_pcm_silence(200);
            eof = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (dev && !dev->drained() && !dev->failed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (dev) dev->stop();
        dev.reset();
        out = no;
        want_fade_in = true;
        event(reset ? "reopening the device" : "restarting the device for the new output");
        std::string e;
        if (!open_dsd(from, &e)) {
            err = e;
            return false;
        }
        if (cb.on_start) cb.on_start(rep);
        return true;
    }

    void run() {
        RealtimeScope rt;
        dsp::set_design_threads(s.design.threads);
        std::string e;
        bool started_dsd = false;
        if (is_dsd_path(path)) {
            auto f = std::make_unique<DsdFile>();
            if (!f->open(path, &e)) {
                std::lock_guard<std::mutex> l(m);
                start_done = true;
                start_ok = false;
                start_err = e;
                cv.notify_all();
                done = true;
                return;
            }
            dsdf = std::move(f);
            const DsdInfo& di = dsdf->info();
            rep.source = AudioInfo{};
            rep.source.rate = di.dsd_rate / 8;
            rep.source.channels = di.channels;
            rep.source.bits = 1;
            rep.source.lossless_int = true;
            rep.source.frames = di.frames;
            rep.source.channel_mask = di.channel_mask;
            rep.source.codec = di.codec;
            rep.source.dsd_rate = di.dsd_rate;
            std::string de = "output.dsd = pcm";
            if (s.output.dsd != DsdMode::Pcm && open_dsd(static_cast<int64_t>(start_sec * rep.source.rate), &de)) {
                started_dsd = true;
                if (cb.on_start) cb.on_start(rep);
            } else {
                // no DSD path to the device: the DSD file is converted to PCM
                event(s.output.dsd == DsdMode::Pcm ? "converting the DSD file to PCM (output.dsd = pcm)"
                                                    : "DSD cannot be sent directly (" + de + "): converting the DSD file to PCM");
                // shared mode runs at the mixer rate: convert to it when it divides the DSD rate
                if (out.api == OutputApi::WasapiShared) {
                    std::string pe;
                    if (auto probe = open_output(out, dsdf->info().dsd_rate / 16, dsdf->info().channels, &pe)) {
                        const int mix = probe->format().rate;
                        if (mix > 0 && dsdf->info().dsd_rate % (mix * 8) == 0) dsd_pcm_rate = mix;
                    }
                }
                dsdf.reset();
                dsdenc.reset();
            }
        }
        if (started_dsd) {
            {
                std::lock_guard<std::mutex> l(m);
                start_done = true;
                start_ok = true;
            }
            cv.notify_all();
            ok = true;
            cur_path = path;
            while (!cancel) {
                std::optional<std::pair<Settings, OutputChoice>> p;
                {
                    std::lock_guard<std::mutex> l(m);
                    p.swap(pending);
                }
                if (p && !handle_dsd(p->first, p->second)) {
                    ok = false;
                    break;
                }
                kick_prepare();
                service_transport();
                if (!eof) {
                    if (!produce_dsd() && !try_gapless()) queue_end();
                }
                if (!push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                else if (eof && pend_off >= pend) {
                    if (dev->drained()) {
                        ended_flag = true;
                        if (cb.on_ended) cb.on_ended();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                pos = heard_position();
                pos_sec = rep.source.rate > 0 ? static_cast<double>(pos) / rep.source.rate : 0.0;
                if (cb.on_progress) cb.on_progress(pos, rep.source.frames);
                if (dev->failed()) break;
                if (!service_device()) {
                    ok = false;
                    break;
                }
            }
            finish_playback();
            return;
        }
        dec = open_source(path, s, &e);
        if (dec) {
            rep.source = dec->info();
            ch = rep.source.channels;
            pcm.assign(static_cast<size_t>(kChunk) * ch, 0.0);
            hist.assign(static_cast<size_t>(hist_cap) * ch, 0.0);
        }
        const bool started = dec && open_and_start(static_cast<int64_t>(start_sec * rep.source.rate), &e);
        if (dec && !started && !cancel) device_fail = true;   // the file decoded: the device / chain failed
        {
            std::lock_guard<std::mutex> l(m);
            start_done = true;
            start_ok = started;
            start_err = e;
        }
        cv.notify_all();
        if (!started) {
            stop_cache_trim(true);
            done = true;
            return;
        }
        ok = true;
        cur_path = path;
        while (!cancel) {
            std::optional<std::pair<Settings, OutputChoice>> p;
            {
                std::lock_guard<std::mutex> l(m);
                p.swap(pending);
            }
            if (p && !handle(p->first, p->second)) {
                ok = false;
                break;
            }
            if (!poll_build()) {
                ok = false;
                break;
            }
            if (!fading && !building.valid() && !cross_pending() && queued) {
                auto q = *queued;
                queued.reset();
                if (!handle(q.first, q.second)) {
                    ok = false;
                    break;
                }
            }
            kick_prepare();
            service_transport();
            if (!eof && !produce()) queue_end();
            if (!push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            else if (eof && pend_off >= pend) {
                if (dev->drained()) {
                    ended_flag = true;
                    if (cb.on_ended) cb.on_ended();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            pos = heard_position();
            pos_sec = rep.source.rate > 0 ? static_cast<double>(pos) / rep.source.rate : 0.0;
            if (cb.on_progress) cb.on_progress(pos, rep.source.frames);
            if (dev->failed()) break;
            if (!service_device()) {
                ok = false;
                break;
            }
        }
        finish_playback();
    }

    // The device's notes go to the host log (driver messages, late callbacks); a driver that asks
    // to be opened again gets reopened, resuming where it was, at most 3 times in 10 s.
    const AudioOutput* late_dev = nullptr;
    int64_t late_seen = 0;
    std::chrono::steady_clock::time_point late_at{};
    int64_t rate_cb = -1;            // callbacks at the start of the rate window
    std::chrono::steady_clock::time_point rate_at{};
    std::deque<std::chrono::steady_clock::time_point> resets;
    bool service_device() {
        for (const auto& n : dev->take_notes()) event(n);
        const auto now = std::chrono::steady_clock::now();
        const OutputStats& st = dev->stats();
        if (late_dev != dev.get()) {
            late_dev = dev.get();
            late_seen = 0;
            rate_cb = -1;
        }
        // callbacks much faster than the buffer size allows: the driver changed its buffer behind us
        const int64_t cbs = st.callbacks.load();
        if (rate_cb < 0 || cbs < rate_cb) {
            rate_cb = cbs;
            rate_at = now;
        } else if (now - rate_at >= std::chrono::seconds(1)) {
            const double sec = std::chrono::duration<double>(now - rate_at).count();
            const double got = (cbs - rate_cb) / sec;
            const double expect = dev->buffer_frames() > 0 ? static_cast<double>(rep.device.frame_rate()) / dev->buffer_frames() : 0;
            if (expect > 0 && got > expect * 1.5) {
                char b[160];
                std::snprintf(b, sizeof b, "device: %.0f callbacks/s, expected %.0f (buffer %d frames)", got, expect, dev->buffer_frames());
                event(b);
            }
            rate_cb = cbs;
            rate_at = now;
        }
        const int64_t late = st.late_callbacks.load();
        if (late != late_seen && now - late_at >= std::chrono::seconds(1)) {
            char b[200];
            std::snprintf(b, sizeof b, "device: %lld late callbacks (total %lld, longest %.1f ms, buffer %.1f ms, underruns %lld)",
                          static_cast<long long>(late - late_seen), static_cast<long long>(late), st.max_interval_us.load() / 1000.0,
                          rep.device.frame_rate() > 0 ? dev->buffer_frames() * 1000.0 / rep.device.frame_rate() : 0.0,
                          static_cast<long long>(st.underruns.load()));
            event(b);
            late_seen = late;
            late_at = now;
        }
        if (!dev->reset_requested()) return true;
        while (!resets.empty() && now - resets.front() > std::chrono::seconds(10)) resets.pop_front();
        if (resets.size() >= 3) {
            err = "the output driver keeps asking to be reset";
            return false;
        }
        resets.push_back(now);
        return dsdf ? handle_dsd(s, out, true) : restart(s, out, false);
    }

    void finish_playback() {
        drop_prep();
        for (auto& [pp, worker] : prep_threads) {
            pp->cancel = true;
            worker.join();
        }
        prep_threads.clear();
        if (dev && !dev->failed() && dsdenc && !ended_flag) {
            // stopped (or failed) before the end: stop quickly, but on silence. DoP: PCM silence (the DAC
            // leaves DSD mode). Native DSD: DSD silence (0x69, what the feeder writes once the ring is
            // empty) reaches the DAC before the stream stops, not a cut in the middle of the music
            if (dev->paused() || cancel) {
                ring->clear();
                dev->set_paused(false);
            }
            pend = pend_off = 0;
            push_pcm_silence(200);
            eof = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!dev->drained() && !dev->failed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!dsdenc->dop() && dev->drained() && rep.device.frame_rate() > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(dev->latency_ms()) + 2 * dev->buffer_frames() * 1000 / rep.device.frame_rate() + 5));
        }
        if (ok && dev && ended_flag) {
            // the device buffer plays out (the ring has drained)
            if (!cancel)
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(dev->latency_ms()) +
                                                                      2 * dev->buffer_frames() * 1000 / rep.device.frame_rate() + 20));
        }
        if (build_cancel) *build_cancel = true;
        if (building.valid()) building.wait();
        stop_cache_trim(true);
        if (dev) {
            dev->stop();
            rep.underruns = dev->stats().underruns;
            rep.event_timeouts = dev->stats().event_timeouts;
            rep.max_interval_us = dev->stats().max_interval_us;
            if (dev->failed()) {
                ok = false;
                err = dev->failure();
            }
            dev.reset();
        }
        if (!ok && !cancel && cb.on_failed) cb.on_failed(err);
        done = true;
    }
};

Player::Player() : impl_(std::make_unique<Impl>()) {}

Player::~Player() {
    stop();
    if (impl_->th.joinable()) impl_->th.join();
}

bool Player::start(const std::wstring& path, const Settings& s, const OutputChoice& out, PlayCallbacks cb, std::string* error,
                   double start_seconds) {
    Impl& p = *impl_;
    p.path = path;
    p.start_sec = std::max(0.0, start_seconds);
    p.s = s;
    p.out = out;
    p.cb = std::move(cb);
    p.th = std::thread([&p] { p.run(); });
    std::unique_lock<std::mutex> l(p.m);
    p.cv.wait(l, [&p] { return p.start_done; });
    if (!p.start_ok) {
        l.unlock();
        p.th.join();
        if (error) *error = p.start_err;
        return false;
    }
    return true;
}

void Player::apply(const Settings& s, const OutputChoice& out) {
    std::lock_guard<std::mutex> l(impl_->m);
    impl_->pending = std::make_pair(s, out);
}

void Player::stop() { impl_->cancel = true; }

bool Player::finished() const { return impl_->done; }

int64_t Player::position() const { return impl_->pos; }
double Player::position_seconds() const { return impl_->pos_sec; }

void Player::pause(bool paused) { impl_->want_pause = paused; }
bool Player::paused() const { return impl_->want_pause; }

void Player::seek(double seconds) {
    std::lock_guard<std::mutex> l(impl_->m);
    impl_->seek_req = seconds;
}

void Player::set_next(const std::wstring& path, std::optional<double> headroom_db) {
    std::lock_guard<std::mutex> l(impl_->m);
    impl_->next_path = path;
    impl_->next_headroom = headroom_db;
}

bool Player::ended() const { return impl_->ended_flag; }
bool Player::last_failure_was_device() const { return impl_->device_fail; }

std::vector<int> Player::asio_rates() const {
    std::lock_guard<std::mutex> l(impl_->diag_m);
    return impl_->asio_rates;
}

std::string Player::debug_json() const {
    std::lock_guard<std::mutex> l(impl_->diag_m);
    return impl_->diag.empty() ? "{}" : impl_->diag;
}

void Player::meter(double& l, double& r) {
    l = impl_->meter_l.exchange(0.0);
    r = impl_->meter_r.exchange(0.0);
}

bool Player::wait(PlayReport* report, std::string* error) {
    if (impl_->th.joinable()) impl_->th.join();
    if (report) *report = impl_->rep;
    if (!impl_->ok && error) *error = impl_->err;
    return impl_->ok;
}

bool play_file(const std::wstring& path, const Settings& s, const OutputChoice& out, const std::atomic<bool>& cancel,
               const PlayCallbacks& cb, PlayReport* report, std::string* error) {
    Player p;
    if (!p.start(path, s, out, cb, error)) return false;
    while (!p.finished()) {
        if (cancel) p.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return p.wait(report, error);
}

}  // namespace daphne
