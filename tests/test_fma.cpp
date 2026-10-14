#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "daphne/dsp/fma.h"

using daphne::dsp::detail::fma_emulated;

namespace {

bool same_bits(double x, double y) {
    uint64_t a, b;
    std::memcpy(&a, &x, 8);
    std::memcpy(&b, &y, 8);
    return a == b;
}

// a double from 52 random mantissa bits, a random sign and a binary exponent in [e0, e1]
double random_double(std::mt19937_64& g, int e0, int e1) {
    const uint64_t m = g() & ((uint64_t{1} << 52) - 1);
    const int e = e0 + static_cast<int>(g() % static_cast<uint64_t>(e1 - e0 + 1));
    const double v = std::ldexp(1.0 + std::ldexp(static_cast<double>(m), -52), e);
    return (g() & 1) ? -v : v;
}

}  // namespace

TEST_CASE("FMA emulation: the same bits as std::fma", "[fma]") {
    std::mt19937_64 g(20261015);
    size_t n = 0, diff = 0;
    double bad_a = 0, bad_b = 0, bad_c = 0;
    auto check = [&](double a, double b, double c) {
        ++n;
        if (!same_bits(fma_emulated(a, b, c), std::fma(a, b, c))) {
            if (diff++ == 0) bad_a = a, bad_b = b, bad_c = c;
        }
    };
    // operands at similar scales, as double-double arithmetic has them
    for (int i = 0; i < 1000000; ++i) check(random_double(g, -60, 60), random_double(g, -60, 60), random_double(g, -120, 120));
    // c cancelling most of the product: the hard cases (the result comes from the low parts)
    for (int i = 0; i < 1000000; ++i) {
        const double a = random_double(g, -30, 30), b = random_double(g, -30, 30);
        const double p = a * b;
        const double nudge = std::ldexp(static_cast<double>(static_cast<int64_t>(g() % 2001) - 1000), std::ilogb(p) - 52);
        check(a, b, -p + nudge);
    }
    // the double-double product's own pattern: e = fma(b.hi, a.hi, -a.hi * b.hi) and fma(a.lo, b.hi, a.hi * b.lo)
    for (int i = 0; i < 1000000; ++i) {
        const double ah = random_double(g, -40, 40), bh = random_double(g, -40, 40);
        const double al = ah * std::ldexp(random_double(g, -1, -1), -53), bl = bh * std::ldexp(random_double(g, -1, -1), -53);
        check(bh, ah, -(ah * bh));
        check(al, bh, ah * bl);
    }
    // wide exponent ranges (the fallback to std::fma included), zeros, infinities, NaN
    for (int i = 0; i < 300000; ++i) check(random_double(g, -1074, 1023), random_double(g, -1074, 1023), random_double(g, -1074, 1023));
    const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
    for (double a : {0.0, -0.0, 1.5, -2.25, inf, -inf})
        for (double b : {0.0, -0.0, 3.0, -0.5, inf})
            for (double c : {0.0, -0.0, 1.0, -7.0, inf}) check(a, b, c);
    INFO("first mismatch: fma(" << bad_a << ", " << bad_b << ", " << bad_c << ")");
    CHECK(diff == 0);
    CHECK(n > 4000000);
    // NaN in, NaN out
    CHECK(std::isnan(fma_emulated(nan, 1.0, 2.0)));
    CHECK(std::isnan(fma_emulated(inf, 0.0, 1.0)));
}
