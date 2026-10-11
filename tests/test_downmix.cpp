#include <catch_amalgamated.hpp>

#include <cmath>
#include <random>
#include <vector>

#include "daphne/dsp/downmix.h"

using daphne::dsp::Downmix;

namespace {

constexpr double k = 0.7071067811865476;
constexpr double k71 = 0.408248290463863;

std::vector<double> mix(uint32_t mask, int ch, std::vector<double> frame) {
    Downmix d(mask);
    d.prepare({48000, ch, 1});
    std::vector<double> y(2);
    d.process(frame.data(), 1, y.data());
    return y;
}

}  // namespace

TEST_CASE("Downmix: standard layouts follow the BS.775 formulas", "[downmix]") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<double> x(8);
        for (auto& v : x) v = u(rng);
        const double t = k * x[2] + 0.5 * x[3];
        // 3.0 (FL FR FC)
        auto y = mix(0x7, 3, {x[0], x[1], x[2]});
        CHECK(y[0] == Catch::Approx(k * (x[0] + k * x[2])).margin(1e-15));
        CHECK(y[1] == Catch::Approx(k * (x[1] + k * x[2])).margin(1e-15));
        // 3.1 (FL FR FC LFE)
        y = mix(0xF, 4, {x[0], x[1], x[2], x[3]});
        CHECK(y[0] == Catch::Approx(k * (x[0] + t)).margin(1e-15));
        CHECK(y[1] == Catch::Approx(k * (x[1] + t)).margin(1e-15));
        // quad (FL FR BL BR)
        y = mix(0x33, 4, {x[0], x[1], x[4], x[5]});
        CHECK(y[0] == Catch::Approx(k * (x[0] + k * x[4])).margin(1e-15));
        CHECK(y[1] == Catch::Approx(k * (x[1] + k * x[5])).margin(1e-15));
        // 5.1 (FL FR FC LFE BL BR)
        y = mix(0x3F, 6, {x[0], x[1], x[2], x[3], x[4], x[5]});
        CHECK(y[0] == Catch::Approx(0.5 * (x[0] + t + k * x[4])).margin(1e-15));
        CHECK(y[1] == Catch::Approx(0.5 * (x[1] + t + k * x[5])).margin(1e-15));
        // 5.1 with side channels (FL FR FC LFE SL SR)
        y = mix(0x60F, 6, {x[0], x[1], x[2], x[3], x[4], x[5]});
        CHECK(y[0] == Catch::Approx(0.5 * (x[0] + t + k * x[4])).margin(1e-15));
        // 7.1 (FL FR FC LFE BL BR SL SR)
        y = mix(0x63F, 8, x);
        CHECK(y[0] == Catch::Approx(k71 * (x[0] + t + k * (x[4] + x[6]))).margin(1e-15));
        CHECK(y[1] == Catch::Approx(k71 * (x[1] + t + k * (x[5] + x[7]))).margin(1e-15));
        // 6.1 (FL FR FC LFE BC SL SR): the back centre forms the back pair at x 0.7071
        y = mix(0x70F, 7, {x[0], x[1], x[2], x[3], x[4], x[5], x[6]});
        CHECK(y[0] == Catch::Approx(k71 * (x[0] + t + k * (k * x[4] + x[5]))).margin(1e-15));
        CHECK(y[1] == Catch::Approx(k71 * (x[1] + t + k * (k * x[4] + x[6]))).margin(1e-15));
    }
}

TEST_CASE("Downmix: missing masks use the default layout; mono and stereo", "[downmix]") {
    // 3 channels unmasked = 3.0: the centre is kept
    CHECK(mix(0, 3, {0, 0, 1})[0] == Catch::Approx(0.5));
    // 4 channels unmasked = quad: rear left goes to the left only
    auto y = mix(0, 4, {0, 0, 1, 0});
    CHECK(y[0] == Catch::Approx(0.5));
    CHECK(y[1] == 0.0);
    // a mask whose bit count does not match the channel count is replaced too
    y = mix(0x3, 6, {0, 0, 1, 0, 0, 0});
    CHECK(y[0] == Catch::Approx(0.5 * k));
    // mono: both sides
    y = mix(0, 1, {0.25});
    CHECK(y[0] == 0.25);
    CHECK(y[1] == 0.25);
    // stereo: identity
    Downmix st(0);
    st.prepare({48000, 2, 4});
    CHECK(st.is_identity());
    // only height channels (no known role): the first two channels
    y = mix(0x1000 | 0x4000, 2 + 0, {0.1, 0.2});
    CHECK(y[0] == 0.1);
    y = mix(0x1000 | 0x4000 | 0x10000, 3, {0.1, 0.2, 0.3});
    CHECK(y[0] == 0.1);
    CHECK(y[1] == 0.2);
}

TEST_CASE("Downmix: in place over a block, more than eight channels", "[downmix]") {
    Downmix d(0x3F);
    d.prepare({48000, 6, 64});
    std::vector<double> buf(64 * 6), ref(64 * 2);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = std::sin(0.37 * double(i));
    std::vector<double> copy = buf;
    d.process(copy.data(), 64, ref.data());
    d.process(buf.data(), 64, buf.data());
    for (size_t i = 0; i < ref.size(); ++i) CHECK(buf[i] == ref[i]);

    Downmix wide(0);   // 12 channels without a layout: first two, no overrun
    wide.prepare({48000, 12, 2});
    std::vector<double> x(24, 0.5), y(4);
    x[0] = 0.1;
    x[1] = 0.2;
    wide.process(x.data(), 2, y.data());
    CHECK(y[0] == 0.1);
    CHECK(y[1] == 0.2);
}
