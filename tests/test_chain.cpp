#include <catch_amalgamated.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "daphne/engine/chain.h"

using namespace daphne;

namespace {

AudioInfo source(int rate, int channels, int bits) {
    AudioInfo a;
    a.rate = rate;
    a.channels = channels;
    a.bits = bits;
    a.lossless_int = bits > 0;
    a.codec = "test";
    return a;
}

DeviceFormat device(int rate, int channels, SampleType t, int bits) {
    DeviceFormat f;
    f.rate = rate;
    f.channels = channels;
    f.type = t;
    f.container_bits = t == SampleType::Float64 ? 64 : 32;
    f.valid_bits = bits;
    return f;
}

std::vector<std::string> stages(const Settings& s, const AudioInfo& a, const DeviceFormat& d) {
    DspChain c;
    std::string e;
    REQUIRE(c.build(s, a, d, 4096, &e));
    return c.stages();
}

}  // namespace

TEST_CASE("Chain: bit-perfect when nothing changes the samples and the source fits", "[chain]") {
    Settings s;
    DspChain c;
    std::string e;
    REQUIRE(c.build(s, source(44100, 2, 24), device(44100, 2, SampleType::Int24In32, 24), 4096, &e));
    CHECK(c.transparent());
    CHECK(c.stages().empty());
    std::vector<double> x(2 * 4096), y(2 * 4096);
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::round(std::sin(0.01 * double(i)) * 8388607.0) / 8388608.0;
    REQUIRE(c.process(x.data(), 4096, y.data()) == 4096);
    CHECK(x == y);
    // a 24-bit source into a 16-bit device: the quantizer comes in
    CHECK(stages(s, source(44100, 2, 24), device(44100, 2, SampleType::Int16, 16)) == std::vector<std::string>{"quantizer"});
    // a lossy (float) source: quantized
    CHECK(stages(s, source(44100, 2, 0), device(44100, 2, SampleType::Int32, 32)) == std::vector<std::string>{"quantizer"});
    // a float device takes it as it is
    CHECK(stages(s, source(44100, 2, 0), device(44100, 2, SampleType::Float32, 32)).empty());
}

TEST_CASE("Chain: stage order and which stages appear", "[chain]") {
    Settings s;
    s.volume.mode = VolumeMode::Digital;
    s.volume.db = -3.0;
    s.crossfeed.enabled = true;
    s.eq.enabled = true;
    s.eq.bands = {{EqType::PK, 1000.0, 1.0, 3.0, true}};
    CHECK(stages(s, source(44100, 6, 24), device(176400, 2, SampleType::Int32, 32)) ==
          std::vector<std::string>{"downmix", "volume", "crossfeed", "eq", "resampler", "quantizer"});
    // apodizing at the source rate: the resampler runs at 1x
    Settings a;
    a.filter.apodize = true;
    CHECK(stages(a, source(44100, 2, 16), device(44100, 2, SampleType::Int32, 32)) ==
          std::vector<std::string>{"resampler", "quantizer"});
    // dsd core: the pre-filter and the limiter, no quantizer (the modulator quantizes)
    Settings d;
    d.core = CoreMode::Dsd;
    CHECK(stages(d, source(44100, 2, 16), device(705600, 2, SampleType::Float64, 64)) ==
          std::vector<std::string>{"modulator pre-filter", "modulator limiter"});
    // a DSD file converted to PCM: limited, then quantized
    AudioInfo dsd = source(176400, 2, 0);
    dsd.dsd_pcm = true;
    CHECK(stages(Settings{}, dsd, device(176400, 2, SampleType::Int32, 32)) == std::vector<std::string>{"dsd limiter", "quantizer"});
}

