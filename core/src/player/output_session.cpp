#include "output_session.h"

#include <cmath>
#include <cstdio>

#include "report.h"

namespace daphne::playback {

void OutputSession::note_buffer_fallback(const Report& report, int buffer_ms) const {
    if (dev->buffer_fallback())
        report.event("output.buffer_ms: the device does not take a " + std::to_string(buffer_ms) + " ms buffer, playing with its default (" +
                     std::to_string(std::lround(dev->latency_ms())) + " ms)");
}

OutputSession::Upkeep OutputSession::upkeep(const Report& report, int frame_rate, std::string* error) {
    for (const auto& n : dev->take_notes()) report.event(n);
    const auto now = std::chrono::steady_clock::now();
    const OutputStats& st = dev->stats();
    if (late_dev_ != dev.get()) {
        late_dev_ = dev.get();
        late_seen_ = 0;
        rate_cb_ = -1;
    }
    // callbacks much faster than the buffer size allows: the driver changed its buffer behind us
    const int64_t cbs = st.callbacks.load();
    if (rate_cb_ < 0 || cbs < rate_cb_) {
        rate_cb_ = cbs;
        rate_at_ = now;
    } else if (now - rate_at_ >= std::chrono::seconds(1)) {
        const double sec = std::chrono::duration<double>(now - rate_at_).count();
        const double got = (cbs - rate_cb_) / sec;
        const double expect = dev->buffer_frames() > 0 ? static_cast<double>(frame_rate) / dev->buffer_frames() : 0;
        if (expect > 0 && got > expect * 1.5) {
            char b[160];
            std::snprintf(b, sizeof b, "device: %.0f callbacks/s, expected %.0f (buffer %d frames)", got, expect, dev->buffer_frames());
            report.event(b);
        }
        rate_cb_ = cbs;
        rate_at_ = now;
    }
    const int64_t late = st.late_callbacks.load();
    if (late != late_seen_ && now - late_at_ >= std::chrono::seconds(1)) {
        char b[200];
        std::snprintf(b, sizeof b, "device: %lld late callbacks (total %lld, longest %.1f ms, buffer %.1f ms, underruns %lld)",
                      static_cast<long long>(late - late_seen_), static_cast<long long>(late), st.max_interval_us.load() / 1000.0,
                      frame_rate > 0 ? dev->buffer_frames() * 1000.0 / frame_rate : 0.0, static_cast<long long>(st.underruns.load()));
        report.event(b);
        late_seen_ = late;
        late_at_ = now;
    }
    if (!dev->reset_requested()) return Upkeep::Ok;
    while (!resets_.empty() && now - resets_.front() > std::chrono::seconds(10)) resets_.pop_front();
    if (resets_.size() >= 3) {
        if (error) *error = "the output driver keeps asking to be reset";
        return Upkeep::Fail;
    }
    resets_.push_back(now);
    return Upkeep::Reopen;
}

}  // namespace daphne::playback
