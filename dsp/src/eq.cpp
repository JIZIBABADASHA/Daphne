#include "daphne/dsp/eq.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace daphne::dsp {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kPi = 3.141592653589793;

// clamp that lets NaN through (a NaN setting must not silently become a limit value; the
// non-finite guard of the cascade then mutes the section)
double clampf(double x, double lo, double hi) { return std::isnan(x) ? x : std::min(std::max(x, lo), hi); }

// RBJ peaking. A = 10^(g/20); sqrt(A) is RBJ's A.
Biquad rbj_peaking(double A, double c, double s, double q) {
    const double r = std::sqrt(A);
    const double alpha = s / (q + q);
    const double x4 = alpha / r, x3 = alpha * r;
    const double a0 = x4 + 1.0;
    Biquad o;
    o.b0 = (x3 + 1.0) / a0;
    o.b1 = o.a1 = (c * -2.0) / a0;
    o.b2 = (1.0 - x3) / a0;
    o.a2 = (1.0 - x4) / a0;
    return o;
}

// Vicanek, "Matched Second Order Digital Filters" (2016), peaking EQ: poles matched to the analog
// prototype, zeros chosen so that the magnitude matches at DC and at w0 with zero slope there. The
// analog prototype is RBJ's (denominator Q = sqrt(G) Q), so the response continues the RBJ branch
// below 0.2 fs. Returns false on a degenerate design (the caller falls back to RBJ).
bool matched_peaking(double G, double w0, double q, Biquad& o) {
    const double z = 1.0 / (2.0 * std::sqrt(G) * q);
    const double e = std::exp(-z * w0);
    o.a1 = z <= 1.0 ? -2.0 * e * std::cos(std::sqrt(1.0 - z * z) * w0) : -2.0 * e * std::cosh(std::sqrt(z * z - 1.0) * w0);
    o.a2 = e * e;
    const double A0 = (1.0 + o.a1 + o.a2) * (1.0 + o.a1 + o.a2);
    const double A1 = (1.0 - o.a1 + o.a2) * (1.0 - o.a1 + o.a2);
    const double A2 = -4.0 * o.a2;
    const double sn = std::sin(w0 / 2.0);
    const double p1 = sn * sn, p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
    const double R1 = (A0 * p0 + A1 * p1 + A2 * p2) * G * G;
    const double R2 = (-A0 + A1 + 4.0 * (p0 - p1) * A2) * G * G;
    const double B0 = A0;
    const double B2 = (R1 - R2 * p1 - B0) / (4.0 * p1 * p1);
    const double B1 = R2 + B0 + 4.0 * (p1 - p0) * B2;
    const double W = 0.5 * (std::sqrt(B0) + std::sqrt(B1));
    o.b0 = 0.5 * (W + std::sqrt(W * W + B2));
    o.b1 = 0.5 * (std::sqrt(B0) - std::sqrt(B1));
    o.b2 = -B2 / (4.0 * o.b0);
    return std::isfinite(o.b0) && std::isfinite(o.b1) && std::isfinite(o.b2) && o.b0 != 0.0;
}

// Notch / all-pass / band-pass: RBJ, Q clamped to [0.1, 30].
Biquad rbj_q30(PeqType t, double fs, double f, double q) {
    f = clampf(f, 10.0, fs * 0.495);
    q = clampf(q, 0.1, 30.0);
    const double w = (f * kTwoPi) / fs;
    const double alpha = std::sin(w) / (q + q);
    const double a0 = alpha + 1.0;
    const double c = std::cos(w);
    Biquad o;
    o.b1 = o.a1 = (c * -2.0) / a0;
    if (t == PeqType::Notch) {
        o.b0 = o.b2 = 1.0 / a0;
        o.a2 = (1.0 - alpha) / a0;
    } else if (t == PeqType::AllPass) {
        o.b0 = o.a2 = (1.0 - alpha) / a0;
        o.b2 = 1.0;
    } else {
        o.b0 = alpha / a0;
        o.b1 = 0.0;
        o.b2 = (-alpha) / a0;
        o.a2 = (1.0 - alpha) / a0;
    }
    return o;
}

std::complex<double> response(const Biquad& q, double w) {
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    return (q.b0 + q.b1 * z1 + q.b2 * z2) / (1.0 + q.a1 * z1 + q.a2 * z2);
}

}  // namespace

