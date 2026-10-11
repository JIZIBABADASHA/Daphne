#include <catch_amalgamated.hpp>

#include <atomic>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

#include "daphne/dsp/filter_design.h"
#include "daphne/dsp/progress.h"

using daphne::dsp::design_lowpass;

namespace {

// |H(f)| of h at frequency f (fs = sample rate), direct DTFT
double mag(const std::vector<double>& h, double f, double fs) {
    std::complex<double> s = 0.0;
    const double w = -2.0 * 3.14159265358979323846 * f / fs;
    for (size_t n = 0; n < h.size(); ++n) s += h[n] * std::polar(1.0, w * static_cast<double>(n));
    return std::abs(s);
}

double db(double x) { return 20.0 * std::log10(x + 1e-300); }

// Neumaier-compensated sum (a plain double sum of 4096 coefficients drifts by ~1e-15)
double sum_comp(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0.0, c = 0.0;
    auto add = [&](double v) {
        const double t = s + v;
        c += std::abs(s) >= std::abs(v) ? (s - t) + v : (v - t) + s;
        s = t;
    };
    for (size_t i = 0; i < a.size(); ++i) add(a[i]), add(b[i]);
    return s + c;
}

}  // namespace

TEST_CASE("Filter design: ratio 1 without apodizing is a unit impulse", "[design]") {
    std::vector<double> lo;
    const auto h = design_lowpass(4096, 1, 44100, 44100, 0.5, false, &lo);
    REQUIRE(h.size() == 4096);
    CHECK(h[0] == 1.0);
    CHECK(std::accumulate(h.begin() + 1, h.end(), 0.0, [](double a, double v) { return a + std::abs(v); }) == 0.0);
    CHECK(lo.size() == 4096);
}

TEST_CASE("Filter design: DC gain, symmetry, pass band and stop band", "[design]") {
    for (double phase : {0.0, 0.5, 1.0})
        for (bool apod : {false, true}) {
            std::vector<double> lo;
            const auto h = design_lowpass(1024, 4, 44100, 176400, phase, apod, &lo);
            INFO("phase " << phase << " apodize " << apod);
            REQUIRE(h.size() == 4096);
            CHECK(std::abs(sum_comp(h, lo) - 1.0) < 1e-20);
            if (phase == 0.0)
                for (size_t i = 0; i < h.size() / 2; ++i) REQUIRE(h[i] == h[h.size() - 1 - i]);
            CHECK(std::abs(db(mag(h, 1000.0, 176400))) < 1e-4);
            CHECK(std::abs(db(mag(h, 20000.0, 176400))) < 1e-4);
            CHECK(db(mag(h, 22050.0, 176400)) < -140.0);   // the input Nyquist
            CHECK(db(mag(h, 30000.0, 176400)) < -140.0);
            CHECK(db(mag(h, 66150.0, 176400)) < -140.0);   // an image of 22.05 kHz
        }
}

TEST_CASE("Filter design: minimum phase moves the peak to the start", "[design]") {
    const auto lin = design_lowpass(2048, 4, 44100, 176400, 0.0, false);
    const auto mid = design_lowpass(2048, 4, 44100, 176400, 0.5, false);
    const auto min = design_lowpass(2048, 4, 44100, 176400, 1.0, false);
    auto peak_at = [](const std::vector<double>& h) {
        size_t k = 0;
        for (size_t i = 1; i < h.size(); ++i)
            if (std::abs(h[i]) > std::abs(h[k])) k = i;
        return k;
    };
    CHECK(peak_at(lin) == lin.size() / 2 - 1);   // even length: two equal centre taps
    CHECK(peak_at(min) < 200);
    CHECK(peak_at(mid) > peak_at(min));
    CHECK(peak_at(mid) < peak_at(lin));
}

TEST_CASE("Filter design: the result does not depend on the thread count", "[design]") {
    daphne::dsp::set_design_threads(1);
    std::vector<double> lo1, lo4;
    const auto h1 = design_lowpass(4096, 4, 44100, 176400, 0.5, true, &lo1);
    daphne::dsp::set_design_threads(4);
    const auto h4 = design_lowpass(4096, 4, 44100, 176400, 0.5, true, &lo4);
    daphne::dsp::set_design_threads(0);
    CHECK(h1 == h4);
    CHECK(lo1 == lo4);
}

TEST_CASE("Filter design: progress reaches 1", "[design]") {
    std::atomic<double> p{0.0};
    {
        daphne::dsp::ProgressScope scope(&p);
        design_lowpass(2048, 4, 44100, 176400, 0.5, false);
    }
    CHECK(p.load() == 1.0);
}