TEST_CASE("Chain: level changes in place, structural changes need a new chain", "[chain]") {
    Settings s;
    s.volume.mode = VolumeMode::Digital;
    s.volume.db = -6.0;
    DspChain c;
    std::string e;
    REQUIRE(c.build(s, source(44100, 2, 24), device(88200, 2, SampleType::Int32, 32), 4096, &e));
    Settings v = s;
    v.volume.db = -12.0;
    v.crossfeed.enabled = true;
    v.headroom_db = 3.0;
    CHECK(c.update(v));
    Settings f = s;
    f.filter.taps = 4096;
    CHECK_FALSE(c.update(f));
    Settings r = s;
    r.output.rate = "auto";
    CHECK_FALSE(c.update(r));
    // bit-perfect -> volume needs the quantizer: a new chain
    DspChain b;
    REQUIRE(b.build(Settings{}, source(44100, 2, 16), device(44100, 2, SampleType::Int16, 16), 4096, &e));
    CHECK_FALSE(b.update(s));
}

TEST_CASE("Chain: content delay and output size come from the blocks' own reports", "[chain]") {
    // an impulse comes out at content_delay(): the resampler's kernel peak, plus the limiter's delay
    auto peak_at = [](const Settings& s, const AudioInfo& a, const DeviceFormat& d, int* delay, int* max_out) {
        DspChain c;
        std::string e;
        REQUIRE(c.build(s, a, d, 4096, &e));
        std::vector<double> x(2 * 4096, 0.0), y;
        x[2 * 100] = x[2 * 100 + 1] = 0.5;
        std::vector<double> o(2 * static_cast<size_t>(c.max_output_frames()));
        for (int k = 0; k < 4; ++k) {
            const int n = c.process(x.data(), 4096, o.data());
            REQUIRE(n <= c.max_output_frames());
            y.insert(y.end(), o.begin(), o.begin() + 2 * n);
            std::fill(x.begin(), x.end(), 0.0);
        }
        size_t best = 0;
        for (size_t f = 0; f < y.size() / 2; ++f)
            if (std::abs(y[2 * f]) > std::abs(y[2 * best])) best = f;
        *delay = c.content_delay();
        *max_out = c.max_output_frames();
        return static_cast<int>(best);
    };
    Settings s;
    s.filter.taps = 1024;
    int delay = 0, max_out = 0;
    // 2x: impulse at input frame 100 -> output frame 200 + kernel peak
    int at = peak_at(s, source(44100, 2, 16), device(88200, 2, SampleType::Float32, 32), &delay, &max_out);
    CHECK(at == 200 + delay);
    CHECK(delay > 0);
    CHECK(max_out >= 2 * 4096);   // the resampler's bound, not the input chunk
    // DSD file converted to PCM: the limiter's 2 ms add to the delay
    AudioInfo dsd = source(44100, 2, 0);
    dsd.dsd_pcm = true;
    const int resampler_only = delay;
    at = peak_at(s, dsd, device(88200, 2, SampleType::Float32, 32), &delay, &max_out);
    CHECK(at == 200 + delay);
    CHECK(delay == resampler_only + 176);   // 2 ms at 88.2 kHz
    // no resampler (native rate): delay 0, output size = input chunk
    at = peak_at(Settings{}, source(44100, 2, 16), device(44100, 2, SampleType::Float32, 32), &delay, &max_out);
    CHECK(at == 100);
    CHECK(delay == 0);
    CHECK(max_out == 4096);
}

TEST_CASE("Chain: wrong device rates are refused", "[chain]") {
    DspChain c;
    std::string e;
    CHECK_FALSE(c.build(Settings{}, source(44100, 2, 16), device(48000, 2, SampleType::Int32, 32), 4096, &e));
    CHECK_FALSE(e.empty());
    Settings d;
    d.core = CoreMode::Dsd;
    CHECK_FALSE(c.build(d, source(44100, 2, 16), device(352800, 2, SampleType::Float64, 64), 4096, &e));
    CHECK_FALSE(c.build(Settings{}, source(44100, 6, 16), device(44100, 4, SampleType::Int32, 32), 4096, &e));
}
