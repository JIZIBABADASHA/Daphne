#include "daphne/dsp/crossfeed.h"

#include <algorithm>
#include <cmath>

namespace daphne::dsp {

void Crossfeed::set_params(bool enabled, double amount, double fc) {
    amount_.store(std::isnan(amount) ? 0.0 : std::clamp(amount, 0.0, 1.0), std::memory_order_relaxed);
    fc_.store(std::isnan(fc) ? 700.0 : std::clamp(fc, 100.0, 3000.0), std::memory_order_relaxed);
    enabled_.store(enabled, std::memory_order_relaxed);
}

void Crossfeed::prepare(const Format& in) {
    fmt_ = in;
    reset();
}

void Crossfeed::reset() { lp_l_ = lp_r_ = 0.0; }

int Crossfeed::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    if (ch != 2 || !enabled_.load(std::memory_order_relaxed)) {
        if (in != out) std::copy(in, in + static_cast<size_t>(frames) * ch, out);
        return frames;
    }
    const double k = amount_.load(std::memory_order_relaxed), fc = fc_.load(std::memory_order_relaxed);
    const double dt = 1.0 / static_cast<double>(fmt_.rate);
    const double a = dt / (1.0 / (fc * 6.283185307179586) + dt);
    const double dry = 1.0 - k;
    double l = lp_l_, r = lp_r_;
    for (int i = 0; i < frames; ++i) {
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
