#pragma once
// Crossfeed for headphones: each side gets the other side through a one-pole low-pass.
//
//   L' = (1 - k) L + k lp(R),  R' = (1 - k) R + k lp(L),  lp: y += (x - y) a, a = dt / (RC + dt),
//   RC = 1 / (2 pi fc)
//
// so the low frequencies of a sound panned to one side reach the other ear as well, and a sound in
// the centre keeps its level (k + (1 - k) = 1 below fc). Stereo only (any other channel count passes
// through). amount k in [0, 1], fc in [100, 3000] Hz. set_params may be called from any thread.

#include <atomic>

#include "daphne/dsp/block.h"

namespace daphne::dsp {

class Crossfeed final : public Block {
public:
    void set_params(bool enabled, double amount, double fc);

    void prepare(const Format& in) override;
    Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override { return fmt_.channels != 2 || !enabled_.load(std::memory_order_relaxed); }

private:
    Format fmt_{};
    std::atomic<bool> enabled_{false};
    std::atomic<double> amount_{0.22}, fc_{700.0};
    double lp_l_ = 0, lp_r_ = 0;
};

}  // namespace daphne::dsp
