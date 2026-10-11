#pragma once
// Double-double (about 106-bit significand) FFT convolution for the resampler's high-precision
// option, and the FFT kernels of the double-double filter design. Complex arrays are stored as four planes (re.hi, re.lo, im.hi, im.lo) so that AVX2 +
// FMA3 can process four values at a time (dd_conv_avx2.cpp); a scalar path with the same
// operation order gives the same bits on CPUs without them.
//
// Arithmetic: dd::operator+ (IEEE-accurate add) and dd::operator* (FMA exact product), applied
// element by element; complex products as (ar br - ai bi, ar bi + ai br).

#include <cstdint>
#include <vector>

namespace daphne::dsp::ddc {

struct Planes {
    std::vector<double> rh, rl, ih, il;
    void assign(size_t n) {
        rh.assign(n, 0.0), rl.assign(n, 0.0), ih.assign(n, 0.0), il.assign(n, 0.0);
    }
    void zero();
    size_t size() const { return rh.size(); }
};

// AVX2 + FMA3 present (checked once; the vector path is used only then).
bool have_avx2_fma();
// For tests: force the scalar path.
void set_force_scalar(bool on);

// Radix-2 complex FFT in double-double, unscaled both ways. Twiddles exp(-+2 pi i j / len) are
// computed in double-double with exact 2 pi (dd::kTwoPi), stored per stage, contiguous.
class Fft {
public:
    explicit Fft(int n = 0) { resize(n); }
    void resize(int n);
    int size() const { return n_; }
    void run(Planes& x, bool inverse) const;

private:
    int n_ = 0;
    std::vector<int> rev_;
    Planes fw_, bw_;   // twiddles per stage: stage with half h at offset h - 1
};

// acc += x * h over n complex values (n a multiple of 4).
void mac(Planes& acc, const Planes& x, size_t xoff, const Planes& h, size_t hoff, int n);

// Butterflies [b0, b1) of one radix-2 stage (butterfly b: block b / half, offset j = b % half;
// u = x[block * 2 half + j], v = x[... + half]; t = w v; u + t, u - t), the twiddle w being
// table[j * stride], with its imaginary part negated when conj. Several threads may run disjoint
// ranges of a stage. AVX2 for half >= 4 when b0, b1 are multiples of 4.
void butterflies(Planes& x, const Planes& table, int64_t stride, bool conj, int half, int64_t b0, int64_t b1);

// Two consecutive stages (halves h and 2 h) fused: unit u (block u / h, offset j = u % h) loads
// the four points j, j + h, j + 2h, j + 3h of its block of 4 h, runs the stage-h butterflies
// (0, 1) and (2, 3) with twiddle table[j * nb / h], then the stage-2h butterflies (0, 2) with
// table[j * nb / 2h] and (1, 3) with table[(j + h) * nb / 2h], and stores once: the same
// butterflies as the two stages one after the other (same bits), half the memory passes.
// nb = n / 2 (the table's size). Units [u0, u1); AVX2 for h >= 4 when u0, u1 are multiples of 4.
void butterflies2(Planes& x, const Planes& table, int64_t nb, bool conj, int h, int64_t u0, int64_t u1);

namespace detail {
// AVX2 + FMA3 kernels (dd_conv_avx2.cpp)
void stages_avx2(Planes& x, const Planes& tw, int n, int first_half);
void mac_avx2(Planes& acc, const Planes& x, size_t xoff, const Planes& h, size_t hoff, int n);
void butterflies_avx2(Planes& x, const Planes& table, int64_t stride, bool conj, int half, int64_t b0, int64_t b1);
void butterflies2_avx2(Planes& x, const Planes& table, int64_t nb, bool conj, int h, int64_t u0, int64_t u1);
}  // namespace detail

}  // namespace daphne::dsp::ddc
