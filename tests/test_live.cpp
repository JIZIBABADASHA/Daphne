#include <catch_amalgamated.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "daphne/engine/decoder.h"
#include "daphne/engine/live_source.h"

using namespace daphne;

namespace {

constexpr double kPi = 3.141592653589793;

// The host's shared ring: uint32 write index | 12 bytes | float32 stereo.
struct Bus {
    explicit Bus(int cap) : cap(cap), mem(16 + static_cast<size_t>(cap) * 8, 0) { set_live_bus(mem.data(), cap, 48000); }
    ~Bus() { set_live_bus(nullptr, 0, 0); }
    uint32_t& index() { return *reinterpret_cast<uint32_t*>(mem.data()); }
    float* ring() { return reinterpret_cast<float*>(mem.data() + 16); }
    // writes n frames of a 1 kHz sine (continuing its phase), then the index
    void write(int n) {
        for (int i = 0; i < n; ++i, ++t) {
            const float v = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * t / 48000.0));
            const size_t k = (index() + static_cast<uint32_t>(i)) % static_cast<uint32_t>(cap);
            ring()[2 * k] = v, ring()[2 * k + 1] = -v;
        }
        index() += static_cast<uint32_t>(n);
    }
    int cap;
    std::vector<uint8_t> mem;
    int64_t t = 0;
};

}  // namespace

TEST_CASE("Live decoder: silence before the producer starts, then the stream without breaks", "[live]") {
    Bus bus(96000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    CHECK(d->info().rate == 48000);
    CHECK(d->info().channels == 2);
    CHECK(d->info().frames == -1);

    std::vector<double> out(4096 * 2);
    // nothing written: silence at once (the player prefills)
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(d->read(out.data(), 4096) == 4096);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(10));
    for (double v : out) CHECK(v == 0.0);

    // the producer runs ahead (0.5 s); then reads and writes alternate at about the same rate
    bus.write(24000);
    std::vector<double> y;
    for (int round = 0; round < 60; ++round) {
        const int n = d->read(out.data(), 1024);
        REQUIRE(n > 0);
        y.insert(y.end(), out.begin(), out.begin() + 2 * n);
        bus.write(1024);
    }
    // skip the silence before the stream started (the first read after priming may begin mid-call)
    size_t s = 0;
    while (s < y.size() && y[s] == 0.0) s += 2;
    REQUIRE(y.size() - s > 2 * 40000);
    // a 1 kHz sine at 0.5: consecutive samples differ by at most 2 pi f / fs * 0.5 (+ the rate correction)
    const double step_max = 2 * kPi * 1000.0 / 48000.0 * 0.5 * 1.01;
    double worst = 0;
    for (size_t i = s + 2; i < y.size(); i += 2) {
        worst = std::max(worst, std::abs(y[i] - y[i - 2]));
        CHECK(y[i + 1] == Catch::Approx(-y[i]).margin(1e-12));   // channels kept apart
    }
    CHECK(worst < step_max);
    const auto st = live_stats();
    CHECK(st.active);
    CHECK(st.resyncs == 0);
    CHECK(st.starves == 0);
    CHECK(std::abs(st.adjust) <= 0.005);
}

TEST_CASE("Live decoder: lapped by the writer it restarts close to it; a starve returns 5 ms of silence", "[live]") {
    Bus bus(48000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(4096 * 2);
    bus.write(20000);
    d->read(out.data(), 1024);   // syncs to the writer (silence)
    bus.write(20000);
    REQUIRE(d->read(out.data(), 1024) > 0);   // primed (more than 0.3 s ahead), reading
    bus.write(47000);                          // laps the reader
    d->read(out.data(), 1024);
    CHECK(live_stats().resyncs == 1);

    // drain what is there, then nothing more is written: after a short wait, 5 ms (240 frames) of silence
    bus.write(20000);
    int guard = 0;
    while (d->read(out.data(), 4096) > 240 && ++guard < 100) {}
    const auto t0 = std::chrono::steady_clock::now();
    const int n = d->read(out.data(), 4096);
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(n == 240);
    for (int i = 0; i < 2 * n; ++i) CHECK(out[static_cast<size_t>(i)] == 0.0);
    CHECK(waited >= std::chrono::milliseconds(15));
    CHECK(waited < std::chrono::milliseconds(200));
    CHECK(live_stats().starves >= 1);
}

TEST_CASE("Live decoder: without the host's ring it does not open", "[live]") {
    set_live_bus(nullptr, 0, 0);
    std::string e;
    CHECK_FALSE(open_decoder(kLivePath, &e));
    CHECK_FALSE(e.empty());
}
