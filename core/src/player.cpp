#include "daphne/engine/player.h"

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
#include "daphne/dsp/quantizer.h"
#include "daphne/engine/ring_buffer.h"
#include "daphne/platform/platform.h"
#include "player/change_plan.h"
#include "player/common.h"
#include "player/crossfader.h"
#include "player/output_session.h"
#include "player/report.h"
#include "player/timeline.h"
#include "player/track_queue.h"

namespace daphne {

using namespace playback;

struct Player::Impl {
    // setup
    PlayerDeps deps;
    std::filesystem::path path;
    Settings s;
    OutputChoice out;
    Report report;
    PlayReport& rep = report.rep;      // the report's parts, used throughout
    PlayCallbacks& cb = report.cb;
    Timeline tl;
    OutputSession io;                  // the device, its ring and the chunk being pushed
    TrackQueue queue;                  // the next track (set_next) and its preparation

    // threading
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool start_done = false, start_ok = false;
    std::string start_err;
    std::optional<std::pair<Settings, OutputChoice>> pending;
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<int64_t> pos{0};
    bool ok = false;
    std::string err;

    // playback (DSP thread only)
    std::unique_ptr<Decoder> dec;
    std::unique_ptr<DspChain> cur;
    std::future<std::unique_ptr<DspChain>> building;
    std::optional<std::pair<Settings, OutputChoice>> build_target, queued;
    std::vector<double> pcm, dsp, tmp_c, tmp_n;
    int64_t in_total = 0;    // source frames read since the start of the track
    int ratio = 1, ch = 0, dch = 0, want = 0;
    // chain output rate / format (= the device's, except for the dsd core: the modulator's PCM)
    int crate = 0;
    DeviceFormat chain_fmt{};
    std::unique_ptr<DsdEncoder> dsdenc;
    std::unique_ptr<DsdFile> dsdf;   // DSD file played directly (no chain)
    std::vector<uint8_t> dsd_in;
    int dsd_pcm_rate = 0;            // DSD -> PCM fallback at this rate (0 = 176.4 / 192 kHz)
    double start_sec = 0.0;          // Player::start position
    // transport requests (under m) and state
    std::atomic<bool> want_pause{false}, ended_flag{false};
    std::optional<double> seek_req;
    std::filesystem::path cur_path;    // the track being heard
    std::atomic<double> pos_sec{0.0};
    std::atomic<double> meter_l{0.0}, meter_r{0.0};
    bool end_queued = false;         // the stream end (DSD silence) is queued
    std::atomic<bool> device_fail{false};
    int run_osr = 0;                 // dsd core: the DSD multiple the device runs
    // diagnostics for the host's log (Report::diag_json)
    void update_diag() {
        if (!report.diag_due()) return;
        char b[512];
        const OutputStats* st = io.dev ? &io.dev->stats() : nullptr;
        std::snprintf(b, sizeof b,
                      "{\"ring\":%zu,\"ring_cap\":%zu,\"pend\":%zu,\"eof\":%d,\"fading\":%d,\"building\":%d,\"paused\":%d,"
                      "\"dev_paused\":%d,\"callbacks\":%lld,\"underruns\":%lld,\"drained\":%d,\"failed\":%d,\"late\":%lld,\"tl.abs_out\":%lld,\"in\":%lld}",
                      io.ring ? io.ring->readable() : 0, io.ring ? io.ring->capacity() : 0, io.pend - io.pend_off, io.eof.load() ? 1 : 0, xf.fading ? 1 : 0,
                      building.valid() ? 1 : 0, want_pause.load() ? 1 : 0, io.dev && io.dev->paused() ? 1 : 0,
                      st ? static_cast<long long>(st->callbacks.load()) : -1LL, st ? static_cast<long long>(st->underruns.load()) : -1LL,
                      io.dev && io.dev->drained() ? 1 : 0, io.dev && io.dev->failed() ? 1 : 0,
                      st ? static_cast<long long>(st->late_callbacks.load()) : -1LL, static_cast<long long>(tl.abs_out), static_cast<long long>(in_total));
        report.set_diag(b);
    }
    std::vector<uint8_t> dsd_bytes;
    bool flushed = false;
    Crossfader xf;                   // a rebuilt chain fading in; the chain output carried over
    History hist;                    // recent input, for a rebuilt chain's warm-up
    FadeQuantizer fq;                // fades on integer devices (N21)
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
            platform::set_thread_priority(platform::Priority::Background);
            if (!*flag) trim_filter_cache(cfg);
        });
        pre_jobs.push_back(std::move(j));
    }
    // the background rebuild can be cancelled when a newer change supersedes it
    std::shared_ptr<std::atomic<bool>> build_cancel;
    bool build_designs = false;   // the background build designs a filter (not only loads it from the cache)


    std::unique_ptr<AudioOutput> open_output(const OutputRequest& req, std::string* error) { return deps.open_output(req, error); }


    void event(const std::string& e) const { report.event(e); }


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
            if (api_is_shared(out.api)) {
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
                if (api_has_native_dsd(out.api) && s.output.dsd != DsdMode::Dop) rates.push_back({dsd_clock(osr), osr, true});
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
                io.dev.reset();
                if (last && error) *error = e;
                return last;
            };
            dec->seek(from);
            in_total = from;
            flushed = false;
            io.eof = false;
            io.pend = io.pend_off = 0;
            xf.fading = false;
            xf.next.reset();
            xf.clear();
            io.dev = open_output({out, s.output.buffer_ms, rates[attempt].rate, src.channels, rates[attempt].native}, &e);
            if (!io.dev) {
                if (fail_attempt()) return false;
                continue;
            }
            if (io.dev->probe_before_design()) {
                // some drivers accept a rate they cannot stream: try it before designing the filter
                // (DoP: DoP silence, so the DAC does not see a stretch of PCM before the stream)
                ByteRing probe(1 << 12);
                std::atomic<bool> probe_eof{false};
                io.dev->set_dop(s.core == CoreMode::Dsd && !rates[attempt].native);
                if (!io.dev->start(&probe, &probe_eof, &e)) {
                    if (fail_attempt()) return false;
                    continue;
                }
                io.dev->stop();
            }
            rep.device = io.dev->format();
            rep.device_name = io.dev->device_name();
            rep.latency_ms = io.dev->latency_ms();
            io.note_buffer_fallback(report, s.output.buffer_ms);
            if (auto rates_ok = io.dev->supported_rates(); !rates_ok.empty()) report.set_asio_rates(std::move(rates_ok));
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
            // (the chain's condition). Only a real design reports its progress: a cached filter can take a moment to
            // load, and the host would show a design popup for it (with the text of the last design).
            bool designing = false;
            if (chain_fmt.rate != src.rate || s.filter.apodize) {
                const int to = chain_fmt.rate;
                designing = !filter_cached(s.filter, src.rate, to);
                if (designing)
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
                    dsp::ProgressScope scope(&report.design_prog);
                    dsp::CancelScope cancel_scope(&cancel);
                    try {
                        return c->build(cs, src, devf, kChunk, &be);
                    } catch (const dsp::DesignCancelled&) {
                        be = "cancelled";
                        return false;
                    }
                });
                while (job.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
                    if (designing) report.design();
                built = job.get();
                if (!built) e = be;
                report.design(true);
            }
            if (!built) {   // never play at the wrong rate
                if (fail_attempt()) return false;
                continue;
            }
            ratio = std::max(1, crate / src.rate);
            tl.h_base = 0;
            dch = rep.device.channels;
            io.block = rep.device.block_align();
            tl.abs_out = from * ratio;
            xf.set_channels(dch);
            io.ring = std::make_unique<ByteRing>(io.ring_bytes(rep.device.frame_rate()));
            dsp.assign(static_cast<size_t>(cur->max_output_frames()) * dch, 0.0);
            tmp_c = dsp;
            tmp_n = dsp;
            io.bytes.assign(dsdenc ? dsdenc->max_bytes(cur->max_output_frames()) : static_cast<size_t>(cur->max_output_frames()) * io.block, 0);
            // DSD: silence first, so the DAC has switched to DSD and locked before the music (output.dsd_lead_in_ms)
            if (dsdenc) push_dsd_idle(s.output.dsd_lead_in_ms);
            while (!cancel && io.ring->writable() >= io.bytes.size()) {   // prefill
                if (!produce()) {
                    io.eof = true;
                    break;
                }
                io.push();
            }
            io.dev->set_dop(dsdenc && dsdenc->dop());
            io.dev->set_paused(want_pause.load());
            if (!io.dev->start(io.ring.get(), &io.eof, &e)) {
                if (fail_attempt()) return false;
                continue;
            }
            break;
        }
        rep.chain = cur->stages();
        rep.filter = report.filter(s, dsdenc.get());
        report.bit_perfect(tl.heard_chain(cur.get()));
        if (cb.on_start) cb.on_start(rep);
        kick_cache_trim();
        return true;
    }


    // a faded frame on an integer device: TPDF re-quantization (FadeQuantizer)
    void requantize(double* frame) { fq.requantize(frame, rep.device, dch, dsdenc != nullptr); }


    void finish_fade() {
        // the next chain becomes current; what it has produced beyond the mixed part plays next
        tl.abs_out += xf.delay_shift;
        rep.clipped += cur->take_clips();   // its last mixed block
        cur = xf.finish();
        rep.chain = cur->stages();
        rep.filter = report.filter(s, dsdenc.get());
        report.bit_perfect(tl.heard_chain(cur.get()));
        event("now playing with filter: " + rep.filter + " | dsp: " + join(rep.chain));
        if (cb.on_changed) cb.on_changed(rep);
    }


    // Starts the crossfade to nx: warm it up on the recent input and align it to the current
    // chain by content delay.
    void begin_crossfade(std::unique_ptr<DspChain> nx) {
        xf.begin(std::move(nx), hist, in_total, tl.abs_out, cur->content_delay(), ratio,
                 std::max<int64_t>(1, static_cast<int64_t>(crate * kFadeSeconds)));
        tmp_n.assign(static_cast<size_t>(xf.next->max_output_frames()) * dch, 0.0);
        tmp_c.assign(static_cast<size_t>(cur->max_output_frames()) * dch, 0.0);
        const size_t need = std::max(tmp_n.size(), tmp_c.size());
        if (dsp.size() < need) {
            dsp.resize(need);
            io.bytes.resize(dsdenc ? dsdenc->max_bytes(static_cast<int>(need / dch)) : need / dch * io.block);
        }
    }

    // One chunk into bytes; false at the end of the stream.
    bool produce() {
        if (io.pend_off < io.pend) return true;
        int n = 0;
        while (n == 0) {
            if (!xf.fading && xf.carry.frames() > 0) {
                n = std::min(xf.carry.frames(), static_cast<int>(dsp.size() / dch));
                std::copy(xf.carry.data(), xf.carry.data() + static_cast<size_t>(n) * dch, dsp.begin());
                xf.carry.pop(n);
                break;
            }
            const int got = dec->read(pcm.data(), kChunk);
            if (got > 0) {
                hist.store(pcm.data(), got, in_total);
                in_total += got;
                if (!xf.fading) {
                    n = cur->process(pcm.data(), got, dsp.data());
                } else {
                    const int a = cur->process(pcm.data(), got, tmp_c.data());
                    xf.fc.push(tmp_c.data(), a);
                    const int b = xf.next->process(pcm.data(), got, tmp_n.data());
                    xf.push_next(tmp_n.data(), b);
                    n = xf.mix(dsp.data(), static_cast<int>(dsp.size() / dch), fq, rep.device, dsdenc != nullptr);
                    if (xf.pos >= xf.len) finish_fade();
                }
            } else {
                if (!flushed && try_gapless()) continue;   // the next track follows without a gap
                if (xf.fading) finish_fade();
                if (xf.carry.frames() > 0) continue;
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
                if (g > 0.0) requantize(dsp.data() + static_cast<size_t>(i) * dch);
            }
        }
        if (fade_out_next) {
            fade_out_next = false;
            for (int i = 0; i < n; ++i) {
                const double g = i < fade_out_len ? 1.0 - static_cast<double>(i) / static_cast<double>(fade_out_len) : 0.0;
                for (int c = 0; c < dch; ++c) dsp[static_cast<size_t>(i) * dch + c] *= g;
                if (g > 0.0 && g < 1.0) requantize(dsp.data() + static_cast<size_t>(i) * dch);
            }
        }
        if (dsdenc) {
            dsd_bytes.clear();
            dsdenc->encode(dsp.data(), n, dsd_bytes);
            if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
            std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
            io.pend = dsd_bytes.size();
        } else {
            rep.clipped += convert_to_device(dsp.data(), static_cast<size_t>(n) * dch, rep.device, io.bytes.data());
            rep.clipped += cur->take_clips() + (xf.next ? xf.next->take_clips() : 0);   // clamped by the final quantizer
            io.pend = static_cast<size_t>(n) * io.block;
        }
        io.pend_off = 0;
        tl.abs_out += n;
        rep.frames_played += n;
        return true;
    }

    // DSD only: queue about `ms` of DSD silence (keeps the DoP marker sequence going).
    void push_dsd_idle(int ms) {
        if (!dsdenc) return;
        dsd_bytes.clear();
        dsdenc->idle(rep.device.frame_rate() * ms / 1000, dsd_bytes);
        if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
        std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
        io.pend = dsd_bytes.size();
        io.pend_off = 0;
        while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // DoP only: about `ms` of plain PCM zeros (no DoP markers) so the DAC leaves its DoP-detected DSD
    // mode before the stream stops; otherwise some DACs beep on one channel
    // when native DSD follows.
    void push_pcm_silence(int ms) {
        if (!dsdenc || !dsdenc->dop()) return;
        const size_t n = static_cast<size_t>(rep.device.frame_rate()) * ms / 1000 * io.block;
        if (io.bytes.size() < n) io.bytes.resize(n);
        std::fill(io.bytes.begin(), io.bytes.begin() + static_cast<std::ptrdiff_t>(n), 0);
        io.pend = n;
        io.pend_off = 0;
        while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }


    // Chain frame being heard (the device's queue counted back from abs_out).
    int64_t heard_out() const {
        int64_t q = io.queued_frames();
        if (rep.device.rate > 0 && crate != rep.device.frame_rate()) q = q * crate / rep.device.frame_rate();   // DoP frames -> chain frames
        return tl.abs_out - q;
    }
    // Stream input frame being heard (Timeline::heard_input).
    int64_t heard_input() const { return tl.heard_input(heard_out(), dsdf ? 0 : cur ? cur->content_delay() : 0, ratio); }
    int64_t heard_position() const { return std::max<int64_t>(0, heard_input() - tl.track_origin_in); }

    // gapless: report tracks that are now heard
    void check_switch() {
        while (!tl.switches.empty()) {
            Switch& w = tl.switches.front();
            if (w.cross) {
                if (heard_out() < w.at_out + w.new_delay) break;
                tl.h_base = w.at_out;
                tl.track_origin_in = 0;
                want = requested_rate(s, w.info.rate);
            } else {
                if (heard_input() < w.at_in) break;
                tl.track_origin_in = w.at_in;
            }
            const bool cross = w.cross;
            rep.source = w.info;
            cur_path = w.path;
            tl.switches.pop_front();
            if (!dsdf) {   // a DSD file sent directly has no chain and stays bit-perfect
                if (cross) rep.filter = report.filter(s, dsdenc.get());
                rep.chain = tl.heard_chain(cur.get())->stages();   // a cross switch's new chain; the track's own headroom
                report.bit_perfect(tl.heard_chain(cur.get()));     // the track's own format
            }
            event(cross ? "gapless: next track (through its own chain: another sample rate or format)" : "gapless: next track");
            if (cb.on_track_started) cb.on_track_started(rep);
        }
    }

    // The source of the track being decoded (the last switched-to one, or the heard one).
    const AudioInfo& decoding_info() const { return tl.switches.empty() ? rep.source : tl.switches.back().info; }

    // Prepares the next track (TrackQueue) for the current playback.
    void kick_prepare() {
        queue.kick([this] {
            TrackQueue::Context c;
            c.dsd = dsdf != nullptr;
            c.cs = chain_settings(s, run_osr);
            c.cf = chain_fmt;
            c.chain_rate = crate;
            c.channels = ch;
            c.cur_rate = decoding_info().rate;
            c.shape = cur ? cur->shape() : DspChain::Shape{};
            c.opener = deps.open_decoder;
            return c;
        });
    }

    // Gapless through a new chain (another sample rate on the same output, or the same rate with a format the old
    // chain does not fit), joined as one filter would: zeros through the old chain
    // bring out A's last samples (its content delay) and their ringing; B's start through the new chain (built
    // ahead) is overlap-added so that B's first sample comes exactly one source period after A's last, its
    // pre-ringing over A's end. The hearing side follows when B is heard (check_switch).
    void cross_switch(std::unique_ptr<Decoder> nd, std::unique_ptr<DspChain> nc, const std::filesystem::path& np) {
        const size_t need = static_cast<size_t>(std::max(cur->max_output_frames(), nc->max_output_frames())) * dch;
        if (dsp.size() < need) dsp.resize(need);
        if (tmp_c.size() < need) tmp_c.resize(need);
        if (tmp_n.size() < need) tmp_n.resize(need);
        const size_t nb = dsdenc ? dsdenc->max_bytes(static_cast<int>(need / dch)) : need / dch * io.block;
        if (io.bytes.size() < nb) io.bytes.resize(nb);

        const int64_t start = tl.abs_out + xf.carry.frames();   // device frame where the joined part begins
        const int64_t a_end = tl.latest_base() + in_total * ratio + cur->content_delay();   // where A's frame K is heard
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
        hist.valid_from = 0;
        std::vector<double> head;
        int64_t produced = 0;
        while (produced <= dn) {
            const int got = nd->read(pcm.data(), kChunk);
            if (got <= 0) break;
            hist.store(pcm.data(), got, in_total);
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
        xf.carry.push(tail.data(), static_cast<int>(tail.size() / dch));

        Switch w;
        w.info = nd->info();
        w.path = np;
        w.cross = true;
        w.at_out = a_end - dn;   // B's frame 0 is heard at at_out + new_delay = a_end
        w.new_delay = dn;
        w.old_ratio = ratio;
        w.old_delay = cur->content_delay();
        w.old_base = tl.latest_base();
        rep.clipped += cur->take_clips();   // its tail, now in carry
        w.old_chain = std::move(cur);
        tl.switches.push_back(std::move(w));
        cur = std::move(nc);
        dec = std::move(nd);
        ratio = std::max(1, crate / dec->info().rate);
    }

    // The track just switched to (switches.back()) has its own headroom (ReplayGain per track): applied from its
    // first decoded sample on (the gain stage ramps over 20 ms); the old value is kept for an undo.
    void apply_headroom(std::optional<double> h) {
        if (!h || tl.switches.empty() || *h == s.headroom_db) return;
        tl.switches.back().old_headroom = s.headroom_db;
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
        for (const auto& w : tl.switches)
            if (w.old_headroom) {
                Settings ns = s;
                ns.headroom_db = *w.old_headroom;
                if (cur && cur->update(chain_settings(ns, run_osr))) s = ns;
                break;
            }
    }

    // A seek or restart before a pending cross switch is heard: back to the heard segment's chain.
    void undo_cross() {
        for (auto& w : tl.switches)
            if (w.cross) {
                cur = std::move(w.old_chain);
                ratio = w.old_ratio;
                break;
            }
    }

    // gapless: continue with the next track when it has the same format (called at the end of
    // the decoder's data)
    bool try_gapless() {
        const auto [np, nh] = queue.get();
        if (np.empty() || is_dsd_path(np) != (dsdf != nullptr)) return false;
        const std::shared_ptr<Prep> p = queue.take(np, cancel);   // opened ahead (kick_prepare)
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
            w.at_in = tl.abs_out;
            w.info = ai;
            w.path = np;
            tl.switches.push_back(std::move(w));
            dsdf = std::move(f);
        } else {
            std::unique_ptr<Decoder> nd = p ? std::move(p->dec) : nullptr;
            if (!nd) nd = deps.open_decoder(np, &e);
            if (!nd || nd->info().channels != ch) return false;
            const bool fits = cur->fits(chain_settings(s, run_osr), nd->info());
            if (fits) {
                Switch w;
                w.at_in = in_total;
                w.info = nd->info();
                w.path = np;
                tl.switches.push_back(std::move(w));
                dec = std::move(nd);
                apply_headroom(nh);
            } else {
                // another sample rate, or the same rate with a format the current chain does not fit (a 24-bit
                // track after a 16-bit one on a 16-bit device needs the quantizer): gapless only on the same
                // output, joined through the new chain built ahead for the current settings; otherwise the track
                // ends and the host loads the next one
                if (!p || !p->done || !p->chain || xf.fading || building.valid() || p->key != TrackQueue::chain_key(chain_settings(s, run_osr), chain_fmt))
                    return false;
                cross_switch(std::move(nd), std::move(p->chain), np);
                apply_headroom(nh);
            }
        }
        queue.consumed(np);
        return true;
    }

    // The end of the stream: DSD silence, then (DoP) PCM silence so the DAC leaves DSD mode;
    // queued without blocking, the loop pushes it.
    void queue_end() {
        end_queued = true;
        io.eof = true;
        if (!dsdenc) return;
        dsd_bytes.clear();
        dsdenc->idle(rep.device.frame_rate() / 10, dsd_bytes);
        if (dsdenc->dop()) dsd_bytes.resize(dsd_bytes.size() + static_cast<size_t>(rep.device.frame_rate()) / 5 * io.block, 0);
        if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
        std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
        io.pend = dsd_bytes.size();
        io.pend_off = 0;
    }

    // Seek with the device open: pause it (PCM fades out), drop what is queued, refill, resume.
    void do_seek(double sec) {
        if (!io.dev) return;
        io.dev->set_paused(true);
        // the device fades out at its next callback, one buffer away (output.buffer_ms): wait for two of them
        const int buf_ms = rep.device.frame_rate() > 0 ? static_cast<int>(1000LL * io.dev->buffer_frames() / rep.device.frame_rate()) : 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(300, 2 * buf_ms + 50));
        while (!io.dev->paused() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        io.ring->clear();
        io.pend = io.pend_off = 0;
        std::string e;
        check_switch();   // a track heard just now is the one the seek is in
        if (!tl.switches.empty()) {
            // the decoder already moved on to a gapless track that is not heard yet: back to this one
            queue.restore(tl.switches.front().path);
            undo_cross();
            undo_headroom();
            tl.switches.clear();
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
            tl.abs_out = from;
        } else {
            dec->seek(from);
            in_total = from;
            hist.valid_from = from;
            flushed = false;
            xf.clear();
            if (xf.fading) {   // a crossfade was running: the new chain takes over
                cur = std::move(xf.next);
                xf.fading = false;
                rep.chain = cur->stages();
                rep.filter = report.filter(s, dsdenc.get());
                report.bit_perfect(tl.heard_chain(cur.get()));
            }
            cur->reset();
            tl.abs_out = from * ratio;
        }
        tl.h_base = 0;
        tl.track_origin_in = 0;
        io.eof = false;
        end_queued = false;
        io.dev->rearm();
        while (!cancel && io.ring->writable() >= io.bytes.size()) {   // refill
            if (!(dsdf ? produce_dsd() : produce())) {
                if (!try_gapless()) {
                    queue_end();
                    break;
                }
                continue;
            }
            io.push();
        }
        io.push();
        io.dev->set_paused(want_pause.load());
        pos = heard_position();
        event("seek to " + std::to_string(sec) + " s");
    }


    // One pass of the transport requests (DSP thread).
    void service_transport() {
        std::optional<double> sk;
        {
            std::lock_guard<std::mutex> l(m);
            sk.swap(seek_req);
        }
        if (sk) do_seek(*sk);
        if (io.dev) {
            io.dev->set_paused(want_pause.load());
            double l = 0, r = 0;
            io.dev->meter(l, r);
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
        report.design_prog = 0.0;
        build_designs = (chain_fmt.rate != src.rate || cs.filter.apodize) && !filter_cached(cs.filter, src.rate, chain_fmt.rate);
        build_cancel = std::make_shared<std::atomic<bool>>(false);
        building = std::async(std::launch::async, [cs, src, devf = chain_fmt, this, flag = build_cancel]() -> std::unique_ptr<DspChain> {
            // playback is running: stay below it (the design workers take this priority)
            platform::set_thread_priority(platform::Priority::BelowNormal);
            dsp::ProgressScope scope(&report.design_prog);
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
        event((build_designs ? "designing " : "building ") + describe_filter(ns) + " in the background (playback continues)");
    }

    // The PCM decoder for a file: DSD files reaching this point are converted to PCM.
    std::unique_ptr<Decoder> open_source(const std::filesystem::path& file, const Settings& st, std::string* e) {
        if (!is_dsd_path(file)) return deps.open_decoder(file, e);
        auto d = make_dsd_pcm_decoder(dsd_pcm_rate, st.output.dsd_pcm_lowpass);
        if (!d->open(file, e)) d.reset();
        return d;
    }

    // what plays now, for the change decisions (change_plan.h)
    Playing playing() const { return {s, out, want, rep.source.rate, cur_path}; }

    // drain = false: the device is reopened as it is (a driver reset): what is queued is dropped.
    bool restart(const Settings& ns, const OutputChoice& no, bool drain = true) {
        check_switch();
        if (tl.cross_pending()) drain = false;   // the queue holds the next track at another rate: not played out
        int64_t from = dsdf ? heard_position() : std::max<int64_t>(0, heard_input());   // PCM: stream timeline
        io.drop_if_paused();
        queue.drop();   // built for the old settings / device
        if (io.dev && !io.eof && !io.dev->failed() && drain) {
            // play what is queued, fade the next chunk out, let the device drain, resume there
            while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            from = std::max<int64_t>(0, (tl.abs_out - (cur ? cur->content_delay() : 0)) / std::max(1, ratio));
            fade_out_len = std::max<int64_t>(1, crate / 50);   // 20 ms
            fade_out_next = true;
            if (produce())
                while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            push_dsd_idle(50);
            push_pcm_silence(200);
            io.eof = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!io.dev->drained() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        want_fade_in = true;
        // `from` is on the stream timeline. With gapless tracks the decoder may have moved on: resume in
        // the track that holds `from`, at its own frame (a later track that was decoded but not reached
        // goes back to being the next track)
        bool reopen_dec = decoder_changes(playing(), ns);
        if (!dsdf) {
            while (!tl.switches.empty() && !tl.switches.front().cross && from >= tl.switches.front().at_in) {
                tl.track_origin_in = tl.switches.front().at_in;
                rep.source = tl.switches.front().info;
                cur_path = tl.switches.front().path;
                tl.switches.pop_front();
                if (cb.on_track_started) cb.on_track_started(rep);
            }
            if (!tl.switches.empty()) {
                queue.restore(tl.switches.front().path);
                undo_cross();
                undo_headroom();
                tl.switches.clear();
                reopen_dec = true;
            }
            from = std::max<int64_t>(0, from - tl.track_origin_in);
            tl.track_origin_in = 0;
        }
        if (io.dev) io.dev->stop();
        io.dev.reset();
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
        queue.drop();   // a chain prepared for the next track was built for the old settings
        if (xf.fading || building.valid() || tl.cross_pending()) {   // one change at a time (and not across a rate switch); the latest waits
            queued = std::make_pair(ns, no);
            if (building.valid() && build_cancel) *build_cancel = true;   // superseded: stop designing it
            return true;
        }
        dsp::set_design_threads(ns.design.threads);
        if (needs_restart(playing(), ns, no)) return restart(ns, no);
        if (cur->update(chain_settings(ns, run_osr))) {
            s = ns;
            event("applied in place (EQ / crossfeed / volume)");
            // the stages that are active now (EQ switched on / off, volume at 0 dB ...)
            const auto stages = cur->stages();
            const bool changed_stages = stages != rep.chain, was_bit_perfect = rep.bit_perfect;
            rep.chain = stages;
            report.bit_perfect(tl.heard_chain(cur.get()));
            if (changed_stages || rep.bit_perfect != was_bit_perfect) {
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
            if (build_designs) report.design();
            return true;
        }
        report.design(true);
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
        if (api_has_native_dsd(out.api) && s.output.dsd != DsdMode::Dop) at.push_back({di.dsd_rate, true});
        if (s.output.dsd != DsdMode::Native) at.push_back({di.dsd_rate / 16, false});
        std::string e = at.empty() ? "output.dsd = native needs an ASIO driver with a DSD mode" : "";
        for (const auto& a : at) {
            io.dev = open_output({out, s.output.buffer_ms, a.rate, di.channels, a.native}, &e);
            if (!io.dev) continue;
            if (io.dev->format().rate != a.rate || io.dev->format().channels != di.channels) {
                e = "the device locked " + describe(io.dev->format()) + " instead of " + std::to_string(a.rate) + " Hz, " +
                    std::to_string(di.channels) + " ch";
                io.dev.reset();
                continue;
            }
            dsdenc = std::make_unique<DsdEncoder>();
            if (!dsdenc->init_direct(di.dsd_rate, io.dev->format(), a.native ? DsdTransport::Native : DsdTransport::Dop, &e)) {
                io.dev.reset();
                continue;
            }
            rep.device = io.dev->format();
            rep.device_name = io.dev->device_name();
            rep.latency_ms = io.dev->latency_ms();
            io.note_buffer_fallback(report, s.output.buffer_ms);
            crate = di.dsd_rate / 8;
            ratio = 1;
            dch = rep.device.channels;
            io.block = rep.device.block_align();
            dsdf->seek(from);
            tl.abs_out = from;
            io.eof = false;
            io.pend = io.pend_off = 0;
            io.ring = std::make_unique<ByteRing>(io.ring_bytes(rep.device.frame_rate()));
            dsd_in.assign(static_cast<size_t>(kChunk) * 4 * di.channels, 0);
            io.bytes.assign(static_cast<size_t>(kChunk) * 4 * io.block, 0);
            // silence first, so the DAC has switched to DSD and locked before the music (output.dsd_lead_in_ms;
            // at least 30 ms after a restart)
            push_dsd_idle(std::max(s.output.dsd_lead_in_ms, want_fade_in ? 30 : 0));
            while (!cancel && io.ring->writable() >= io.bytes.size()) {
                if (!produce_dsd()) {
                    io.eof = true;
                    break;
                }
                io.push();
            }
            io.dev->set_dop(dsdenc->dop());
            io.dev->set_paused(want_pause.load());
            if (!io.dev->start(io.ring.get(), &io.eof, &e)) {
                io.dev.reset();
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
        if (io.pend_off < io.pend) return true;
        const int n = dsdf->read(dsd_in.data(), kChunk * 4);
        if (n <= 0) return false;
        dsd_bytes.clear();
        dsdenc->encode_dsd(dsd_in.data(), n, dsd_bytes);
        if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
        std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
        io.pend = dsd_bytes.size();
        io.pend_off = 0;
        tl.abs_out += n;
        rep.frames_played += n;
        return true;
    }

    // DSD direct: only the output can change (no processing applies to DSD sent directly).
    // reset: a driver reset reopens the device as it is (nothing more is queued for it).
    bool handle_dsd(const Settings& ns, const OutputChoice& no, bool reset = false) {
        const bool reopen = reset || dsd_direct_reopens(playing(), ns, no);
        s = ns;
        if (!reopen) {
            event("DSD file played directly: DSP settings do not apply");
            return true;
        }
        const int64_t from = heard_position();
        io.drop_if_paused();
        if (!reset) {
            push_dsd_idle(50);
            push_pcm_silence(200);
            io.eof = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (io.dev && !io.dev->drained() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (io.dev) io.dev->stop();
        io.dev.reset();
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
        platform::RealtimeScope rt;
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
                if (api_is_shared(out.api)) {
                    std::string pe;
                    if (auto probe = open_output({out, s.output.buffer_ms, dsdf->info().dsd_rate / 16, dsdf->info().channels, false}, &pe)) {
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
                if (!io.eof) {
                    if (!produce_dsd() && !try_gapless()) queue_end();
                }
                if (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                else if (io.eof && io.pend_off >= io.pend) {
                    if (io.dev->drained()) {
                        ended_flag = true;
                        if (cb.on_ended) cb.on_ended();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                pos = heard_position();
                pos_sec = rep.source.rate > 0 ? static_cast<double>(pos) / rep.source.rate : 0.0;
                if (cb.on_progress) cb.on_progress(pos, rep.source.frames);
                if (io.dev->failed()) break;
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
            hist.reset(ch);
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
            if (!xf.fading && !building.valid() && !tl.cross_pending() && queued) {
                auto q = *queued;
                queued.reset();
                if (!handle(q.first, q.second)) {
                    ok = false;
                    break;
                }
            }
            kick_prepare();
            service_transport();
            if (!io.eof && !produce()) queue_end();
            if (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            else if (io.eof && io.pend_off >= io.pend) {
                if (io.dev->drained()) {
                    ended_flag = true;
                    if (cb.on_ended) cb.on_ended();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            pos = heard_position();
            pos_sec = rep.source.rate > 0 ? static_cast<double>(pos) / rep.source.rate : 0.0;
            if (cb.on_progress) cb.on_progress(pos, rep.source.frames);
            if (io.dev->failed()) break;
            if (!service_device()) {
                ok = false;
                break;
            }
        }
        finish_playback();
    }

    // The device's upkeep (OutputSession::upkeep); a driver that asks to be opened again gets reopened,
    // resuming where it was.
    bool service_device() {
        switch (io.upkeep(report, rep.device.frame_rate(), &err)) {
            case OutputSession::Upkeep::Ok: return true;
            case OutputSession::Upkeep::Fail: return false;
            case OutputSession::Upkeep::Reopen: break;
        }
        return dsdf ? handle_dsd(s, out, true) : restart(s, out, false);
    }

    void finish_playback() {
        queue.join_all();
        if (io.dev && !io.dev->failed() && dsdenc && !ended_flag) {
            // stopped (or failed) before the end: stop quickly, but on silence. DoP: PCM silence (the DAC
            // leaves DSD mode). Native DSD: DSD silence (0x69, what the feeder writes once the ring is
            // empty) reaches the DAC before the stream stops, not a cut in the middle of the music
            if (io.dev->paused() || cancel) {
                io.ring->clear();
                io.dev->set_paused(false);
            }
            io.pend = io.pend_off = 0;
            push_pcm_silence(200);
            io.eof = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!io.dev->drained() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!dsdenc->dop() && io.dev->drained() && rep.device.frame_rate() > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(io.dev->latency_ms()) + 2 * io.dev->buffer_frames() * 1000 / rep.device.frame_rate() + 5));
        }
        if (ok && io.dev && ended_flag) {
            // the device buffer plays out (the ring has drained)
            if (!cancel)
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(io.dev->latency_ms()) +
                                                                      2 * io.dev->buffer_frames() * 1000 / rep.device.frame_rate() + 20));
        }
        if (build_cancel) *build_cancel = true;
        if (building.valid()) building.wait();
        stop_cache_trim(true);
        if (io.dev) {
            io.dev->stop();
            rep.underruns = io.dev->stats().underruns;
            rep.event_timeouts = io.dev->stats().event_timeouts;
            rep.max_interval_us = io.dev->stats().max_interval_us;
            if (io.dev->failed()) {
                ok = false;
                err = io.dev->failure();
            }
            io.dev.reset();
        }
        if (!ok && !cancel && cb.on_failed) cb.on_failed(err);
        done = true;
    }
};

Player::Player() : Player(PlayerDeps{}) {}

Player::Player(PlayerDeps deps) : impl_(std::make_unique<Impl>()) {
    if (!deps.open_output) deps.open_output = platform_output_opener();
    if (!deps.open_decoder) deps.open_decoder = [](const std::filesystem::path& p, std::string* e) { return open_decoder(p, e); };
    impl_->deps = std::move(deps);
}

Player::~Player() {
    stop();
    if (impl_->th.joinable()) impl_->th.join();
}

bool Player::start(const std::filesystem::path& path, const Settings& s, const OutputChoice& out, PlayCallbacks cb, std::string* error,
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

void Player::set_next(const std::filesystem::path& path, std::optional<double> headroom_db) {
    impl_->queue.set(path, headroom_db);
}

bool Player::ended() const { return impl_->ended_flag; }
bool Player::last_failure_was_device() const { return impl_->device_fail; }

std::vector<int> Player::asio_rates() const {
    return impl_->report.asio_rates();
}

std::string Player::debug_json() const {
    return impl_->report.diag_json();
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

bool play_file(const std::filesystem::path& path, const Settings& s, const OutputChoice& out, const std::atomic<bool>& cancel,
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
