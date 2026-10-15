// Player::Impl, the processing thread (core.h): start, the playback loop, transport, device upkeep, the end.
#include "core.h"

namespace daphne {

// PCM: settings changes in place / rebuilt / with a restart; the background rebuild and the change
// queued behind it are polled between chunks.
class Player::Impl::PcmStream final : public Stream {
public:
    explicit PcmStream(Impl& p) : p_(p) {}
    bool apply(const Settings& ns, const OutputChoice& no) override { return p_.handle(ns, no); }
    bool poll() override {
        if (!p_.poll_build()) return false;
        if (!p_.xf.fading && !p_.building.valid() && !p_.tl.cross_pending() && p_.queued) {
            auto q = *p_.queued;
            p_.queued.reset();
            return p_.handle(q.first, q.second);
        }
        return true;
    }
    bool produce() override { return p_.produce(); }
    bool reopen() override { return p_.restart(p_.s, p_.out, false); }

private:
    Impl& p_;
};

// A DSD file sent directly: only the output can change; at the end of a file the next one may follow.
class Player::Impl::DsdDirectStream final : public Stream {
public:
    explicit DsdDirectStream(Impl& p) : p_(p) {}
    bool apply(const Settings& ns, const OutputChoice& no) override { return p_.handle_dsd(ns, no); }
    bool produce() override { return p_.produce_dsd() || p_.try_gapless(); }
    bool reopen() override { return p_.handle_dsd(p_.s, p_.out, true); }

private:
    Impl& p_;
};

void Player::Impl::update_diag() {
    if (!report.diag_due()) return;
    char b[512];
    const OutputStats* st = io.dev ? &io.dev->stats() : nullptr;
    std::snprintf(b, sizeof b,
                  "{\"ring\":%zu,\"ring_cap\":%zu,\"pend\":%zu,\"eof\":%d,\"fading\":%d,\"building\":%d,\"paused\":%d,"
                  "\"dev_paused\":%d,\"callbacks\":%lld,\"underruns\":%lld,\"drained\":%d,\"failed\":%d,\"late\":%lld,\"abs_out\":%lld,\"in\":%lld,"
                  "\"rt\":%.2f}",
                  io.ring ? io.ring->readable() : 0, io.ring ? io.ring->capacity() : 0, io.pend - io.pend_off, io.eof.load() ? 1 : 0, xf.fading ? 1 : 0,
                  building.valid() ? 1 : 0, want_pause.load() ? 1 : 0, io.dev && io.dev->paused() ? 1 : 0,
                  st ? static_cast<long long>(st->callbacks.load()) : -1LL, st ? static_cast<long long>(st->underruns.load()) : -1LL,
                  io.dev && io.dev->drained() ? 1 : 0, io.dev && io.dev->failed() ? 1 : 0,
                  st ? static_cast<long long>(st->late_callbacks.load()) : -1LL, static_cast<long long>(tl.abs_out), static_cast<long long>(in_total),
                  throughput.factor());
    report.set_diag(b);
}

void Player::Impl::stop_cache_trim(bool join) {
    for (auto& j : pre_jobs) *j.cancel = true;
    if (!join) return;
    for (auto& j : pre_jobs)
        if (j.th.joinable()) j.th.join();
    pre_jobs.clear();
}

void Player::Impl::kick_cache_trim() {
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

int64_t Player::Impl::heard_out() const {
    int64_t q = io.queued_frames();
    if (rep.device.rate > 0 && crate != rep.device.frame_rate()) q = q * crate / rep.device.frame_rate();   // DoP frames -> chain frames
    return tl.abs_out - q;
}

void Player::Impl::check_switch() {
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

void Player::Impl::do_seek(double sec) {
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

void Player::Impl::service_transport() {
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
    if (throughput.warn()) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "processing runs at %.2fx real time (it needs %.1fx): the device will play gaps; a shorter filter, "
                      "no double-double convolution or a lower output rate helps",
                      throughput.factor(), Throughput::kWarn);
        event(b);
    }
    update_diag();
}

void Player::Impl::run() {
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
        DsdDirectStream st(*this);
        play(st);
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
    PcmStream st(*this);
    play(st);
}

void Player::Impl::play(Stream& st) {
    stream = &st;
    ok = true;
    cur_path = path;
    while (!cancel) {
        std::optional<std::pair<Settings, OutputChoice>> p;
        {
            std::lock_guard<std::mutex> l(m);
            p.swap(pending);
        }
        if (p && !st.apply(p->first, p->second)) {
            ok = false;
            break;
        }
        if (!st.poll()) {
            ok = false;
            break;
        }
        kick_prepare();
        service_transport();
        if (!io.eof && !st.produce()) queue_end();
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
    stream = nullptr;
}

bool Player::Impl::service_device() {
    switch (io.upkeep(report, rep.device.frame_rate(), &err)) {
        case OutputSession::Upkeep::Ok: return true;
        case OutputSession::Upkeep::Fail: return false;
        case OutputSession::Upkeep::Reopen: break;
    }
    return stream->reopen();
}

void Player::Impl::finish_playback() {
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

}  // namespace daphne
