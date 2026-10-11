#pragma once
// Radix-2 complex FFT with precomputed twiddles and bit-reversal table, for the runtime
// convolution in double (the filter design uses the double-double FFT of dd_conv.h).
// Unscaled in both directions.

#include <cmath>
#include <complex>
#include <vector>

namespace daphne::dsp {

class Fft {
public:
    explicit Fft(int n = 0) { resize(n); }

    void resize(int n) {
        n_ = n;
        tw_.resize(n / 2);
        for (int k = 0; k < n / 2; ++k) tw_[k] = std::polar(1.0, -2.0 * 3.14159265358979323846 * k / n);
        rev_.resize(n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[i] = r;
        }
    }

    int size() const { return n_; }

    // In place. inverse: conjugate twiddles, no 1/n scaling.
    void run(std::complex<double>* x, bool inverse) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len >> 1, step = n_ / len;
            for (int i = 0; i < n_; i += len) {
                for (int k = 0; k < half; ++k) {
                    std::complex<double> w = tw_[k * step];
                    if (inverse) w = std::conj(w);
                    const std::complex<double> v = x[i + k + half] * w;
                    x[i + k + half] = x[i + k] - v;
                    x[i + k] += v;
                }
            }
        }
    }

private:
    int n_ = 0;
    std::vector<std::complex<double>> tw_;
    std::vector<int> rev_;
};

}  // namespace daphne::dsp
