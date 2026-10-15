#pragma once
// How fast the processing thread produces audio compared with real time (N4): the chain's work on each
// chunk (decoding, the disk, waiting for a live source, gapless preparation and the warm-ups of a new
// chain are not counted) against the length of the audio it produced. Below real time the ring empties
// and the device plays gaps (underruns) however large the buffers are: a long filter with double-double
// convolution on a slow CPU, for example.
//
// Measured over windows of 2 s of audio; the factor is audio seconds per second of work in the last
// window (10 = ten times faster than real time). warn() is true once when two windows in a row come
// below kWarn, and again only after the factor has recovered above kClear.

#include <chrono>

namespace daphne::playback {

class Throughput {
public:
    static constexpr double kWindow = 2.0;   // seconds of audio per measurement
    static constexpr double kWarn = 1.2;     // the margin a real-time thread needs (scheduling, other work)
    static constexpr double kClear = 1.5;

    using Clock = std::chrono::steady_clock;

    // `work` spent producing `audio_s` seconds of output
    void add(Clock::duration work, double audio_s) {
        work_ += std::chrono::duration<double>(work).count();
        audio_ += audio_s;
        if (audio_ < kWindow) return;
        factor_ = work_ > 0 ? audio_ / work_ : 1e9;
        work_ = audio_ = 0;
        if (factor_ < kWarn) {
            if (++low_ >= 2 && !warned_) warned_ = pending_ = true;
        } else {
            low_ = 0;
            if (factor_ > kClear) warned_ = false;
        }
    }
    // the last window's factor; 0 before the first window
    double factor() const { return factor_; }
    // true once per slow stretch (see above)
    bool warn() {
        const bool w = pending_;
        pending_ = false;
        return w;
    }
    void reset() { *this = Throughput{}; }

private:
    double work_ = 0, audio_ = 0, factor_ = 0;
    int low_ = 0;
    bool warned_ = false, pending_ = false;
};

}  // namespace daphne::playback
