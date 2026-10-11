#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "daphne/dsp/modulator.h"
#include "daphne/engine/dsd_output.h"

using daphne::DeviceFormat;
using daphne::DsdEncoder;
using daphne::DsdTransport;
using daphne::SampleType;

namespace {

const double kPi = 3.14159265358979323846;

DeviceFormat device(SampleType t, int rate, int channels, int container, int valid) {
    DeviceFormat f;
    f.rate = rate;
    f.channels = channels;
    f.type = t;
    f.container_bits = container;
    f.valid_bits = valid;
    return f;
}

}  // namespace

TEST_CASE("DoP: markers alternate, payload is the modulator's bytes oldest first", "[dsd_output]") {
    const auto f = device(SampleType::Int32, 176400, 2, 32, 32);
    DsdEncoder e;
    REQUIRE(e.init(44100, 64, 5, f, DsdTransport::Dop, nullptr));
    CHECK(e.dsd_rate() == 2822400);
    CHECK(daphne::dop_rate(e.dsd_rate()) == 176400);
    std::vector<double> x(4096 * 2, 0.0);
    for (int i = 0; i < 4096; ++i) x[2 * i] = 0.3 * std::sin(2 * kPi * 440 * i / 44100.0);
    std::vector<uint8_t> out;
    e.encode(x.data(), 4096, out);
    REQUIRE(out.size() == 4096u * 4 * 8);   // 64 bits per frame and channel = 4 DoP frames
    for (size_t fr = 0; fr < out.size() / 8; ++fr) {   // 0x05 / 0xFA per frame, both channels; low byte unused
        const uint8_t mk = fr % 2 ? 0xFA : 0x05;
        REQUIRE(out[fr * 8 + 3] == mk);
        REQUIRE(out[fr * 8 + 7] == mk);
        REQUIRE(out[fr * 8 + 0] == 0);
    }
    daphne::dsp::DsdModulator ref(44100, 64, 5);
    std::vector<uint8_t> bits;
    ref.process(x.data(), 4096, 2, bits);
    bool same = true;
    for (size_t fr = 0; fr < out.size() / 8; ++fr)
        for (int c = 0; c < 2; ++c)
            same &= out[fr * 8 + 4 * c + 2] == bits[fr * 4 + c] && out[fr * 8 + 4 * c + 1] == bits[fr * 4 + 2 + c];
    CHECK(same);
    // idle keeps the marker sequence going
    std::vector<uint8_t> sil;
    e.idle(3, sil);
    REQUIRE(sil.size() == 3u * 8);
    CHECK(sil[3] == 0x05);   // 4096 * 4 frames before: an even count, 0x05 next
    CHECK(sil[2] == 0x69);
    CHECK(sil[1] == 0x69);
    CHECK(sil[11] == 0xFA);
    // 16-bit devices cannot carry DoP
    DsdEncoder e16;
    CHECK_FALSE(e16.init(44100, 64, 5, device(SampleType::Int16, 176400, 2, 16, 16), DsdTransport::Dop, nullptr));
}

TEST_CASE("DoP container layouts: packed 24, 24-in-32, right-aligned 24", "[dsd_output]") {
    const uint8_t dsd[4] = {0xA1, 0xB2, 0xC3, 0xD4};   // frame 0: L A1, R B2; frame 1: L C3, R D4
    {
        DsdEncoder e;
        REQUIRE(e.init_direct(2822400, device(SampleType::Packed24, 176400, 2, 24, 24), DsdTransport::Dop, nullptr));
        std::vector<uint8_t> out;
        e.encode_dsd(dsd, 2, out);
        REQUIRE(out.size() == 6);
        CHECK((out[0] == 0xC3 && out[1] == 0xA1 && out[2] == 0x05));   // L: newer byte low, older byte middle
        CHECK((out[3] == 0xD4 && out[4] == 0xB2 && out[5] == 0x05));
    }
    {
        DsdEncoder e;
        REQUIRE(e.init_direct(2822400, device(SampleType::Int24In32, 176400, 2, 32, 24), DsdTransport::Dop, nullptr));
        std::vector<uint8_t> out;
        e.encode_dsd(dsd, 2, out);
        REQUIRE(out.size() == 8);
        CHECK((out[0] == 0 && out[1] == 0xC3 && out[2] == 0xA1 && out[3] == 0x05));
    }
    {
        DsdEncoder e;
        REQUIRE(e.init_direct(2822400, device(SampleType::Int32Low, 176400, 2, 32, 24), DsdTransport::Dop, nullptr));
        std::vector<uint8_t> out;
        e.encode_dsd(dsd, 2, out);
        REQUIRE(out.size() == 8);
        CHECK((out[0] == 0xC3 && out[1] == 0xA1 && out[2] == 0x05 && out[3] == 0));
    }
    DsdEncoder e20;
    CHECK_FALSE(e20.init_direct(2822400, device(SampleType::Int32Low, 176400, 2, 32, 20), DsdTransport::Dop, nullptr));
}

TEST_CASE("Native DSD: bytes as they are, bit-reversed for LSB-first devices", "[dsd_output]") {
    const uint8_t dsd[6] = {0x01, 0x80, 0x0F, 0x69, 0x69, 0xF0};
    DsdEncoder m;
    REQUIRE(m.init_direct(5644800, device(SampleType::Dsd8Msb, 5644800, 2, 8, 8), DsdTransport::Native, nullptr));
    std::vector<uint8_t> out;
    m.encode_dsd(dsd, 3, out);
    CHECK(out == std::vector<uint8_t>(dsd, dsd + 6));
    DsdEncoder l;
    REQUIRE(l.init_direct(5644800, device(SampleType::Dsd8Lsb, 5644800, 2, 8, 8), DsdTransport::Native, nullptr));
    out.clear();
    l.encode_dsd(dsd, 3, out);
    CHECK(out == std::vector<uint8_t>{0x80, 0x01, 0xF0, 0x96, 0x96, 0x0F});
    // native needs a DSD format; direct playback takes up to 8 channels
    DsdEncoder bad;
    CHECK_FALSE(bad.init_direct(2822400, device(SampleType::Int32, 176400, 2, 32, 32), DsdTransport::Native, nullptr));
    DsdEncoder six;
    CHECK(six.init_direct(2822400, device(SampleType::Dsd8Msb, 2822400, 6, 8, 8), DsdTransport::Native, nullptr));
    CHECK(six.describe().find("DSD64") == 0);
}
