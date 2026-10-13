#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
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

TEST_CASE("Live decoder: lapped by the writer it restarts close to it; a starve fades out, then 5 ms of silence", "[live]") {
    Bus bus(48000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(4096 * 2);
    bus.write(20000);
    d->read(out.data(), 1024);   // syncs to the writer (silence)
    bus.write(20000);
    REQUIRE(d->read(out.data(), 1024) > 0);   // more than 0.3 s ahead: reading
    bus.write(47000);                          // laps the reader
    d->read(out.data(), 1024);
    CHECK(live_stats().resyncs == 1);

    // refill, read until only what is held back for a fade-out is left, then nothing more is written
    bus.write(20000);
    std::vector<double> last;
    int guard = 0;
    for (int n; (n = d->read(out.data(), 1024)) > 0 && ++guard < 200;) {
        if (n == 240 && out[0] == 0.0 && out[2 * 239] == 0.0) break;   // the starve's silence
        last.assign(out.begin(), out.begin() + 2 * n);
    }
    REQUIRE(guard < 200);
    // the call before the silence faded out: its last frames are near zero, its first ones are not
    REQUIRE(last.size() >= 2 * 200);
    CHECK(std::abs(last[last.size() - 2]) < 0.01);
    CHECK(live_stats().starves == 1);
    // starved: wait about 20 ms, then 240 frames (5 ms) of silence
    const auto t0 = std::chrono::steady_clock::now();
    const int n = d->read(out.data(), 4096);
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(n == 240);
    for (int i = 0; i < 2 * n; ++i) CHECK(out[static_cast<size_t>(i)] == 0.0);
    CHECK(waited >= std::chrono::milliseconds(15));
    CHECK(waited < std::chrono::milliseconds(200));
    CHECK(live_stats().starves == 1);   // one event, not one per silent call
}

namespace {

// Feeds a sine of frequency f with the producer `fast` times the reader's rate; returns the reader's output (left),
// one segment per read call (within a call the read rate is constant).
std::vector<std::vector<double>> run_sine(double f, double fast, int rounds) {
    const int cap = 1 << 17;
    std::vector<uint8_t> mem(16 + static_cast<size_t>(cap) * 8, 0);
    set_live_bus(mem.data(), cap, 48000);
    uint32_t& w = *reinterpret_cast<uint32_t*>(mem.data());
    float* ring = reinterpret_cast<float*>(mem.data() + 16);
    int64_t t = 0;
    double owed = 0;
    auto produce = [&](int n) {
        for (int i = 0; i < n; ++i, ++t) {
            const float v = static_cast<float>(0.5 * std::sin(2 * kPi * f * static_cast<double>(t) / 48000.0));
            const size_t k = (w + static_cast<uint32_t>(i)) % static_cast<uint32_t>(cap);
            ring[2 * k] = v, ring[2 * k + 1] = v;
        }
        w += static_cast<uint32_t>(n);
    };
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(2 * 512);
    std::vector<std::vector<double>> y;
    produce(24000);
    for (int r = 0; r < rounds; ++r) {
        owed += 512 * fast;
        const int n = static_cast<int>(owed);
        owed -= n;
        produce(n);
        const int got = d->read(out.data(), 512);
        std::vector<double> seg(static_cast<size_t>(got));
        for (int i = 0; i < got; ++i) seg[static_cast<size_t>(i)] = out[2 * i];
        y.push_back(std::move(seg));
    }
    d.reset();
    set_live_bus(nullptr, 0, 0);
    return y;
}

// Residual after removing the best-fitting sine near f from each of the last `count` full segments, in dB relative
// to the tone. One segment is one read call: its read rate is constant, so the fit sees the interpolation alone
// (across calls the rate correction moves by a few ppm, which a single sine over several calls would count).
double spurious_db(const std::vector<std::vector<double>>& segs, double f, int count) {
    double res = 0, sig = 0;
    int used = 0;
    for (size_t k = segs.size(); k-- > 0 && used < count;) {
        const std::vector<double>& y = segs[k];
        const int n = static_cast<int>(y.size());
        if (n < 512) continue;
        auto fit = [&](double ff) {
            double a = 0, b = 0, aa = 0, bb = 0, ab = 0;
            for (int i = 0; i < n; ++i) {
                const double sn = std::sin(2 * kPi * ff * i / 48000.0), cs = std::cos(2 * kPi * ff * i / 48000.0);
                a += y[i] * sn, b += y[i] * cs, aa += sn * sn, bb += cs * cs, ab += sn * cs;
            }
            const double det = aa * bb - ab * ab, A = (a * bb - b * ab) / det, B = (b * aa - a * ab) / det;
            double r2 = 0;
            for (int i = 0; i < n; ++i) {
                const double q = y[i] - A * std::sin(2 * kPi * ff * i / 48000.0) - B * std::cos(2 * kPi * ff * i / 48000.0);
                r2 += q * q;
            }
            return r2;
        };
        double best_f = f, best = 1e300;
        for (double ff = f * 0.994; ff <= f * 1.006; ff += f * 0.0002)   // coarse
            if (const double r = fit(ff); r < best) best = r, best_f = ff;
        // fine: golden-section search around the coarse minimum, down to a frequency error far below what would show
        double lo = best_f - f * 0.0002, hi = best_f + f * 0.0002;
        const double g = 0.6180339887498949;
        double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo), f1 = fit(x1), f2 = fit(x2);
        for (int it = 0; it < 80; ++it) {
            if (f1 < f2) hi = x2, x2 = x1, f2 = f1, x1 = hi - g * (hi - lo), f1 = fit(x1);
            else lo = x1, x1 = x2, f1 = f2, x2 = lo + g * (hi - lo), f2 = fit(x2);
        }
        best = std::min({best, f1, f2});
        res += best, sig += n * 0.125, ++used;
    }
    return 10 * std::log10(res / sig);
}

}  // namespace