Biquad peq_peaking(double fs, double f, double q, double gain_db) {
    if (std::abs(gain_db) < 1e-4) return {};
    f = clampf(f, 10.0, fs * 0.495);
    q = clampf(q, 0.05, 40.0);
    const double A = std::pow(10.0, gain_db / 20.0);
    const double w = (f * kTwoPi) / fs;
    const double c = std::cos(w);
    const double s = std::sin(w);
    // the analog prototype's gain at Nyquist: where it is still 1, RBJ is exact enough everywhere
    const double ny = fs * 0.5;
    const double t1 = std::pow(f / q, 2.0);
    const double d = std::pow(f * f - ny * ny, 2.0);
    const double den = (t1 * ny) * ny + d;
    double G = 1.0;
    if (den > 1e-12) {
        const double r = ((((A * A) * t1) * ny) * ny + d) / den;
        G = std::sqrt(r > 1e-12 ? r : 1e-12);
    }
    if (fs * 0.2 > f || 0.001 > std::abs(G - 1.0)) return rbj_peaking(A, c, s, q);
    Biquad o;
    if (matched_peaking(A, w, q, o)) return o;
    return rbj_peaking(A, c, s, q);
}

Biquad peq_shelf(bool high, double fs, double f, double S, double gain_db) {
    f = clampf(f, 10.0, fs * 0.495);
    S = clampf(S, 0.1, 10.0);
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w = (f * kTwoPi) / fs;
    const double c = std::cos(w);
    const double sA = std::sqrt(A);
    const double alpha = std::sin(w) * 0.5 * std::sqrt((1.0 / A + A) * (1.0 / S - 1.0) + 2.0);
    const double k = 2.0 * sA * alpha;
    const double ap = A + 1.0, am = A - 1.0;
    Biquad o;
    if (!high) {
        const double a0 = ap + am * c + k;
        o.b0 = A * (ap - am * c + k) / a0;
        o.b1 = 2.0 * A * (am - ap * c) / a0;
        o.b2 = A * (ap - am * c - k) / a0;
        o.a1 = -2.0 * (am + ap * c) / a0;
        o.a2 = (ap + am * c - k) / a0;
    } else {
        const double a0 = ap - am * c + k;
        o.b0 = A * (ap + am * c + k) / a0;
        o.b1 = -2.0 * A * (am + ap * c) / a0;
        o.b2 = A * (ap + am * c - k) / a0;
        o.a1 = 2.0 * (am - ap * c) / a0;
        o.a2 = (ap - am * c - k) / a0;
    }
    return o;
}

Biquad peq_highpass(double fs, double f, double q) {
    q = clampf(q, 0.1, 10.0);
    f = clampf(f, 10.0, fs * 0.495);
    const double w = (f * kTwoPi) / fs;
    const double alpha = std::sin(w) / (q + q);
    const double c = std::cos(w);
    const double x2 = c + 1.0;
    const double a0 = alpha + 1.0;
    Biquad o;
    o.b0 = o.b2 = (x2 * 0.5) / a0;
    o.b1 = (-x2) / a0;
    o.a1 = (c * -2.0) / a0;
    o.a2 = (1.0 - alpha) / a0;
    return o;
}

Biquad peq_lowpass(double fs, double f, double q) {
    q = clampf(q, 0.1, 10.0);
    f = clampf(f, 10.0, fs * 0.495);
    const double w = (f * kTwoPi) / fs;
    const double alpha = std::sin(w) / (q + q);
    const double c = std::cos(w);
    const double a0 = alpha + 1.0;
    const double x2 = 1.0 - c;
    Biquad o;
    o.b0 = o.b2 = (x2 * 0.5) / a0;
    o.b1 = x2 / a0;
    o.a1 = (c * -2.0) / a0;
    o.a2 = (1.0 - alpha) / a0;
    return o;
}

std::vector<Biquad> design_peq(const std::vector<PeqBand>& bands, double fs) {
    std::vector<Biquad> out;
    for (const PeqBand& b : bands) {
        if (!b.enabled) continue;
        switch (b.type) {
        case PeqType::Peaking: out.push_back(peq_peaking(fs, b.freq, b.q, b.gain_db)); break;
        case PeqType::LowShelf: out.push_back(peq_shelf(false, fs, b.freq, b.q, b.gain_db)); break;
        case PeqType::HighShelf: out.push_back(peq_shelf(true, fs, b.freq, b.q, b.gain_db)); break;
        case PeqType::HighPass12: out.push_back(peq_highpass(fs, b.freq, b.q)); break;
        case PeqType::HighPass24: {
            const Biquad h = peq_highpass(fs, b.freq, 0.7071);
            out.push_back(h);
            out.push_back(h);
            break;
        }
        case PeqType::LowPass12: out.push_back(peq_lowpass(fs, b.freq, b.q)); break;
        case PeqType::LowPass24: {
            const Biquad l = peq_lowpass(fs, b.freq, 0.7071);
            out.push_back(l);
            out.push_back(l);
            break;
        }
        case PeqType::Notch:
        case PeqType::AllPass:
        case PeqType::BandPass: out.push_back(rbj_q30(b.type, fs, b.freq, b.q)); break;
        default: break;
        }
    }
    return out;
}

