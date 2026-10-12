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

TEST_CASE("Chain: fits() tells when the next track at the same rate needs its own chain", "[chain]") {
    Settings s;
    std::string e;
    // 16-bit source on a 16-bit device: bit-perfect, no quantizer
    DspChain c;
    REQUIRE(c.build(s, source(44100, 2, 16), device(44100, 2, SampleType::Int16, 16), 4096, &e));
    REQUIRE(c.stages().empty());
    CHECK(c.fits(s, source(44100, 2, 16)));
    CHECK_FALSE(c.fits(s, source(44100, 2, 24)));   // needs the quantizer
    CHECK_FALSE(c.fits(s, source(44100, 2, 0)));    // lossy: needs the quantizer
    CHECK_FALSE(c.fits(s, source(48000, 2, 16)));   // another rate
    AudioInfo dsd = source(44100, 2, 0);
    dsd.dsd_pcm = true;
    CHECK_FALSE(c.fits(s, dsd));                    // the DSD limiter
    // what fits() says matches a chain built for the next source
    for (const AudioInfo& next : {source(44100, 2, 16), source(44100, 2, 24), source(44100, 2, 0)}) {
        DspChain n;
        REQUIRE(n.build(s, next, device(44100, 2, SampleType::Int16, 16), 4096, &e));
        CHECK(c.fits(s, next) == (n.stages() == c.stages()));
    }
    // the same, copied out for another thread
    CHECK(DspChain::fits(c.shape(), s, source(44100, 2, 16)));
    CHECK_FALSE(DspChain::fits(c.shape(), s, source(44100, 2, 24)));
    // 24-bit device: a 16- and a 24-bit source both fit it untouched
    DspChain w;
    REQUIRE(w.build(s, source(44100, 2, 16), device(44100, 2, SampleType::Int24In32, 24), 4096, &e));
    CHECK(w.fits(s, source(44100, 2, 24)));
    CHECK_FALSE(w.fits(s, source(44100, 2, 32)));
    // already quantizing (volume): any word length fits
    Settings v = s;
    v.volume.mode = VolumeMode::Digital;
    v.volume.db = -6.0;
    DspChain q;
    REQUIRE(q.build(v, source(44100, 2, 16), device(44100, 2, SampleType::Int16, 16), 4096, &e));
    CHECK(q.fits(v, source(44100, 2, 24)));
    CHECK(q.fits(v, source(44100, 2, 0)));
    // a float device never quantizes
    DspChain f;
    REQUIRE(f.build(s, source(44100, 2, 16), device(44100, 2, SampleType::Float32, 32), 4096, &e));
    CHECK(f.fits(s, source(44100, 2, 0)));
    // downmixed: the channel layout is part of the chain
    AudioInfo a51 = source(44100, 6, 16), a51side = a51;
    a51.channel_mask = 0x3F;        // 5.1 back
    a51side.channel_mask = 0x60F;   // 5.1 side
    DspChain m;
    REQUIRE(m.build(s, a51, device(44100, 2, SampleType::Float32, 32), 4096, &e));
    CHECK(m.fits(s, a51));
    CHECK_FALSE(m.fits(s, a51side));
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
