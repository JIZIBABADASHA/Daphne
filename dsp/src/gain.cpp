#include "daphne/dsp/gain.h"

#include <algorithm>
#include <cmath>

namespace daphne::dsp {

double Gain::db_to_linear(double db) { return std::pow(10.0, db / 20.0); }

void Gain::prepare(const Format& in) {
    fmt_ = in;
    reset();
}

void Gain::reset() {
    target_db_ = db();
    target_ = db_to_linear(target_db_);
    current_ = target_;
    step_ = 0.0;
}

bool Gain::is_identity() const { return db() == 0.0 && current_ == 1.0; }

int Gain::process(const double* in, int frames, double* out) {
    const double db_now = db();
    if (db_now != target_db_) {   // a new target: the ramp starts here, over ramp_ms whatever the block size
        target_db_ = db_now;
        target_ = db_to_linear(db_now);
        step_ = (target_ - current_) / std::max(1.0, ramp_ms_ * 1e-3 * fmt_.rate);
    }
    const double target = target_;
    const int ch = fmt_.channels;
    const size_t n = static_cast<size_t>(frames) * ch;

    if (current_ == target) {
        if (target == 1.0) {
            if (in != out) std::copy(in, in + n, out);
        } else {
            for (size_t i = 0; i < n; ++i) out[i] = in[i] * target;
        }
        return frames;
    }

    int f = 0;
    for (; f < frames && current_ != target; ++f) {
        current_ += step_;
        if ((step_ > 0 && current_ >= target) || (step_ < 0 && current_ <= target) || step_ == 0.0) current_ = target;
        for (int c = 0; c < ch; ++c) out[static_cast<size_t>(f) * ch + c] = in[static_cast<size_t>(f) * ch + c] * current_;
    }
    // the rest of the block at the target, without the per-frame test
    for (size_t i = static_cast<size_t>(f) * ch; i < n; ++i) out[i] = in[i] * target;
    return frames;
}

}  // namespace daphne::dsp
