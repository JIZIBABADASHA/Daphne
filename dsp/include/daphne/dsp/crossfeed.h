#pragma once
// Crossfeed for headphones: each side gets the other side through a one-pole low-pass.
//
//   L' = (1 - k) L + k lp(R),  R' = (1 - k) R + k lp(L),  lp: y += (x - y) a, a = dt / (RC + dt),
//   RC = 1 / (2 pi fc)
//
// so the low frequencies of a sound panned to one side reach the other ear as well, and a sound in
// the centre keeps its level (k + (1 - k) = 1 below fc). Stereo only (any other channel count passes
// through). amount k in [0, 1], fc in [100, 3000] Hz.
//
// set_params() may be called from any (one) control thread at any time; it never blocks the audio
// thread (mailbox.h) and the three values arrive together. The first process() after prepare() /
// reset() starts at the newest parameters; later changes (on / off, amount, fc) ramp k and a
// linearly over ramp_ms, whatever the block size, so a change never steps the output. Off is k = 0;
// once the ramp to off has ended the block copies its input again (bit-exact).

#include <atomic>

#include "daphne/dsp/block.h"
#include "daphne/dsp/mailbox.h"

namespace daphne::dsp {

class Crossfeed final : public Block {
public:
    explicit Crossfeed(double ramp_ms = 20.0) : ramp_ms_(ramp_ms) {}

    void set_params(bool enabled, double amount, double fc);

    void prepare(const Format& in) override;
    Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override;

private:
    struct Params {
        bool enabled = false;
        double amount = 0.22, fc = 700.0;
    };
    void retarget(const Params& p, bool jump);

    Format fmt_{};
    Mailbox<Params> box_;
    std::atomic<bool> want_on_{false};   // the newest posted parameters (for is_identity)
    double ramp_ms_;
    // audio thread only
    double k_ = 0.0, a_ = 0.0;             // in use
    double k_target_ = 0.0, a_target_ = 0.0;
    double k_step_ = 0.0, a_step_ = 0.0;   // per frame
    int ramp_left_ = 0;                    // frames until the targets are reached
    double lp_l_ = 0, lp_r_ = 0;
};

}  // namespace daphne::dsp
