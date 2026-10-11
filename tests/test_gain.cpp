#include <catch_amalgamated.hpp>

#include <cmath>
#include <thread>
#include <vector>

#include "daphne/dsp/gain.h"

using daphne::dsp::Gain;

TEST_CASE("Gain: 0 dB is a bit-exact identity", "[gain]") {
    Gain g;
    g.prepare({48000, 2, 256});
    std::vector<double> in(512), out(512);
    for (size_t i = 0; i < in.size(); ++i) in[i] = std::sin(0.01 * static_cast<double>(i)) * 0.9;
    REQUIRE(g.is_identity());
    REQUIRE(g.process(in.data(), 256, out.data()) == 256);
    CHECK(in == out);
}

TEST_CASE("Gain: a change ramps without a step, then holds", "[gain]") {
    Gain g(0.0, 10.0);   // 480 frames at 48 kHz
    g.prepare({48000, 1, 4800});
    g.set_db(-20.0);
    std::vector<double> in(4800, 1.0), out(4800);
    g.process(in.data(), 4800, out.data());
    const double target = 0.1;
    CHECK(out[0] < 1.0);
    CHECK(out[0] > 0.99);                         // starts near the old gain
    CHECK(out[479] == Catch::Approx(target).margin(1e-9));
    for (int i = 480; i < 4800; ++i) REQUIRE(out[i] == in[i] * Gain::db_to_linear(-20.0));
    for (int i = 1; i < 4800; ++i) REQUIRE(out[i] <= out[i - 1] + 1e-15);   // monotonic, no overshoot
    CHECK_FALSE(g.is_identity());
}

TEST_CASE("Gain: the ramp does not depend on the block size and ends exactly on the target", "[gain]") {
    Gain a(0.0, 20.0), b(0.0, 20.0);
    a.prepare({44100, 2, 4410});
    b.prepare({44100, 2, 4410});
    std::vector<double> in(2 * 4410), oa(2 * 4410), ob(2 * 4410);
    for (size_t i = 0; i < in.size(); ++i) in[i] = std::cos(0.003 * double(i));
    a.set_db(-6.0);
    b.set_db(-6.0);
    a.process(in.data(), 4410, oa.data());
    for (int off = 0; off < 4410; off += 100) b.process(in.data() + 2 * off, std::min(100, 4410 - off), ob.data() + 2 * off);
    CHECK(oa == ob);
    // and back to 0 dB in small blocks: bit-exact identity again after 20 ms (882 frames)
    b.set_db(0.0);
    for (int off = 0; off < 4410; off += 64) b.process(in.data() + 2 * off, std::min(64, 4410 - off), ob.data() + 2 * off);
    CHECK(b.is_identity());
    b.process(in.data(), 4410, ob.data());
    CHECK(ob == in);
}

TEST_CASE("Gain: reset starts at the target; positive gain for raised tracks", "[gain]") {
    Gain g(-6.0);
    g.prepare({44100, 2, 64});
    std::vector<double> in(128, 0.5), out(128);
    g.process(in.data(), 64, out.data());
    CHECK(out[0] == 0.5 * Gain::db_to_linear(-6.0));
    Gain up(6.0);
    up.prepare({44100, 2, 64});
    up.process(in.data(), 64, out.data());
    CHECK(out[0] == Catch::Approx(0.5 * 1.9952623149688795));
}

TEST_CASE("Gain: set_db from another thread while processing", "[gain]") {
    Gain g;
    g.prepare({48000, 2, 512});
    std::vector<double> in(1024, 0.5), out(1024);
    std::atomic<bool> stop{false};
    std::thread t([&] {
        for (int i = 0; !stop; ++i) g.set_db(-(i % 40));
    });
    for (int k = 0; k < 2000; ++k) {
        g.process(in.data(), 512, out.data());
        for (double v : out) REQUIRE((v <= 0.5 && v >= 0.5 * 0.0099));
    }
    stop = true;
    t.join();
}
