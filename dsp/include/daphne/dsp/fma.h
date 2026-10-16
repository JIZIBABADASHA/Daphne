#pragma once
// fma(a, b, c) = a * b + c rounded once, with or without the CPU's FMA instruction.
//
// The double-double arithmetic (dd.h, dd_ops.h) relies on the single rounding. std::fma gives it, but
// on CPUs without an FMA instruction (Intel before Haswell, AMD before Piledriver) the C library
// computes it in software, about 100 times slower: a filter design with a phase took seconds instead
// of tens of milliseconds there. On those CPUs fma_exact emulates it with plain double operations
// (Boldo and Melquiond, "Emulation of FMA and correctly rounded sums: proved algorithms using rounding
// to odd", 2008): the exact product by Dekker's split, the sum with c by two-sum, the two low parts
// added rounded to odd, then one rounding to nearest. The result is the same bits as a real FMA.
//
// The emulation needs no overflow and no underflow in its steps; outside a safe exponent range (and
// for zeros, infinities, NaN) it falls back to std::fma, which is exact anyway.
//
// DAPHNE_FMA_EMULATE=1 forces the emulation (tests: the same bits on a CPU that has FMA).

#include <cmath>
#include <cstdint>
#include <cstring>

namespace daphne::dsp {

namespace detail {

// The CPU has an FMA instruction and DAPHNE_FMA_EMULATE is not set (fma.cpp; false until the program's
// static initialization sets it, which only means the exact emulation is used meanwhile).
extern bool g_hw_fma;

inline void two_sum(double a, double b, double& s, double& t) {
    s = a + b;
    const double bb = s - a;
    t = (a - (s - bb)) + (b - bb);
}

// a * b = p + e exactly (Dekker / Veltkamp split into 26-bit halves)
inline void two_prod(double a, double b, double& p, double& e) {
    constexpr double kSplit = 134217729.0;   // 2^27 + 1
    p = a * b;
    double t = kSplit * a;
    const double ah = t - (t - a), al = a - ah;
    t = kSplit * b;
    const double bh = t - (t - b), bl = b - bh;
    e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

// a + b rounded to odd: when the sum is not exact, the neighbour with an odd last bit
inline double add_odd(double a, double b) {
    double s, t;
    two_sum(a, b, s, t);
    if (t == 0.0) return s;
    uint64_t u;
    std::memcpy(&u, &s, sizeof u);
    if ((u & 1) == 0) return std::nextafter(s, t > 0.0 ? INFINITY : -INFINITY);   // the exact sum lies on t's side of s
    return s;
}

inline double fma_emulated(double a, double b, double c) {
    // zeros: the exact product is a signed zero, so fma is c + a * b as computed
    if (a == 0.0 || b == 0.0) return c + a * b;
    const double fa = std::fabs(a), fb = std::fabs(b), fc = std::fabs(c), fp = fa * fb;
    // the steps neither overflow (the split, the sums) nor underflow (the low parts stay normal):
    // otherwise (and for infinities / NaN, which fail these tests) the library's exact fma
    if (!(fa < 0x1p995 && fb < 0x1p995 && fc < 0x1p1000 && fp < 0x1p900 && fp > 0x1p-900 && (fc == 0.0 || fc > 0x1p-900)))
        return std::fma(a, b, c);
    double uh, ul, th, tl;
    two_prod(a, b, uh, ul);
    two_sum(c, uh, th, tl);
    return th + add_odd(tl, ul);
}

}  // namespace detail

inline double fma_exact(double a, double b, double c) {
    if (detail::g_hw_fma) return std::fma(a, b, c);
    return detail::fma_emulated(a, b, c);
}

// What fma_exact uses (for logs and tests).
bool fma_in_hardware();

}  // namespace daphne::dsp
