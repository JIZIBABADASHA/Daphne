#include "daphne/dsp/filter_design.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "daphne/dsp/progress.h"
#include "dd.h"
#include "dd_conv.h"
#include "parallel.h"

namespace daphne::dsp {
namespace {

using dd::DD;
using DVec = std::vector<DD>;
using ddc::Planes;

inline DD re_at(const Planes& p, size_t i) { return DD(p.rh[i], p.rl[i]); }
inline DD im_at(const Planes& p, size_t i) { return DD(p.ih[i], p.il[i]); }
inline void set_re(Planes& p, size_t i, const DD& v) { p.rh[i] = v.hi, p.rl[i] = v.lo; }
inline void set_im(Planes& p, size_t i, const DD& v) { p.ih[i] = v.hi, p.il[i] = v.lo; }

// --------------------------------------------------------------------------- double-double FFT

// Twiddles of the size-n transform: n / 2 entries cos, sin of k * (-2 pi / n). Every stage of the
// FFT uses entries k * (n / len) of this table (the angle 2 pi / len is 2 pi / n times a power of
// two, so the dd argument and its sincos are the same bits), so one table serves all stages and
// all FFTs of a design.
struct TwiddleCache {
    int n = 0;
    Planes table;
};

const Planes& twiddles(TwiddleCache& c, int n) {
    if (c.n == n) return c.table;
    c.n = n;
    c.table.assign(static_cast<size_t>(n / 2));
    // -2 pi / n as a two-term dd / double division
    const DD full = -dd::kTwoPi;
    const double q1 = full.hi / n;
    const DD r = full - DD(q1) * DD(static_cast<double>(n));
    const DD ang = dd::quick_two_sum(q1, r.hi / n);
    Planes& t = c.table;
    parallel_for(n / 2, [&](int64_t b, int64_t e) {
        for (int64_t k = b; k < e; ++k) {
            DD s, co;
            dd::sincos_series(DD(static_cast<double>(k)) * ang, s, co);
            set_re(t, static_cast<size_t>(k), co);
            set_im(t, static_cast<size_t>(k), s);
        }
    }, 1024);
    return t;
}

// Radix-2 FFT in double-double on split planes; the inverse uses the conjugate twiddles and scales
// by 1 / n. Butterflies with AVX2 + FMA3 (ddc::butterflies / butterflies2) on several threads.
void fft_dd(Planes& x, bool inverse, TwiddleCache& cache) {
    const int n = static_cast<int>(x.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(x.rh[i], x.rh[j]), std::swap(x.rl[i], x.rl[j]);
            std::swap(x.ih[i], x.ih[j]), std::swap(x.il[i], x.il[j]);
        }
    }
    const Planes& tw = twiddles(cache, n);
    int stages = 0;
    for (int m = n; m > 1; m >>= 1) ++stages;
    int stage = 0;
    const int64_t nb = n / 2;   // butterflies per stage
    // Cache blocking: the stages up to len = kBlock stay inside blocks of kBlock points, so each
    // block (512 KB) runs all of them while it sits in the cache; only the later stages sweep the
    // whole array. Every butterfly gets the same inputs as in stage order: the same bits.
    constexpr int kBlock = 1 << 14;
    int len = 2;
    if (n > kBlock) {
        int inner = 0;
        for (int m = kBlock; m > 1; m >>= 1) ++inner;
        parallel_for(n / kBlock, [&](int64_t b, int64_t e) {
            for (int64_t blk = b; blk < e; ++blk)
                for (int l = 2; l <= kBlock; l <<= 1) {
                    const int half = l >> 1;
                    ddc::butterflies(x, tw, nb / half, inverse, half, blk * (kBlock / 2), (blk + 1) * (kBlock / 2));
                }
        }, 1);
        stage += inner;
        len = 2 * kBlock;
    }
    // the remaining stages two at a time (one pass over the array instead of two), a last single
    // stage when their count is odd
    while (len <= n) {
        progress(static_cast<double>(stage) / std::max(1, stages));
        const int half = len >> 1;
        if (2 * len <= n) {
            const int64_t units = n / 4;
            if (half >= 4) {   // groups of 4 units, aligned for AVX2
                parallel_for(units / 4, [&](int64_t b, int64_t e) { ddc::butterflies2(x, tw, nb, inverse, half, b * 4, e * 4); }, 256);
            } else {
                parallel_for(units, [&](int64_t b, int64_t e) { ddc::butterflies2(x, tw, nb, inverse, half, b, e); }, 1024);
            }
            stage += 2;
            len <<= 2;
            continue;
        }
        const int64_t stride = nb / half;
        if (half >= 4) {
            parallel_for(nb / 4, [&](int64_t b, int64_t e) { ddc::butterflies(x, tw, stride, inverse, half, b * 4, e * 4); }, 1024);
        } else {
            parallel_for(nb, [&](int64_t b, int64_t e) { ddc::butterflies(x, tw, stride, inverse, half, b, e); }, 4096);
        }
        ++stage;
        len <<= 1;
    }
    if (inverse) {
        const DD inv(1.0 / n);
        parallel_for(n, [&](int64_t b, int64_t e) {
            for (int64_t i = b; i < e; ++i) {
                set_re(x, static_cast<size_t>(i), re_at(x, static_cast<size_t>(i)) * inv);
                set_im(x, static_cast<size_t>(i), im_at(x, static_cast<size_t>(i)) * inv);
            }
        });
    }
}

// --------------------------------------------------------------------------- partial minimum phase

// Cepstrum of 0.5 log(|H|^2 + floor) (FFT length max(65536, nextpow2(8 L))), causal part x (1 + a),
// anti-causal part x (1 - a), complex exp, back to time. The L output samples start D before 0
// (wrapping): D is the smallest offset whose edge level (the larger of max|y| over the G samples
// just before and just after the window) is within 6 dB of the best over D in [K, min(N - L, L)].
// A minimum phase has next to nothing before its peak, so its edge level barely depends on D there
// and the least delay wins; an intermediate phase gets its heavier tail kept. Levels 240 dB below
// the peak count as equal. Raised-cosine fades over K = min(L / 16, 32) samples at both ends, DC
// gain 1.
DVec partial_min_phase(const DVec& h, int L, double alpha, const DD& floor) {
    DVec out(L, DD(0.0));
    if (L <= 0) return out;
    int N = 1;
    while (N < 8 * L) N += N;
    if (N < 65536) N = 65536;
    Planes X;
    X.assign(static_cast<size_t>(N));
    TwiddleCache tc;
    for (int i = 0; i < L && i < static_cast<int>(h.size()); ++i) set_re(X, static_cast<size_t>(i), h[i]);
    {
        ProgressStage st(0.0, 0.171);
        fft_dd(X, false, tc);
    }
    {
        ProgressStage st(0.171, 0.366);
        parallel_for(N, [&](int64_t b, int64_t e) {
            for (int64_t i = b; i < e; ++i) {
                const size_t u = static_cast<size_t>(i);
                const DD vr = re_at(X, u), vi = im_at(X, u);
                const DD m2 = (vi * vi + vr * vr) + floor;
                set_re(X, u, dd::log(m2) * DD(0.5));
                set_im(X, u, DD(0.0));
                if (((i - b) & 0x3fff) == 0) progress(static_cast<double>(i - b) / (e - b));
            }
        }, 1024);
    }
    {
        ProgressStage st(0.366, 0.537);
        fft_dd(X, true, tc);
    }
    const double a = std::clamp(alpha, 0.0, 1.0);
    const DD up(1.0 + a), down(1.0 - a);
    for (int k = 1; k < N; ++k) {
        if (k == N / 2) continue;
        const DD f = k < N / 2 ? up : down;
        const size_t u = static_cast<size_t>(k);
        set_re(X, u, re_at(X, u) * f);
        set_im(X, u, im_at(X, u) * f);
    }
    {
        ProgressStage st(0.537, 0.708);
        fft_dd(X, false, tc);
    }
    {
        ProgressStage st(0.708, 0.827);
        parallel_for(N, [&](int64_t b, int64_t e) {
            for (int64_t i = b; i < e; ++i) {
                const size_t u = static_cast<size_t>(i);
                const DD ex = dd::exp(re_at(X, u));
                DD s, c;
                dd::sincos(im_at(X, u), s, c);
                set_re(X, u, c * ex);
                set_im(X, u, s * ex);
                if (((i - b) & 0x3fff) == 0) progress(static_cast<double>(i - b) / (e - b));
            }
        }, 1024);
    }
    {
        ProgressStage st(0.827, 1.0);
        fft_dd(X, true, tc);
    }

    const int K = std::min(L >> 4, 32);
    const int maxD = std::min(N - L, L);
    const int G = std::max(L >> 4, 32);
    // ay[j] = |y[j - maxD - G]| (circular), j in [0, maxD + L + 2 G); m[j] = max ay[j .. j + G)
    const int span = maxD + L + 2 * G;
    std::vector<double> ay(static_cast<size_t>(span));
    for (int j = 0; j < span; ++j) {
        int idx = j - maxD - G;
        idx = ((idx % N) + N) % N;
        ay[static_cast<size_t>(j)] = std::abs(X.rh[static_cast<size_t>(idx)]);
    }
    std::vector<double> m(static_cast<size_t>(span - G + 1));
    std::vector<int> q(static_cast<size_t>(span));
    int qh = 0, qt = 0;
    for (int j = 0; j < span; ++j) {   // sliding maximum (monotone deque)
        while (qt > qh && ay[static_cast<size_t>(q[static_cast<size_t>(qt - 1)])] <= ay[static_cast<size_t>(j)]) --qt;
        q[static_cast<size_t>(qt++)] = j;
        if (q[static_cast<size_t>(qh)] <= j - G) ++qh;
        if (j >= G - 1) m[static_cast<size_t>(j - G + 1)] = ay[static_cast<size_t>(q[static_cast<size_t>(qh)])];
    }
    double peak = 0.0;
    for (double v : ay) peak = std::max(peak, v);
    const double tie = peak * 1e-12;
    auto edge = [&](int d) {
        // window start -d sits at j = maxD + G - d: before it [j - G, j), after it [j + L, j + L + G)
        const int j = maxD + G - d;
        return std::max({m[static_cast<size_t>(j - G)], m[static_cast<size_t>(j + L)], tie});
    };
    const int d0 = std::min(K, maxD);
    double best = edge(d0);
    for (int d = d0 + 1; d <= maxD; ++d) best = std::min(best, edge(d));
    int D = d0;
    while (D < maxD && edge(D) > 2.0 * best) ++D;

    DD sum(0.0);
    for (int n = 0; n < L; ++n) {
        int idx = n - D;
        if (idx < 0) idx += N;
        DD y = re_at(X, static_cast<size_t>(idx));
        int f = -1;
        if (n < K) f = n;
        else if (n >= L - K) f = L - n - 1;
        if (f >= 0) {
            const double w = (1.0 - std::cos(static_cast<double>(f) / K * 3.141592653589793)) * 0.5;
            y = y * DD(w);
        }
        out[n] = y;
        sum = sum + y;
    }
    if (std::abs(sum.hi) > 1e-30) {
        const DD inv = dd::div(DD(1.0), sum);
        for (auto& v : out) v = inv * v;
    }
    return out;
}

// --------------------------------------------------------------------------- Kaiser prototype

// Exponentially scaled modified Bessel I0, e^-x I0(x), in double-double (x >= 0): the power
// series below 40, the asymptotic series 1 / sqrt(2 pi x) sum ((2k - 1)!!)^2 / (k! (8x)^k) above
// (its smallest term is about e^-2x < 1e-34 there). Scaled, so the apodizing window's beta (up to
// about 1000 for long filters; I0 itself overflows a double beyond 713) is fine.
DD bessel_i0e(const DD& x) {
    if (x.hi < 40.0) {
        const DD q = x * x * DD(0.25);
        DD sum(1.0), term(1.0);
        for (int k = 1; k < 400; ++k) {
            term = dd::div(term * q, DD(static_cast<double>(k) * k));
            const DD next = sum + term;
            if (next.hi == sum.hi && next.lo == sum.lo) break;
            sum = next;
        }
        return sum * dd::exp(-x);
    }
    const DD inv8x = dd::div(DD(1.0), x * DD(8.0));
    DD sum(1.0), term(1.0);
    for (int k = 1; k < 200; ++k) {
        const double odd = 2.0 * k - 1.0;
        const DD next_term = dd::div(term * DD(odd * odd) * inv8x, DD(static_cast<double>(k)));
        if (std::abs(next_term.hi) >= std::abs(term.hi)) break;   // the series starts to diverge
        term = next_term;
        const DD next = sum + term;
        if (next.hi == sum.hi && next.lo == sum.lo) break;
        sum = next;
    }
    return dd::div(sum, dd::sqrt(x * DD(2.0) * dd::kPi));
}

// Kaiser-windowed sinc, DC gain 1: L taps centred at (L - 1) / 2, fc in cycles per output sample;
// the window is 0 at the ends.
DVec kaiser_sinc(int L, double fc, double beta) {
    DVec h(std::max(L, 0), DD(0.0));
    if (L <= 0) return h;
    const DD bb(beta);
    const DD i0b = bessel_i0e(bb);   // w = I0(beta sqrt r) / I0(beta) = e^(beta sqrt r - beta) I0e(.) / I0e(beta)
    const DD two_fc = DD(fc) + DD(fc);
    const double c = (L - 1) * 0.5;
    parallel_for(L, [&](int64_t b, int64_t e) {
        for (int n = static_cast<int>(b); n < e; ++n) {
            // r = 1 - x^2, x = (n - c) / c, so r = n (L - 1 - n) / c^2 exactly
            const DD r = dd::div(DD(static_cast<double>(n)) * DD(static_cast<double>(L - 1 - n)), DD(c) * DD(c));
            if (r.hi <= 0.0) continue;
            const DD z = bb * dd::sqrt(r);
            const DD w = dd::div(bessel_i0e(z), i0b) * dd::exp(z - bb);
            h[n] = w * dd::sinc(two_fc * DD(n - c)) * two_fc;
            if (((n - b) & 0x3fff) == 0) progress(static_cast<double>(n - b) / (e - b));
        }
    }, 1024);
    DD sum(0.0);
    for (const DD& v : h) sum += v;
    const DD inv = dd::div(DD(1.0), sum);
    for (auto& v : h) v = inv * v;
    return h;
}

constexpr double kPhaseRoom = 0.75;   // share of the taps the prototype gets when a phase is applied
constexpr double kApodizeBw = 1.5;    // DPSS half bandwidth / half the apodizing transition

}  // namespace

