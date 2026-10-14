#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "daphne/dsp/mailbox.h"
#include "daphne/engine/audio_output.h"
#include "daphne/engine/ring_buffer.h"
#include "daphne/engine/ring_feeder.h"

using namespace daphne;

TEST_CASE("ByteRing: clear() while the consumer reads never corrupts the indices", "[ring]") {
    // records of 16 bytes: (epoch, seq) twice; a torn read or a reset under a read shows up as a
    // record whose halves differ, or as readable() beyond the capacity
    ByteRing ring(1 << 12);
    std::atomic<bool> stop{false}, bad{false};
    std::atomic<int64_t> records{0};
    std::thread consumer([&] {
        uint8_t buf[256];
        while (!stop) {
            if (!ring.begin_read()) continue;
            if (ring.readable() > ring.capacity()) bad = true;
            const size_t n = ring.read(buf, std::min<size_t>(ring.readable() / 16, sizeof buf / 16) * 16);
            for (size_t i = 0; i < n; i += 16) {
                uint64_t a, b;
                std::memcpy(&a, buf + i, 8);
                std::memcpy(&b, buf + i + 8, 8);
                if (a != b) bad = true;
                ++records;
            }
            ring.end_read();
        }
    });
    uint64_t epoch = 0;
    for (int round = 0; round < 20000 && !bad; ++round) {
        for (uint64_t seq = 0; seq < 64; ++seq) {
            const uint64_t v = epoch << 32 | seq;
            uint8_t rec[16];
            std::memcpy(rec, &v, 8);
            std::memcpy(rec + 8, &v, 8);
            if (ring.writable() < 16) break;
            ring.write(rec, 16);
        }
        if (round % 3 == 0) {
            ring.clear();   // as a seek does, with the consumer running
            ++epoch;
            CHECK(ring.readable() == 0);
        }
    }
    stop = true;
    consumer.join();
    CHECK_FALSE(bad);
    CHECK(records > 0);
}

TEST_CASE("RingFeeder: the pause fade-out is dithered on integer formats", "[ring]") {
    // 16-bit stereo at 48 kHz, every sample 1000 LSB; pausing fades out over 20 ms (960 frames). Plain
    // rounding of the faded value has an error variance of 1/12 LSB^2; TPDF dither + rounding 1/4.
    DeviceFormat f;
    f.rate = 48000, f.channels = 2, f.type = SampleType::Int16, f.container_bits = 16, f.valid_bits = 16;
    RingFeeder feeder;
    feeder.configure(f);
    ByteRing ring(1 << 16);
    std::vector<uint8_t> in(4800 * 4);
    for (size_t i = 0; i < in.size(); i += 2) in[i] = 1000 & 0xFF, in[i + 1] = 1000 >> 8;
    ring.write(in.data(), in.size());
    OutputStats stats;
    std::vector<uint8_t> out(960 * 4);
    feeder.set_paused(true);
    feeder.fill(out.data(), 960, &ring, false, stats);
    double sum = 0, sum2 = 0;
    for (int i = 0; i < 960; ++i) {
        const double t = (i + 0.5) / 960.0, g = 0.5 + 0.5 * std::cos(3.14159265358979323846 * t);
        for (int c = 0; c < 2; ++c) {
            const int16_t v = static_cast<int16_t>(out[static_cast<size_t>(i) * 4 + c * 2] | out[static_cast<size_t>(i) * 4 + c * 2 + 1] << 8);
            const double err = v - 1000.0 * g;
            CHECK(std::abs(err) < 1.5);   // within the dither's +-1 LSB plus rounding
            sum += err, sum2 += err * err;
        }
    }
    const double mean = sum / 1920, var = sum2 / 1920 - mean * mean;
    CHECK(std::abs(mean) < 0.05);
    CHECK(var > 0.18);   // plain rounding: about 0.083
    CHECK(var < 0.32);
    CHECK(feeder.paused());
}

namespace {

std::atomic<std::thread::id> g_audio_thread{};
std::atomic<int> g_freed_on_audio{0};

struct Params {
    int value = 0;
    explicit Params(int v) : value(v) {}
    ~Params() {
        if (std::this_thread::get_id() == g_audio_thread.load()) ++g_freed_on_audio;
    }
};

}  // namespace

TEST_CASE("Mailbox: the audio thread never frees, and the last post is always taken", "[mailbox]") {
    for (int run = 0; run < 20; ++run) {
        auto box = std::make_unique<daphne::dsp::Mailbox<Params>>();
        std::atomic<bool> stop{false}, started{false};
        std::atomic<int> taken{0};
        std::thread audio([&] {
            g_audio_thread = std::this_thread::get_id();
            started = true;
            while (!stop)
                if (box->take()) ++taken;
        });
        // the audio thread runs before the posts start, and has taken one before it stops: otherwise a slow
        // scheduler (a CI VM) may leave it idle while all 20000 posts go by, and taken stays 0
        while (!started) std::this_thread::yield();
        constexpr int kPosts = 20000;
        for (int i = 1; i <= kPosts; ++i) box->post(std::make_unique<Params>(i));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (taken == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        stop = true;
        audio.join();
        // with the control side done, at most two more calls reach the newest object
        box->take();
        box->take();
        REQUIRE(box->current() != nullptr);
        CHECK(box->current()->value == kPosts);
        CHECK(taken > 0);
        g_audio_thread = std::thread::id{};
        box.reset();
    }
    CHECK(g_freed_on_audio == 0);
}
