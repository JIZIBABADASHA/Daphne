// AVX2 + FMA3 kernels of the double-double convolution (compiled with /arch:AVX2; called only
// when the CPU has both). Same operations, in the same order, as the scalar path: same bits.

#include <immintrin.h>

#include "dd_conv.h"
#include "dd_ops.h"

namespace daphne::dsp::ddc::detail {

namespace {

struct V4 {
    __m256d v;
};

struct AvxOps {
    using T = V4;
    static T add(T a, T b) { return {_mm256_add_pd(a.v, b.v)}; }
    static T sub(T a, T b) { return {_mm256_sub_pd(a.v, b.v)}; }
    static T mul(T a, T b) { return {_mm256_mul_pd(a.v, b.v)}; }
    static T fma(T a, T b, T c) { return {_mm256_fmadd_pd(a.v, b.v, c.v)}; }
    static T neg(T a) { return {_mm256_xor_pd(a.v, _mm256_set1_pd(-0.0))}; }
};

using A = AvxOps;
using D4 = DDv<V4>;

inline D4 load(const std::vector<double>& h, const std::vector<double>& l, size_t i) {
    return {{_mm256_loadu_pd(h.data() + i)}, {_mm256_loadu_pd(l.data() + i)}};
}
inline void store(std::vector<double>& h, std::vector<double>& l, size_t i, D4 v) {
    _mm256_storeu_pd(h.data() + i, v.hi.v);
    _mm256_storeu_pd(l.data() + i, v.lo.v);
}

}  // namespace

void stages_avx2(Planes& x, const Planes& tw, int n, int first_half) {
    for (int half = first_half; half < n; half *= 2) {
        const size_t off = static_cast<size_t>(half) - 1;
        for (int i = 0; i < n; i += 2 * half)
            for (int j = 0; j < half; j += 4) {
                const size_t a = static_cast<size_t>(i + j), b = a + half;
                D4 tr, ti;
                cmul<A>(load(tw.rh, tw.rl, off + j), load(tw.ih, tw.il, off + j), load(x.rh, x.rl, b), load(x.ih, x.il, b), tr, ti);
                const D4 ar = load(x.rh, x.rl, a), ai = load(x.ih, x.il, a);
                store(x.rh, x.rl, b, dd_add<A>(ar, dd_neg<A>(tr)));
                store(x.ih, x.il, b, dd_add<A>(ai, dd_neg<A>(ti)));
                store(x.rh, x.rl, a, dd_add<A>(ar, tr));
                store(x.ih, x.il, a, dd_add<A>(ai, ti));
            }
    }
}

// Twiddles table[(j .. j + 3) * stride]: a plain load for stride 1, else a gather.
inline D4 load_tw(const std::vector<double>& h, const std::vector<double>& l, int64_t j, int64_t stride) {
    if (stride == 1) return load(h, l, static_cast<size_t>(j));
    const __m256i idx = _mm256_set_epi64x((j + 3) * stride, (j + 2) * stride, (j + 1) * stride, j * stride);
    return {{_mm256_i64gather_pd(h.data(), idx, 8)}, {_mm256_i64gather_pd(l.data(), idx, 8)}};
}

void butterflies_avx2(Planes& x, const Planes& table, int64_t stride, bool conj, int half, int64_t b0, int64_t b1) {
    for (int64_t bf = b0; bf < b1; bf += 4) {   // 4 butterflies of one block (half >= 4)
        const int64_t blk = bf / half, j = bf % half;
        const size_t a = static_cast<size_t>(blk * 2 * half + j), b = a + half;
        D4 wi = load_tw(table.ih, table.il, j, stride);
        if (conj) wi = dd_neg<A>(wi);
        D4 tr, ti;
        cmul<A>(load_tw(table.rh, table.rl, j, stride), wi, load(x.rh, x.rl, b), load(x.ih, x.il, b), tr, ti);
        const D4 ar = load(x.rh, x.rl, a), ai = load(x.ih, x.il, a);
        store(x.rh, x.rl, a, dd_add<A>(ar, tr));
        store(x.ih, x.il, a, dd_add<A>(ai, ti));
        store(x.rh, x.rl, b, dd_add<A>(ar, dd_neg<A>(tr)));
        store(x.ih, x.il, b, dd_add<A>(ai, dd_neg<A>(ti)));
    }
}

inline void bfly4(D4& ur, D4& ui, D4& vr, D4& vi, D4 wr, D4 wi) {
    D4 tr, ti;
    cmul<A>(wr, wi, vr, vi, tr, ti);
    const D4 ar = ur, ai = ui;
    ur = dd_add<A>(ar, tr), ui = dd_add<A>(ai, ti);
    vr = dd_add<A>(ar, dd_neg<A>(tr)), vi = dd_add<A>(ai, dd_neg<A>(ti));
}

void butterflies2_avx2(Planes& x, const Planes& table, int64_t nb, bool conj, int h, int64_t u0, int64_t u1) {
    const int64_t s1 = nb / h, s2 = nb / (2 * h);
    auto tw = [&](int64_t j, int64_t stride, D4& wr, D4& wi) {
        wr = load_tw(table.rh, table.rl, j, stride);
        wi = load_tw(table.ih, table.il, j, stride);
        if (conj) wi = dd_neg<A>(wi);
    };
    for (int64_t u = u0; u < u1; u += 4) {   // 4 units of one block (h >= 4)
        const int64_t blk = u / h, j = u % h;
        const size_t p0 = static_cast<size_t>(blk * 4 * h + j), p1 = p0 + h, p2 = p0 + 2 * h, p3 = p0 + 3 * h;
        D4 r0 = load(x.rh, x.rl, p0), i0 = load(x.ih, x.il, p0), r1 = load(x.rh, x.rl, p1), i1 = load(x.ih, x.il, p1);
        D4 r2 = load(x.rh, x.rl, p2), i2 = load(x.ih, x.il, p2), r3 = load(x.rh, x.rl, p3), i3 = load(x.ih, x.il, p3);
        D4 wr, wi;
        tw(j, s1, wr, wi);
        bfly4(r0, i0, r1, i1, wr, wi);
        bfly4(r2, i2, r3, i3, wr, wi);
        tw(j, s2, wr, wi);
        bfly4(r0, i0, r2, i2, wr, wi);
        tw(j + h, s2, wr, wi);
        bfly4(r1, i1, r3, i3, wr, wi);
        store(x.rh, x.rl, p0, r0), store(x.ih, x.il, p0, i0), store(x.rh, x.rl, p1, r1), store(x.ih, x.il, p1, i1);
        store(x.rh, x.rl, p2, r2), store(x.ih, x.il, p2, i2), store(x.rh, x.rl, p3, r3), store(x.ih, x.il, p3, i3);
    }
}

void mac_avx2(Planes& acc, const Planes& x, size_t xoff, const Planes& h, size_t hoff, int n) {
    for (int k = 0; k < n; k += 4) {
        D4 pr, pi;
        cmul<A>(load(x.rh, x.rl, xoff + k), load(x.ih, x.il, xoff + k), load(h.rh, h.rl, hoff + k), load(h.ih, h.il, hoff + k), pr, pi);
        store(acc.rh, acc.rl, static_cast<size_t>(k), dd_add<A>(load(acc.rh, acc.rl, static_cast<size_t>(k)), pr));
        store(acc.ih, acc.il, static_cast<size_t>(k), dd_add<A>(load(acc.ih, acc.il, static_cast<size_t>(k)), pi));
    }
}

}  // namespace daphne::dsp::ddc::detail