TEST_CASE("Live decoder: the clock correction leaves no spurious products up to 15 kHz", "[live]") {
    // the producer 0.1 % fast: the read rate is corrected continuously (4-point Hermite left -28 dB at 10 kHz and
    // -18 dB at 15 kHz here). The input is float32, so the floor is its rounding, about -150 dB.
    for (double f : {1000.0, 10000.0, 15000.0}) {
        const auto segs = run_sine(f, 1.001, 600);
        const double db = spurious_db(segs, f, 8);
        INFO(f << " Hz: " << db << " dB");
        CHECK(db < -90.0);
        CHECK(std::abs(live_stats().adjust) > 1e-5);   // it was correcting
    }
}

TEST_CASE("Live decoder: a stall fades out and back in instead of jumping", "[live]") {
    // the producer writes 1.5 s at once while the reader reads little: more than 1.2 s piles up
    const int cap = 1 << 17;
    std::vector<uint8_t> mem(16 + static_cast<size_t>(cap) * 8, 0);
    set_live_bus(mem.data(), cap, 48000);
    uint32_t& w = *reinterpret_cast<uint32_t*>(mem.data());
    float* ring = reinterpret_cast<float*>(mem.data() + 16);
    auto produce = [&](int n) {
        for (int i = 0; i < n; ++i) {
            const size_t k = (w + static_cast<uint32_t>(i)) % static_cast<uint32_t>(cap);
            ring[2 * k] = ring[2 * k + 1] = 0.5f;   // DC: any step is the decoder's
        }
        w += static_cast<uint32_t>(n);
    };
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(2 * 1024), y;
    d->read(out.data(), 1024);   // sync
    produce(20000);
    for (int r = 0; r < 20; ++r) {
        produce(1024);
        const int n = d->read(out.data(), 1024);
        y.insert(y.end(), out.begin(), out.begin() + 2 * n);
    }
    produce(72000);   // a stall's worth piles up
    for (int r = 0; r < 20; ++r) {
        produce(1024);
        const int n = d->read(out.data(), 1024);
        y.insert(y.end(), out.begin(), out.begin() + 2 * n);
    }
    CHECK(live_stats().resyncs == 1);
    // the level goes 0.5 -> 0 -> 0.5 in 5 ms ramps: no step bigger than one ramp step (0.5 / 240) plus slack
    double worst = 0;
    size_t s = 0;
    while (s < y.size() && y[s] == 0.0) s += 2;
    bool dipped = false;
    for (size_t i = s + 2; i < y.size(); i += 2) {
        worst = std::max(worst, std::abs(y[i] - y[i - 2]));
        if (y[i] < 0.01) dipped = true;
    }
    CHECK(dipped);
    CHECK(worst < 0.5 / 240 * 1.5);
    d.reset();
    set_live_bus(nullptr, 0, 0);
}

TEST_CASE("Live decoder: the write index wrapping at 2^32 is not a restart", "[live]") {
    Bus bus(48000);
    bus.index() = 0xFFFFFFFFu - 30000;   // about 0.6 s before the wrap
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(2 * 1024);
    d->read(out.data(), 1024);   // sync
    bus.write(20000);
    for (int r = 0; r < 60; ++r) {   // reads across the wrap
        bus.write(1024);
        d->read(out.data(), 1024);
    }
    CHECK(bus.index() < 100000u);   // the index did wrap
    CHECK(live_stats().resyncs == 0);
    CHECK(live_stats().starves == 0);
}

TEST_CASE("Live decoder: detaching the ring waits for a read in progress", "[live]") {
    // the host detaches and frees its memory while a decoder is reading it (with 2 s written, so reads go through the
    // interpolation): set_live_bus(nullptr) must not return before the read has left. Under AddressSanitizer a read
    // still inside the freed memory is reported.
    for (int round = 0; round < 200; ++round) {
        const int cap = 96000;
        auto mem = std::make_unique<std::vector<uint8_t>>(16 + static_cast<size_t>(cap) * 8, 0);
        *reinterpret_cast<uint32_t*>(mem->data()) = 0;
        set_live_bus(mem->data(), cap, 48000);
        std::string e;
        auto d = open_decoder(kLivePath, &e);
        REQUIRE(d);
        std::vector<double> out(2 * 4096);
        d->read(out.data(), 16);                                    // sync at index 0
        *reinterpret_cast<uint32_t*>(mem->data()) = 90000;         // almost 2 s written
        std::atomic<bool> stop{false};
        std::thread reader([&] {
            while (!stop) d->read(out.data(), 4096);
        });
        std::this_thread::sleep_for(std::chrono::microseconds(200 + (round % 7) * 50));
        set_live_bus(nullptr, 0, 0);   // returns only when no read is inside the old memory
        mem.reset();                   // freed
        stop = true;
        reader.join();
    }
    SUCCEED();
}

TEST_CASE("Live decoder: without the host's ring it does not open", "[live]") {
    set_live_bus(nullptr, 0, 0);
    std::string e;
    CHECK_FALSE(open_decoder(kLivePath, &e));
    CHECK_FALSE(e.empty());
}