std::vector<double> design_lowpass(int taps, int ratio, double fs_in, double fs_out, double phase, bool apodize,
                                   std::vector<double>* lo) {
    const int L = std::max(taps * ratio, 1);
    if (ratio == 1 && !apodize) {   // nothing to filter: a unit impulse (bit-perfect)
        if (lo) lo->assign(L, 0.0);
        std::vector<double> h(L, 0.0);
        h[0] = 1.0;
        return h;
    }
    phase = std::clamp(phase, 0.0, 1.0);
    const int pt = phase > 0.0 ? std::max(1, static_cast<int>(std::lround(taps * kPhaseRoom))) : taps;
    const int Lp = pt * ratio;
    double fc, beta;
    if (apodize) {
        const double fp = 0.4535 * fs_in, fsb = 0.5 * fs_in;
        fc = (fp + fsb) * 0.5 / fs_out;
        beta = 3.141592653589793 * kApodizeBw * (fsb - fp) * 0.5 / fs_out * Lp;
    } else {
        const double A = std::clamp(16.0 * std::log10(std::max(Lp, 128)) + 105.0, 115.0, 185.0);
        beta = 0.1102 * (A - 8.7);
        const double width = (A - 7.95) / (14.36 * pt);
        fc = (0.5 - width * 0.5) * fs_in / fs_out;
    }
    DVec h;
    {
        ProgressStage st(0.0, phase > 0.0 ? 0.3 : 1.0);
        h = kaiser_sinc(Lp, fc, beta);
    }
    if (phase > 0.0) {
        ProgressStage st(0.3, 1.0);
        h = partial_min_phase(h, L, phase, DD(1e-20) * DD(1e-20));   // |H|^2 floor -400 dB
    }
    std::vector<double> out(h.size());
    for (size_t i = 0; i < h.size(); ++i) out[i] = h[i].hi;
    if (lo) {
        lo->resize(h.size());
        for (size_t i = 0; i < h.size(); ++i) (*lo)[i] = h[i].lo;
    }
    return out;
}

}  // namespace daphne::dsp