double peq_magnitude(const std::vector<Biquad>& sections, double f, double fs) {
    const double w = kTwoPi * f / fs;
    std::complex<double> h = 1.0;
    for (const Biquad& q : sections) h *= response(q, w);
    return std::abs(h);
}

double peq_auto_preamp_db(const std::vector<PeqBand>& bands, const std::vector<Biquad>& sections, double fs) {
    std::vector<double> grid;
    grid.reserve(501 + bands.size());
    for (int i = 0; i <= 500; ++i) grid.push_back(std::pow(1000.0, i / 500.0) * 20.0);
    for (const PeqBand& b : bands)
        if (b.enabled) grid.push_back(b.freq);
    double peak = 0.0;
    for (double f : grid) {
        const double m = peq_magnitude(sections, f, fs);
        if (!std::isfinite(m)) continue;
        peak = std::max(peak, 20.0 * std::log10(std::max(m, 1e-9)));
    }
    // step overshoot of a boosting peaking / shelf band with Q above Butterworth (damping z < 0.7071)
    double over = 0.0;
    for (const PeqBand& b : bands) {
        if (!b.enabled || b.gain_db <= 0.0) continue;
        if (b.type != PeqType::Peaking && b.type != PeqType::LowShelf && b.type != PeqType::HighShelf) continue;
        if (!(b.q > 0.7071)) continue;
        const double z = 1.0 / (b.q + b.q);
        const double A = std::pow(10.0, b.gain_db / 20.0);
        const double e = std::exp((z * -kPi) / std::sqrt(1.0 - z * z));
        over = std::max(over, 20.0 * std::log10(((A - 1.0) * (e + 1.0) + 1.0) / A));
    }
    const double total = over + peak;
    return total > 0.0 ? -total - 0.2 : 0.0;
}

// --------------------------------------------------------------------------------------- Peq

void Peq::set_params(std::unique_ptr<PeqParams> p) {
    if (!p) return;
    if (p->sections.size() > static_cast<size_t>(kMaxSections)) p->sections.resize(kMaxSections);
    want_active_.store(p->enabled && !p->sections.empty(), std::memory_order_relaxed);
    box_.post(std::move(p));
}

void Peq::prepare(const Format& in) {
    fmt_ = in;
    const int ch = std::max(1, in.channels);
    cur_.assign(kMaxSections, Biquad{});
    target_.assign(kMaxSections, Biquad{});
    old_.assign(kMaxSections, Biquad{});
    state_.assign(static_cast<size_t>(kMaxSections) * ch * 4, 0.0);
    old_state_ = state_;
    tmp_.assign(static_cast<size_t>(std::max(1, in.max_frames)) * ch, 0.0);
    tick_frames_ = std::max(1, static_cast<int>(std::lround(0.015 * in.rate)));
    fade_len_ = std::max(1, static_cast<int>(std::lround(0.05 * in.rate)));
    reset();
}

// Takes `p` as the configuration: jump = at once with a cleared state (reset), otherwise glide or
// crossfade.
void Peq::apply(const PeqParams& p, bool jump) {
    const int n = std::min<int>(static_cast<int>(p.sections.size()), kMaxSections);
    const bool active = p.enabled && n > 0;
    std::copy(p.sections.begin(), p.sections.begin() + n, target_.begin());
    target_pre_ = p.preamp;
    if (jump) {
        std::fill(state_.begin(), state_.end(), 0.0);
        nsec_ = n;
        std::copy(target_.begin(), target_.begin() + n, cur_.begin());
        preamp_ = target_pre_;
        active_ = active;
        gliding_ = fading_ = false;
        tick_pos_ = 0;
        return;
    }
    if (n != nsec_ || active != active_) {   // crossfade from the old configuration
        fading_ = true;
        fade_pos_ = 0;
        old_active_ = active_;
        old_nsec_ = nsec_;
        std::copy(cur_.begin(), cur_.begin() + nsec_, old_.begin());
        old_state_.swap(state_);
        old_pre_ = preamp_;
        std::fill(state_.begin(), state_.end(), 0.0);
        nsec_ = n;
        std::copy(target_.begin(), target_.begin() + n, cur_.begin());
        preamp_ = target_pre_;
        active_ = active;
        gliding_ = false;
    } else {
        gliding_ = true;
        tick_pos_ = 0;
    }
}

