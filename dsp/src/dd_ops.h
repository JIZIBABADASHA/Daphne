#pragma once
// Double-double primitives on separate hi / lo values, the same operation order as dd::DD's
// operator+ (IEEE-accurate add) and operator* (FMA exact product). A template over the value
// type so the scalar path (double) and the AVX2 path (V4, dd_conv_avx2.cpp) are one definition
// and give the same bits.

#include <cmath>

#include "daphne/dsp/fma.h"

namespace daphne::dsp::ddc {

template <class T>
struct DDv {
    T hi, lo;
};

// Scalar value-type operations.
struct ScalarOps {
    using T = double;
    static T add(T a, T b) { return a + b; }
    static T sub(T a, T b) { return a - b; }
    static T mul(T a, T b) { return a * b; }
    static T fma(T a, T b, T c) { return fma_exact(a, b, c); }   // the same bits with or without the instruction (fma.h)
    static T neg(T a) { return -a; }
};

template <class O>
inline DDv<typename O::T> dd_add(DDv<typename O::T> a, DDv<typename O::T> b) {
    using T = typename O::T;
    const T s = O::add(a.hi, b.hi);
    const T bb = O::sub(s, a.hi);
    const T e1 = O::add(O::sub(a.hi, O::sub(s, bb)), O::sub(b.hi, bb));
    const T t = O::add(a.lo, b.lo);
    const T tb = O::sub(t, a.lo);
    const T e2 = O::add(O::sub(a.lo, O::sub(t, tb)), O::sub(b.lo, tb));
    const T s2 = O::add(t, e1);
    const T h1 = O::add(s, s2);
    const T l1 = O::sub(s2, O::sub(h1, s));
    const T s3 = O::add(l1, e2);
    const T h2 = O::add(h1, s3);
    return {h2, O::sub(s3, O::sub(h2, h1))};
}

template <class O>
inline DDv<typename O::T> dd_neg(DDv<typename O::T> a) {
    return {O::neg(a.hi), O::neg(a.lo)};
}

template <class O>
inline DDv<typename O::T> dd_mul(DDv<typename O::T> a, DDv<typename O::T> b) {
    using T = typename O::T;
    const T p = O::mul(a.hi, b.hi);
    T e = O::fma(b.hi, a.hi, O::neg(p));
    const T t = O::fma(a.lo, b.hi, O::mul(a.hi, b.lo));
    e = O::add(O::add(t, e), O::mul(a.lo, b.lo));
    const T h = O::add(p, e);
    return {h, O::sub(e, O::sub(h, p))};
}

// Complex: (ar + i ai)(br + i bi)
template <class O>
inline void cmul(DDv<typename O::T> ar, DDv<typename O::T> ai, DDv<typename O::T> br, DDv<typename O::T> bi,
                 DDv<typename O::T>& rr, DDv<typename O::T>& ri) {
    rr = dd_add<O>(dd_mul<O>(ar, br), dd_neg<O>(dd_mul<O>(ai, bi)));
    ri = dd_add<O>(dd_mul<O>(ar, bi), dd_mul<O>(ai, br));
}

}  // namespace daphne::dsp::ddc
