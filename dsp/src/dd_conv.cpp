#include "dd_conv.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

#include "daphne/platform/platform.h"
#include "dd.h"
#include "dd_ops.h"

#if DAPHNE_DD_AVX2 && defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#endif

namespace daphne::dsp::ddc {

namespace {

std::atomic<bool> g_force_scalar{false};

bool detect_avx2_fma() {
#if DAPHNE_DD_AVX2 && defined(_MSC_VER)   // cpuid exists on x86 only (not on MSVC ARM64)
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!fma || !osxsave || !avx) return false;
    if ((_xgetbv(0) & 0x6) != 0x6) return false;   // the OS saves the YMM state
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;   // AVX2
#elif DAPHNE_DD_AVX2
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return false;   // not x86: the scalar path
#endif
}

using S = ScalarOps;
using D = DDv<double>;

inline D at(const std::vector<double>& h, const std::vector<double>& l, size_t i) { return {h[i], l[i]}; }
inline void put(std::vector<double>& h, std::vector<double>& l, size_t i, D v) { h[i] = v.hi, l[i] = v.lo; }

// One butterfly stage (half h, twiddles at offset h - 1), scalar.
void stage_scalar(Planes& x, const Planes& tw, int n, int half) {
    const size_t off = static_cast<size_t>(half) - 1;
    for (int i = 0; i < n; i += 2 * half)
        for (int j = 0; j < half; ++j) {
            const size_t a = static_cast<size_t>(i + j), b = a + half;
            D tr, ti;
            cmul<S>(at(tw.rh, tw.rl, off + j), at(tw.ih, tw.il, off + j), at(x.rh, x.rl, b), at(x.ih, x.il, b), tr, ti);
            const D ar = at(x.rh, x.rl, a), ai = at(x.ih, x.il, a);
            put(x.rh, x.rl, b, dd_add<S>(ar, dd_neg<S>(tr)));
            put(x.ih, x.il, b, dd_add<S>(ai, dd_neg<S>(ti)));
            put(x.rh, x.rl, a, dd_add<S>(ar, tr));
            put(x.ih, x.il, a, dd_add<S>(ai, ti));
        }
}

}  // namespace

bool have_avx2_fma() {
    // DAPHNE_DD_SCALAR=1 forces the scalar path (benchmarks)
    static const bool ok = detect_avx2_fma() && [] {
        return !platform::get_env("DAPHNE_DD_SCALAR").has_value();
    }();
    return ok && !g_force_scalar.load(std::memory_order_relaxed);
}

void set_force_scalar(bool on) { g_force_scalar = on; }

void Planes::zero() {
    std::fill(rh.begin(), rh.end(), 0.0);
    std::fill(rl.begin(), rl.end(), 0.0);
    std::fill(ih.begin(), ih.end(), 0.0);
    std::fill(il.begin(), il.end(), 0.0);
}

void Fft::resize(int n) {
    n_ = n;
    rev_.resize(static_cast<size_t>(n));
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        rev_[static_cast<size_t>(i)] = r;
    }
    fw_.assign(static_cast<size_t>(std::max(n - 1, 0)));
    bw_.assign(static_cast<size_t>(std::max(n - 1, 0)));
    for (int half = 1; half < n; half *= 2) {
        const size_t off = static_cast<size_t>(half) - 1;
        const double scale = 1.0 / (2.0 * half);   // a power of two: exact
        for (int j = 0; j < half; ++j) {
            // angle 2 pi j / (2 half), in double-double with the exact 2 pi
            dd::DD a = dd::kTwoPi * dd::DD(static_cast<double>(j));
            a = dd::DD(a.hi * scale, a.lo * scale);
            dd::DD s, c;
            dd::sincos_series(a, s, c);
            fw_.rh[off + j] = c.hi, fw_.rl[off + j] = c.lo, fw_.ih[off + j] = -s.hi, fw_.il[off + j] = -s.lo;
            bw_.rh[off + j] = c.hi, bw_.rl[off + j] = c.lo, bw_.ih[off + j] = s.hi, bw_.il[off + j] = s.lo;
        }
    }
}

void Fft::run(Planes& x, bool inverse) const {
    const int n = n_;
    for (int i = 0; i < n; ++i) {
        const int r = rev_[static_cast<size_t>(i)];
        if (r > i) {
            std::swap(x.rh[i], x.rh[r]), std::swap(x.rl[i], x.rl[r]);
            std::swap(x.ih[i], x.ih[r]), std::swap(x.il[i], x.il[r]);
        }
    }
    const Planes& tw = inverse ? bw_ : fw_;
    if (have_avx2_fma() && n >= 8) {
        stage_scalar(x, tw, n, 1);
        stage_scalar(x, tw, n, 2);
        detail::stages_avx2(x, tw, n, 4);
        return;
    }
    for (int half = 1; half < n; half *= 2) stage_scalar(x, tw, n, half);
}

