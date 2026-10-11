#pragma once
// Double-double arithmetic (about 106-bit significand) for filter design and the high-precision
// convolution: a value is the unevaluated sum hi + lo with |lo| <= ulp(hi) / 2.
//
// Addition is the IEEE-accurate two-sum form; the product uses an FMA for the exact hi * hi
// part. The product is not commutative in its last bits (a.lo * b.hi goes through the FMA while
// a.hi * b.lo is rounded first); dd_ops.h repeats the same operation order for the vector paths,
// so scalar and AVX2 code give the same bits.

#include <cmath>

namespace daphne::dsp::dd {

struct DD {
    double hi = 0.0, lo = 0.0;
    DD() = default;
    constexpr DD(double h) : hi(h), lo(0.0) {}
    constexpr DD(double h, double l) : hi(h), lo(l) {}
};

inline DD quick_two_sum(double a, double b) {
    const double s = a + b;
    return {s, b - (s - a)};
}

inline DD operator+(const DD& a, const DD& b) {
    const double s = a.hi + b.hi;
    const double bb = s - a.hi;
    const double e1 = (a.hi - (s - bb)) + (b.hi - bb);
    const double t = a.lo + b.lo;
    const double tb = t - a.lo;
    const double e2 = (a.lo - (t - tb)) + (b.lo - tb);
    const double s2 = t + e1;
    const double h1 = s + s2;
    const double l1 = s2 - (h1 - s);
    const double s3 = l1 + e2;
    const double h2 = h1 + s3;
    return {h2, s3 - (h2 - h1)};
}

inline DD operator-(const DD& a) { return {-a.hi, -a.lo}; }
inline DD operator-(const DD& a, const DD& b) { return a + (-b); }

// p = a.hi*b.hi; e = fma(b.hi, a.hi, -p); t = fma(a.lo, b.hi, a.hi*b.lo);
// e = (t + e) + a.lo*b.lo; quick two-sum.
inline DD operator*(const DD& a, const DD& b) {
    const double p = a.hi * b.hi;
    double e = std::fma(b.hi, a.hi, -p);
    const double t = std::fma(a.lo, b.hi, a.hi * b.lo);
    e = (t + e) + a.lo * b.lo;
    const double h = p + e;
    return {h, e - (h - p)};
}

inline DD& operator+=(DD& a, const DD& b) { return a = a + b; }

// x = sqrt(a.hi), one Newton correction (a - x^2).hi / 2x. 0 for a.hi <= 0.
inline DD sqrt(const DD& a) {
    if (!(a.hi > 0.0)) return DD(0.0);
    const double x = std::sqrt(a.hi);
    const DD r = a + (-(DD(x) * DD(x)));
    return quick_two_sum(x, r.hi / (x + x));
}

// Two-term division: q1 = a.hi / b.hi; r = a - b q1; q2 = r.hi / b.hi.
DD div(const DD& a, const DD& b);

constexpr DD kPi{3.141592653589793, 1.2246467991473532e-16};
constexpr DD kTwoPi{6.283185307179586, 2.4492935982947064e-16};
constexpr DD kInvTwoPi{0.15915494309189535, -9.839338337591243e-18};
constexpr DD kLn2{0.6931471805599453, 2.3190468138462996e-17};

// sin / cos by argument reduction k = round(a / 2pi), r = a - k 2pi (|r| <= pi), then the Taylor
// series on r until a term falls below 1e-33. Used for the FFT twiddles, where its exact behaviour
// at |r| close to pi keeps a twiddle table bit-stable across sizes.
void sincos_series(const DD& a, DD& s, DD& c);

// Fast functions for the per-bin loops (about 1e-32 relative accuracy):
//   exp:    r = (a - m ln2) / 2^10, expm1(r) by a 10-term series, 10 doublings s = s (s + 2),
//           (1 + s) 2^m
//   log:    y = log(a.hi) in double, one Newton step y + a exp(-y) - 1 (each step doubles the
//           correct digits: one step reaches double-double from double)
//   sincos: the 2 pi reduction, then to |t| <= pi/4 by quadrant, series on t
DD exp(const DD& a);
DD log(const DD& a);
void sincos(const DD& a, DD& s, DD& c);

// sin(pi x) / (pi x); 1 when |x| < 1e-15.
DD sinc(const DD& x);

}  // namespace daphne::dsp::dd
