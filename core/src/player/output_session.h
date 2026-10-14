#pragma once
// The device side of the player (N23, N24): the open output, the ring it reads, the chunk being pushed
// into it, and the device's upkeep while playing (driver notes, callback rate, late callbacks, reset
// requests).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "daphne/engine/audio_output.h"
#include "daphne/engine/ring_buffer.h"

namespace daphne::playback {

class Report;

class OutputSession {
public:
    std::unique_ptr<AudioOutput> dev;
    std::unique_ptr<ByteRing> ring;
    std::vector<uint8_t> bytes;      // the chunk in device format: pend bytes, pend_off of them in the ring
    size_t pend = 0, pend_off = 0;
    int block = 0;                   // bytes per device frame
    std::atomic<bool> eof{false};    // the end of the stream is queued (the device drains then)

    // about 0.5 s, and at least three device buffers (a long output.buffer_ms takes a whole buffer per event)
    size_t ring_bytes(int frame_rate) const {
        return std::max(static_cast<size_t>(frame_rate) * block / 2, static_cast<size_t>(dev->buffer_frames()) * 3 * block);
    }
    // The pending chunk into the ring; false when the ring is full.
    bool push() {
        while (pend_off < pend) {
            const size_t n = ring->write(bytes.data() + pend_off, pend - pend_off);
            pend_off += n;
            if (n == 0) return false;
        }
        return true;
    }
    // Device frames written but not heard yet (the ring and the rest of the pending chunk).
    int64_t queued_frames() const {
        return ring ? static_cast<int64_t>(ring->readable() / std::max(1, block)) + static_cast<int64_t>((pend - pend_off) / std::max(1, block))
                    : 0;
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
    // output.buffer_ms the device did not take (WASAPI exclusive): it runs with its default buffer
    void note_buffer_fallback(const Report& report, int buffer_ms) const;

    enum class Upkeep { Ok, Reopen, Fail };
    // The device's notes go to the host log (driver messages, late callbacks); a driver that asks to be
    // opened again gets Reopen, at most 3 times in 10 s (then Fail, with *error).
    Upkeep upkeep(const Report& report, int frame_rate, std::string* error);

private:
    const AudioOutput* late_dev_ = nullptr;
    int64_t late_seen_ = 0;
    std::chrono::steady_clock::time_point late_at_{};
    int64_t rate_cb_ = -1;            // callbacks at the start of the rate window
    std::chrono::steady_clock::time_point rate_at_{};
    std::deque<std::chrono::steady_clock::time_point> resets_;
};

}  // namespace daphne::playback
