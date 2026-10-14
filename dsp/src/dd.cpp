#include "dd.h"

namespace daphne::dsp::dd {
namespace {

// dd / double: q1 = t.hi / d; r = t - q1 d; q2 = r.hi / d; quick two-sum.
inline DD div_d(const DD& t, double d) {
    const double q1 = t.hi / d;
    const DD r = t - DD(q1) * DD(d);
    const double q2 = r.hi / d;
    return quick_two_sum(q1, q2);
}

// k = round(a / 2pi) from the product hi + (unnormalized) lo, rounded half away from zero
inline double periods(const DD& a) {
    const double p = a.hi * kInvTwoPi.hi;
    double e = fma_exact(kInvTwoPi.lo, a.hi, a.lo * kInvTwoPi.hi);
    e = (e + fma_exact(kInvTwoPi.hi, a.hi, -p)) + a.lo * kInvTwoPi.lo;
    return std::round(e + p);
}

// 1 / k!, k = 0 .. 10 (k! is exact in double)
struct InvFact {
    DD v[11];
    InvFact() {
        double f = 1.0;
        for (int k = 0; k <= 10; ++k) {
            if (k > 0) f *= k;
            v[k] = div(DD(1.0), DD(f));
        }
    }
};
const InvFact& inv_fact() {
    static const InvFact t;
    return t;
}

inline DD ldexp_dd(const DD& a, int e) { return DD(std::ldexp(a.hi, e), std::ldexp(a.lo, e)); }

}  // namespace

DD div(const DD& a, const DD& b) {
    const double q1 = a.hi / b.hi;
    const DD r = a - b * DD(q1);
    const double q2 = r.hi / b.hi;
    return quick_two_sum(q1, q2);
}

void sincos_series(const DD& a, DD& s, DD& c) {
    constexpr int kLastTerm = 81;   // |r| <= pi: r^81 / 81! < 1e-80
    const DD r = a + (-(DD(periods(a)) * kTwoPi));
    const DD neg_r2 = -(r * r);
    DD term = r, sum = r;
    for (int n = 3; n <= kLastTerm; n += 2) {
        term = div_d(neg_r2 * term, static_cast<double>((n - 1) * n));
        sum = sum + term;
        if (std::abs(term.hi) < 1e-33) break;
    }
    s = sum;
    term = DD(1.0);
    sum = DD(1.0);
    for (int n = 2; n < kLastTerm; n += 2) {
        term = div_d(neg_r2 * term, static_cast<double>((n - 1) * n));
        sum = sum + term;
        if (std::abs(term.hi) < 1e-33) break;
    }
    c = sum;
}

DD exp(const DD& a) {
    if (a.hi < -700.0) return DD(0.0);
    if (a.hi > 700.0) return DD(1e300);
    const double m = std::round(a.hi * 1.4426950408889634);
    const DD r = ldexp_dd(a + (-(DD(m) * kLn2)), -10);   // |r| <= ln2 / 2^11
    const InvFact& f = inv_fact();
    DD p = f.v[10];
    for (int k = 9; k >= 1; --k) p = p * r + f.v[k];
    DD s = p * r;   // expm1(r)
    for (int i = 0; i < 10; ++i) s = s * (s + DD(2.0));   // expm1(2x) = expm1(x) (expm1(x) + 2)
    return ldexp_dd(s + DD(1.0), static_cast<int>(m));
}

DD log(const DD& a) {
    if (a.hi <= 0.0) return DD(-1e300);
    const DD x(std::log(a.hi));
    return (x + a * exp(-x)) + DD(-1.0);
}

void sincos(const DD& a, DD& s, DD& c) {
    const DD r = a + (-(DD(periods(a)) * kTwoPi));                 // |r| <= pi
    const DD pio2 = DD(kTwoPi.hi * 0.25, kTwoPi.lo * 0.25);         // exact
    const double j = std::round(r.hi / pio2.hi);                    // -2 .. 2
    const DD t = r + (-(DD(j) * pio2));                             // |t| <= pi / 4
    const DD neg_t2 = -(t * t);
    DD term = t, st = t;
    for (int n = 3; n <= 41; n += 2) {
        term = div_d(neg_t2 * term, static_cast<double>((n - 1) * n));
        st = st + term;
        if (std::abs(term.hi) < 1e-33) break;
    }
    term = DD(1.0);
    DD ct(1.0);
    for (int n = 2; n <= 40; n += 2) {
        term = div_d(neg_t2 * term, static_cast<double>((n - 1) * n));
        ct = ct + term;
        if (std::abs(term.hi) < 1e-33) break;
    }
    switch (((static_cast<int>(j) % 4) + 4) % 4) {
        case 0: s = st, c = ct; break;
        case 1: s = ct, c = -st; break;
        case 2: s = -st, c = -ct; break;
        default: s = -ct, c = st; break;
    }
}

DD sinc(const DD& x) {
    if (std::abs(x.hi) < 1e-15) return DD(1.0);
    const DD px = x * kPi;
    DD s, c;
    sincos_series(px, s, c);
    return div(s, px);
}

}  // namespace daphne::dsp::dd
