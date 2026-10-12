#include "daphne/dsp/crossfeed.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace daphne::dsp {

void Crossfeed::set_params(bool enabled, double amount, double fc) {
    auto p = std::make_unique<Params>();
    p->enabled = enabled;
    p->amount = std::isnan(amount) ? 0.0 : std::clamp(amount, 0.0, 1.0);
    p->fc = std::isnan(fc) ? 700.0 : std::clamp(fc, 100.0, 3000.0);
    want_on_.store(enabled, std::memory_order_relaxed);
    box_.post(std::move(p));
}

void Crossfeed::prepare(const Format& in) {
    fmt_ = in;
    reset();
}

void Crossfeed::reset() {
    // a reset (seek, new track) jumps straight to the newest parameters
    if (const Params* p = box_.take()) retarget(*p, true);
    else if (const Params* c = box_.current()) retarget(*c, true);
    else retarget(Params{}, true);
    lp_l_ = lp_r_ = 0.0;
}

void Crossfeed::retarget(const Params& p, bool jump) {
    const double dt = 1.0 / static_cast<double>(std::max(1, fmt_.rate));
    const double k = p.enabled ? p.amount : 0.0;
    // off keeps the coefficient where it is: only k ramps to 0
    const double a = p.enabled ? dt / (1.0 / (p.fc * 6.283185307179586) + dt) : a_target_;
    k_target_ = k;
    a_target_ = a;
    if (jump || (k_ == 0.0 && ramp_left_ == 0)) a_ = a;   // while off nothing depends on a
    if (jump) {
        k_ = k;
        ramp_left_ = 0;
        return;
    }
    if (k_ == k && a_ == a) {
        ramp_left_ = 0;
        return;
    }
    ramp_left_ = std::max(1, static_cast<int>(std::lround(ramp_ms_ * 1e-3 * fmt_.rate)));
    k_step_ = (k - k_) / ramp_left_;
    a_step_ = (a - a_) / ramp_left_;
}

bool Crossfeed::is_identity() const { return fmt_.channels != 2 || (!want_on_.load(std::memory_order_relaxed) && k_ == 0.0 && ramp_left_ == 0); }

int Crossfeed::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    if (ch == 2)
        if (const Params* p = box_.take()) retarget(*p, false);
    if (ch != 2 || (k_ == 0.0 && ramp_left_ == 0)) {
        if (in != out) std::copy(in, in + static_cast<size_t>(frames) * ch, out);
        if (ch == 2) lp_l_ = lp_r_ = 0.0;   // off: the low-pass starts from rest when switched on again
        return frames;
    }
    double l = lp_l_, r = lp_r_;
    int i = 0;
    for (; i < frames && ramp_left_ > 0; ++i) {   // ramping: k and a move every frame and end exactly on the targets
        if (--ramp_left_ == 0) k_ = k_target_, a_ = a_target_;
        else k_ += k_step_, a_ += a_step_;
        const double xl = in[2 * i], xr = in[2 * i + 1];
        l = (xl - l) * a_ + l;
        r = (xr - r) * a_ + r;
        out[2 * i] = r * k_ + (1.0 - k_) * xl;
        out[2 * i + 1] = l * k_ + (1.0 - k_) * xr;
    }
    const double k = k_, a = a_, dry = 1.0 - k;
    if (k == 0.0) {   // the ramp to off ended inside this block: the rest passes through
        std::copy(in + 2 * static_cast<size_t>(i), in + 2 * static_cast<size_t>(frames), out + 2 * static_cast<size_t>(i));
        lp_l_ = lp_r_ = 0.0;
        return frames;
    }
    for (; i < frames; ++i) {
        const double xl = in[2 * i], xr = in[2 * i + 1];
        l = (xl - l) * a + l;
        r = (xr - r) * a + r;
        out[2 * i] = r * k + dry * xl;
        out[2 * i + 1] = l * k + dry * xr;
    }
    lp_l_ = l;
    lp_r_ = r;
    return frames;
}

}  // namespace daphne::dsp
