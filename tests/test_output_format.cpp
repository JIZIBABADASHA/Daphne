#include <catch_amalgamated.hpp>

#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <vector>

#include "daphne/dsp/quantizer.h"
#include "daphne/engine/sample_format.h"

using daphne::DeviceFormat;
using daphne::SampleType;

namespace {

const double kPi = 3.14159265358979323846;

DeviceFormat fmt(SampleType t, int container, int valid) {
    DeviceFormat f;
    f.rate = 44100;
    f.channels = 2;
    f.type = t;
    f.container_bits = container;
    f.valid_bits = valid;
    return f;
}

int32_t read_i32(const std::vector<uint8_t>& b, size_t i) {
    int32_t v;
    std::memcpy(&v, b.data() + i * 4, 4);
    return v;
}

}  // namespace

TEST_CASE("Device conversion: N-bit samples come out bit for bit", "[format]") {
    std::vector<double> x;
    std::vector<int32_t> s16, s24;
    for (int v : {-32768, -32767, -1, 0, 1, 12345, 32767}) {
        x.push_back(v / 32768.0);
        s16.push_back(v);
    }
    std::vector<uint8_t> out(x.size() * 4);
    CHECK(daphne::convert_to_device(x.data(), x.size(), fmt(SampleType::Int16, 16, 16), out.data()) == 0);
    for (size_t i = 0; i < x.size(); ++i) {
        int16_t v;
        std::memcpy(&v, out.data() + i * 2, 2);
        CHECK(v == s16[i]);
    }
    // the same 16-bit samples into a 24-in-32 and a 32-bit container: shifted, exact
    CHECK(daphne::convert_to_device(x.data(), x.size(), fmt(SampleType::Int24In32, 32, 24), out.data()) == 0);
    for (size_t i = 0; i < x.size(); ++i) CHECK(read_i32(out, i) == s16[i] * 65536);
    CHECK(daphne::convert_to_device(x.data(), x.size(), fmt(SampleType::Int32, 32, 32), out.data()) == 0);
    for (size_t i = 0; i < x.size(); ++i) CHECK(read_i32(out, i) == s16[i] * 65536);
    // packed 24 and int20 right-aligned (ASIO)
    std::vector<double> y = {-8388608 / 8388608.0, 8388607 / 8388608.0, 4660 / 8388608.0};
    std::vector<uint8_t> p(y.size() * 3);
    daphne::convert_to_device(y.data(), y.size(), fmt(SampleType::Packed24, 24, 24), p.data());
    CHECK((p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x80));
    CHECK((p[3] == 0xff && p[4] == 0xff && p[5] == 0x7f));
    CHECK((p[6] == 0x34 && p[7] == 0x12 && p[8] == 0x00));
    std::vector<double> z = {0.5, -1.0};
    std::vector<uint8_t> q(8);
    daphne::convert_to_device(z.data(), 2, fmt(SampleType::Int32Low, 32, 20), q.data());
    CHECK(read_i32(q, 0) == (1 << 18));
    CHECK(read_i32(q, 1) == -(1 << 19));
}

TEST_CASE("Device conversion: clamp, round half to even, NaN is silence", "[format]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> x = {1.5, -1.5, 0.5 / 32768.0, 1.5 / 32768.0, nan};
    std::vector<uint8_t> out(x.size() * 8);
    CHECK(daphne::convert_to_device(x.data(), x.size(), fmt(SampleType::Int16, 16, 16), out.data()) == 3);
    int16_t v[5];
    std::memcpy(v, out.data(), 10);
    CHECK(v[0] == 32767);
    CHECK(v[1] == -32768);
    CHECK(v[2] == 0);   // 0.5 LSB rounds to even
    CHECK(v[3] == 2);   // 1.5 LSB rounds to even
    CHECK(v[4] == 0);
    // floats: values above full scale pass (the device or its mixer decides), NaN does not
    CHECK(daphne::convert_to_device(x.data(), x.size(), fmt(SampleType::Float32, 32, 32), out.data()) == 1);
    float f[5];
    std::memcpy(f, out.data(), 20);
    CHECK(f[0] == 1.5f);
    CHECK(f[4] == 0.0f);
    CHECK(daphne::convert_to_device(x.data(), x.size(), fmt(SampleType::Float64, 64, 64), out.data()) == 1);
    double d[5];
    std::memcpy(d, out.data(), 40);
    CHECK(d[1] == -1.5);
    CHECK(d[4] == 0.0);
}

TEST_CASE("Quantizer: TPDF on the output grid, error statistics, no distortion at low level", "[quantizer]") {
    const int bits = 16, n = 1 << 17;
    const double lsb = 1.0 / 32768.0;
    daphne::dsp::Quantizer q(bits);
    q.prepare({44100, 1, n});
    std::vector<double> x(n), y(n);
    // a 1 kHz tone at -90 dBFS (about 1 LSB): plain rounding would give a square-ish wave with harmonics
    for (int i = 0; i < n; ++i) x[i] = std::pow(10.0, -90.0 / 20.0) * std::sin(2 * kPi * 1000.0 * i / 44100.0);
    q.process(x.data(), n, y.data());
    double mean = 0, var = 0;
    for (int i = 0; i < n; ++i) {
        REQUIRE(y[i] * 32768.0 == std::round(y[i] * 32768.0));   // on the 16-bit grid
        const double e = y[i] - x[i];
        mean += e;
        var += e * e;
    }
    mean /= n;
    var /= n;
    CHECK(std::abs(mean) < 0.01 * lsb);
    CHECK(var / (lsb * lsb) == Catch::Approx(0.25).epsilon(0.03));   // TPDF lsb^2/6 + rounding lsb^2/12
    // the 3rd harmonic stays at the noise floor (single-bin DFT, Hann window)
    auto bin = [&](double f) {
        std::complex<double> s = 0;
        for (int i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / n);
            s += w * y[i] * std::polar(1.0, -2 * kPi * f * i / 44100.0);
        }
        return std::abs(s);
    };
    const double fund = bin(1000.0), h3 = bin(3000.0), noise = bin(3270.0);
    CHECK(20 * std::log10(fund / h3) > 30.0);
    CHECK(h3 < 4.0 * noise + 1e-12);
    // clipping is counted
    daphne::dsp::Quantizer c(16);
    c.prepare({44100, 1, 4});
    std::vector<double> loud = {1.2, -1.2, 0.0, 0.5}, o(4);
    c.process(loud.data(), 4, o.data());
    CHECK(c.clip_count() == 2);
    CHECK(o[0] == 32767.0 / 32768.0);
    CHECK(o[1] == -1.0);
}