void butterflies(Planes& x, const Planes& table, int64_t stride, bool conj, int half, int64_t b0, int64_t b1) {
    if (have_avx2_fma() && half >= 4 && b0 % 4 == 0 && b1 % 4 == 0) {
        detail::butterflies_avx2(x, table, stride, conj, half, b0, b1);
        return;
    }
    for (int64_t bf = b0; bf < b1; ++bf) {
        const int64_t blk = bf / half, j = bf % half;
        const size_t a = static_cast<size_t>(blk * 2 * half + j), b = a + half;
        const size_t t = static_cast<size_t>(j * stride);
        D wi = at(table.ih, table.il, t);
        if (conj) wi = dd_neg<S>(wi);
        D tr, ti;
        cmul<S>(at(table.rh, table.rl, t), wi, at(x.rh, x.rl, b), at(x.ih, x.il, b), tr, ti);
        const D ar = at(x.rh, x.rl, a), ai = at(x.ih, x.il, a);
        put(x.rh, x.rl, a, dd_add<S>(ar, tr));
        put(x.ih, x.il, a, dd_add<S>(ai, ti));
        put(x.rh, x.rl, b, dd_add<S>(ar, dd_neg<S>(tr)));
        put(x.ih, x.il, b, dd_add<S>(ai, dd_neg<S>(ti)));
    }
}

namespace {

// one butterfly on values in registers: (u, v) -> (u + w v, u - w v)
inline void bfly(D& ur, D& ui, D& vr, D& vi, D wr, D wi) {
    D tr, ti;
    cmul<S>(wr, wi, vr, vi, tr, ti);
    const D ar = ur, ai = ui;
    ur = dd_add<S>(ar, tr), ui = dd_add<S>(ai, ti);
    vr = dd_add<S>(ar, dd_neg<S>(tr)), vi = dd_add<S>(ai, dd_neg<S>(ti));
}

}  // namespace

void butterflies2(Planes& x, const Planes& table, int64_t nb, bool conj, int h, int64_t u0, int64_t u1) {
    if (have_avx2_fma() && h >= 4 && u0 % 4 == 0 && u1 % 4 == 0) {
        detail::butterflies2_avx2(x, table, nb, conj, h, u0, u1);
        return;
    }
    const int64_t s1 = nb / h, s2 = nb / (2 * h);
    auto tw = [&](int64_t idx, D& wr, D& wi) {
        wr = at(table.rh, table.rl, static_cast<size_t>(idx));
        wi = at(table.ih, table.il, static_cast<size_t>(idx));
        if (conj) wi = dd_neg<S>(wi);
    };
    for (int64_t u = u0; u < u1; ++u) {
        const int64_t blk = u / h, j = u % h;
        const size_t p0 = static_cast<size_t>(blk * 4 * h + j), p1 = p0 + h, p2 = p0 + 2 * h, p3 = p0 + 3 * h;
        D r0 = at(x.rh, x.rl, p0), i0 = at(x.ih, x.il, p0), r1 = at(x.rh, x.rl, p1), i1 = at(x.ih, x.il, p1);
        D r2 = at(x.rh, x.rl, p2), i2 = at(x.ih, x.il, p2), r3 = at(x.rh, x.rl, p3), i3 = at(x.ih, x.il, p3);
        D wr, wi;
        tw(j * s1, wr, wi);
        bfly(r0, i0, r1, i1, wr, wi);
        bfly(r2, i2, r3, i3, wr, wi);
        tw(j * s2, wr, wi);
        bfly(r0, i0, r2, i2, wr, wi);
        tw((j + h) * s2, wr, wi);
        bfly(r1, i1, r3, i3, wr, wi);
        put(x.rh, x.rl, p0, r0), put(x.ih, x.il, p0, i0), put(x.rh, x.rl, p1, r1), put(x.ih, x.il, p1, i1);
        put(x.rh, x.rl, p2, r2), put(x.ih, x.il, p2, i2), put(x.rh, x.rl, p3, r3), put(x.ih, x.il, p3, i3);
    }
}

void mac(Planes& acc, const Planes& x, size_t xoff, const Planes& h, size_t hoff, int n) {
    if (have_avx2_fma()) {
        detail::mac_avx2(acc, x, xoff, h, hoff, n);
        return;
    }
    for (int k = 0; k < n; ++k) {
        D pr, pi;
        cmul<S>(at(x.rh, x.rl, xoff + k), at(x.ih, x.il, xoff + k), at(h.rh, h.rl, hoff + k), at(h.ih, h.il, hoff + k), pr, pi);
        put(acc.rh, acc.rl, k, dd_add<S>(at(acc.rh, acc.rl, k), pr));
        put(acc.ih, acc.il, k, dd_add<S>(at(acc.ih, acc.il, k), pi));
    }
}

}  // namespace daphne::dsp::ddc
