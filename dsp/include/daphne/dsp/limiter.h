#pragma once
// Look-ahead peak limiter: after DSD -> PCM (whose +6 dB lets peaks above the SACD reference level
// exceed full scale) and in front of the DSD modulator (which must stay below its stable input).
//
// Every frame whose peak (largest channel) would exceed the ceiling asks for gain ceiling / peak.
// That request is held for the look-ahead window, released towards 1 with a one-pole release,
// and averaged over the window; the audio is delayed by the window. The average of values that
// are all <= the request reaches the peak exactly when the delayed peak sample does, so no sample
// leaves above the ceiling, and the gain moves in a smooth ramp instead of a step. All channels
// share one gain (the stereo image stays). Below the ceiling the output is the delayed input,
// bit for bit.

#include <vector>

#include "daphne/dsp/block.h"

namespace daphne::dsp {

class Limiter final : public Block {
public:
    explicit Limiter(double ceiling = 0.999, double lookahead_ms = 2.0, double release_ms = 80.0)
        : ceiling_(ceiling), lookahead_ms_(lookahead_ms), release_ms_(release_ms) {}

    void prepare(const Format& in) override;
    Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return w_; }
    int flush(double* out) override;   // the delayed tail
    void reset() override;
    bool is_identity() const override { return false; }   // it delays the signal

    // Frames since the last reset whose gain was below 1, for reports and tests.
    long long limited_frames() const { return limited_; }

private:
    double ceiling_, lookahead_ms_, release_ms_;
    Format fmt_{};
    int w_ = 1;                       // look-ahead (delay) in frames
    double rel_ = 0.0;                // release coefficient per frame
    std::vector<double> delay_;       // w_ frames, interleaved ring
    std::vector<double> avg_;         // last w_ released gains (box average)
    std::vector<double> min_val_;     // monotonic queue for the sliding minimum over w_ + 1 requests
    std::vector<long long> min_idx_;
    int qhead_ = 0, qsize_ = 0;
    int pos_ = 0;                     // ring position (delay_, avg_)
    long long n_ = 0;                 // frame counter
    double sum_ = 0.0, env_ = 1.0;
    long long limited_ = 0;
    std::vector<double> zeros_;       // flush input
};

}  // namespace daphne::dsp