void Peq::reset() {
    // a reset (seek, new track) jumps straight to the newest parameters
    if (const PeqParams* p = box_.take()) apply(*p, true);
    else if (const PeqParams* c = box_.current()) apply(*c, true);
    else apply(PeqParams{}, true);
}

bool Peq::is_identity() const { return !fading_ && !active_ && !want_active_.load(std::memory_order_relaxed); }

void Peq::glide_step() {
    double worst = 0.0;
    for (int i = 0; i < nsec_; ++i) {
        double* c = &cur_[i].b0;
        const double* g = &target_[i].b0;
        for (int k = 0; k < 5; ++k) {
            const double d = g[k] - c[k];
            c[k] = d * 0.28 + c[k];
            worst = std::max(worst, std::abs(d));
        }
    }
    const double d = target_pre_ - preamp_;
    preamp_ = d * 0.28 + preamp_;
    worst = std::max(std::abs(d), worst);
    if (worst < 1e-5) {
        std::copy(target_.begin(), target_.begin() + nsec_, cur_.begin());
        preamp_ = target_pre_;
        gliding_ = false;
    }
}

void Peq::filter_run(const Biquad* secs, int nsec, double* state, double pre, const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    const size_t n = static_cast<size_t>(frames) * ch;
    for (size_t i = 0; i < n; ++i) out[i] = in[i] * pre;
    for (int s = 0; s < nsec; ++s) {
        const Biquad q = secs[s];
        double* st = state + static_cast<size_t>(s) * ch * 4;
        bool ok = true;
        for (int c = 0; c < ch; ++c) {
            double* sc = st + c * 4;
            double x1 = sc[0], x2 = sc[1], y1 = sc[2], y2 = sc[3];
            double* v = out + c;
            for (int i = 0; i < frames; ++i, v += ch) {
                const double x = *v;
                const double y = q.b0 * x + q.b1 * x1 + q.b2 * x2 - q.a1 * y1 - q.a2 * y2;
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                *v = y;
            }
            sc[0] = x1, sc[1] = x2, sc[2] = y1, sc[3] = y2;
            ok = ok && std::abs(y1) < 1e9 && std::abs(y2) < 1e9;   // false for NaN as well
        }
        if (!ok) {   // unstable or overflowing: clear the section, silence for this block
            std::fill(st, st + static_cast<size_t>(ch) * 4, 0.0);
            std::fill(out, out + n, 0.0);
        }
    }
}

int Peq::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    if (const PeqParams* p = box_.take()) apply(*p, false);
    if (fading_) {   // the previous configuration, from the same input
        if (old_active_) filter_run(old_.data(), old_nsec_, old_state_.data(), old_pre_, in, frames, tmp_.data());
        else std::copy(in, in + static_cast<size_t>(frames) * ch, tmp_.begin());
    }
    if (!active_) {
        if (in != out) std::copy(in, in + static_cast<size_t>(frames) * ch, out);
    } else {
        int f = 0;
        while (f < frames) {
            if (gliding_ && tick_pos_ == 0) glide_step();
            int run = frames - f;
            if (gliding_) run = std::min(run, tick_frames_ - tick_pos_);
            filter_run(cur_.data(), nsec_, state_.data(), preamp_, in + static_cast<size_t>(f) * ch, run,
                       out + static_cast<size_t>(f) * ch);
            f += run;
            if (gliding_) tick_pos_ = (tick_pos_ + run) % tick_frames_;
        }
    }
    if (fading_) {
        for (int i = 0; i < frames; ++i) {
            const double t = std::min(1.0, (fade_pos_ + i + 0.5) / fade_len_);
            const double g = 0.5 - 0.5 * std::cos(t * kPi);
            for (int c = 0; c < ch; ++c) {
                const size_t k = static_cast<size_t>(i) * ch + c;
                out[k] = tmp_[k] * (1.0 - g) + out[k] * g;
            }
        }
        fade_pos_ += frames;
        if (fade_pos_ >= fade_len_) fading_ = false;
    }
    return frames;
}

}  // namespace daphne::dsp
