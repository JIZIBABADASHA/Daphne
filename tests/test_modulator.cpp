#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "daphne/dsp/limiter.h"
#include "daphne/dsp/modulator.h"

using daphne::dsp::DsdModulator;

namespace {

const double kPi = 3.14159265358979323846;

// amplitude of the f component of one channel of the DSD bytes (stereo interleaved), skipping the start
double tone_level(const std::vector<uint8_t>& out, int channels, int c, double f, int dsd_rate, int64_t skip_bits) {
    double re = 0, im = 0;
    int64_t count = 0;
    const double w = 2 * kPi * f / dsd_rate;
    for (size_t k = c; k < out.size(); k += channels)
        for (int bit = 0; bit < 8; ++bit) {
            const int64_t t = static_cast<int64_t>(k / channels) * 8 + bit;
            if (t < skip_bits) continue;
            const double s = (out[k] >> (7 - bit) & 1) ? 1.0 : -1.0;
            re += s * std::cos(w * t);
            im += s * std::sin(w * t);
            ++count;
        }
    return 2 * std::hypot(re, im) / count;
}

}  // namespace

TEST_CASE("Modulator: stays within the scaled range and encodes the signal at the reference gain", "[modulator]") {
    const int fs = 44100, n = 8192;
    for (int order : {5, 8, 9}) {
        std::vector<double> x(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; ++i) x[2 * i] = x[2 * i + 1] = 0.9 * std::sin(2 * kPi * 1000 * i / fs);   // -0.9 dBFS
        DsdModulator m(fs, 64, order);
        CHECK(m.order() == order);
        CHECK(m.dsd_rate() == 2822400);
        std::vector<uint8_t> out;
        m.process(x.data(), n, 2, out);
        REQUIRE(out.size() == static_cast<size_t>(n) * 8 * 2);
        CHECK(m.clamped_bits() == 0);
        CHECK(m.loop_resets() == 0);
        CHECK(m.input_clips() == 0);
        CHECK(tone_level(out, 2, 0, 1000, m.dsd_rate(), 64 * 1024) == Catch::Approx(0.9 * DsdModulator::kGain).margin(0.01));
    }
    // far beyond full scale: the input is clipped, the loop never needs a reset
    std::vector<double> loud(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) loud[2 * i] = loud[2 * i + 1] = 4.0 * std::sin(2 * kPi * 3000 * i / fs);
    DsdModulator m(fs, 64, 8);
    std::vector<uint8_t> out;
    m.process(loud.data(), n, 2, out);
    CHECK(m.input_clips() > 0);
    CHECK(m.loop_resets() == 0);
}

TEST_CASE("Modulator: worker thread and uneven chunks give the same bytes", "[modulator]") {
    const int fs = 705600, n = 1 << 16;
    std::vector<double> x(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        x[2 * i] = 0.7 * std::sin(2 * kPi * 997 * i / fs);
        x[2 * i + 1] = 0.5 * std::sin(2 * kPi * 3000 * i / fs + 1.0);
    }
    DsdModulator a(fs, 128, 8), b(fs, 128, 8);
    b.set_threaded(false);
    std::vector<uint8_t> oa, ob, oc;
    for (int k = 0; k < n; k += 4099) {   // uneven chunks: partial bytes carry over between calls
        const int m = std::min(4099, n - k);
        a.process(x.data() + 2 * static_cast<size_t>(k), m, 2, oa);
        b.process(x.data() + 2 * static_cast<size_t>(k), m, 2, ob);
    }
    DsdModulator c(fs, 128, 8);
    c.process(x.data(), n, 2, oc);
    CHECK(oa == ob);
    CHECK(oa == oc);
    CHECK(oa.size() == static_cast<size_t>(n) * 2);   // 5.6448 MHz / 705.6 kHz = 8 bits = one byte per frame and channel
}

TEST_CASE("Modulator: a rate that is not a power-of-two ratio (interpolated phase)", "[modulator]") {
    const int fs = 768000, n = 768000 / 4;   // 48 kHz family on the 44.1 kHz-family clock
    std::vector<double> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[i] = 0.5 * std::sin(2 * kPi * 1000 * i / fs);
    DsdModulator m(fs, 128, 8);
    std::vector<uint8_t> out;
    m.process(x.data(), n, 1, out);
    // 0.25 s at 5.6448 MHz: 176400 bytes (the phase accumulates: within one byte)
    CHECK(std::abs(static_cast<long long>(out.size()) - 176400) <= 1);
    CHECK(m.clamped_bits() == 0);
    CHECK(tone_level(out, 1, 0, 1000, m.dsd_rate(), 64 * 1024) == Catch::Approx(0.5 * DsdModulator::kGain).margin(0.01));
}

TEST_CASE("Limiter: no sample above the ceiling, bit-exact delay below it", "[limiter]") {
    daphne::dsp::Limiter lim(0.9, 2.0, 80.0);
    lim.prepare({48000, 2, 4800});
    const int w = lim.latency_frames();
    CHECK(w == 96);
    const int n = 48000 * 3;
    std::vector<double> x(2 * n), y(2 * n);
    for (int i = 0; i < n; ++i) x[2 * i] = x[2 * i + 1] = (i > 10000 && i < 12000 ? 1.4 : 0.5) * std::sin(2 * kPi * 440 * i / 48000);
    for (int off = 0; off < n; off += 4800) lim.process(x.data() + 2 * off, 4800, y.data() + 2 * off);
    double peak = 0;
    for (double v : y) peak = std::max(peak, std::abs(v));
    CHECK(peak <= 0.9 + 1e-12);
    CHECK(lim.limited_frames() > 0);
    // before the loud part: the input delayed by w frames, bit for bit
    for (int i = w; i < 9000; ++i) REQUIRE(y[2 * i] == x[2 * (i - w)]);
    // 0.4 s after it the one-pole release (80 ms) is within 0.05 dB of unity ...
    for (int i = 12000 + 19200; i < 12000 + 20000; ++i) REQUIRE(std::abs(y[2 * i] - x[2 * (i - w)]) <= 0.006 * std::abs(x[2 * (i - w)]) + 1e-15);
    // ... and once it is within 1e-9 (about 21 time constants, 1.7 s) it snaps to 1: bit for bit again
    for (int i = 12000 + 48000 * 2; i < n; ++i) REQUIRE(y[2 * i] == x[2 * (i - w)]);
}
