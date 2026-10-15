// Core Audio output on a real Mac (hidden: run with daphne_tests "[.coreaudio]"; CI runners have no
// dependable audio device). Plays at -60 dBFS on the system output, so it is barely audible.
#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "coreaudio_util.h"
#include "daphne/engine/coreaudio_output.h"
#include "daphne/engine/devices.h"
#include "daphne/engine/player.h"
#include "fake_output.h"

using namespace daphne;

namespace {

constexpr double kPi = 3.141592653589793;

CoreAudioDevice find(const std::string& uid) {
    for (auto& d : list_coreaudio_devices())
        if (uid.empty() ? d.is_default : d.uid == uid) return d;
    return {};
}

int other_rate(const CoreAudioDevice& d) {
    for (int r : {96000, 48000, 88200, 44100})
        if (r != d.rate && std::find(d.rates.begin(), d.rates.end(), r) != d.rates.end()) return r;
    return 0;
}

AudioDeviceID device_of(const std::string& uid) {
    for (auto d : ca::output_devices())
        if (ca::string_prop(d, kAudioDevicePropertyDeviceUID) == uid) return d;
    return kAudioObjectUnknown;
}

// the physical (DAC) format's bits of the device's first output stream, 0 for a float or unknown format
int physical_bits(const std::string& uid) {
    const auto streams = ca::output_streams(device_of(uid));
    AudioStreamBasicDescription f{};
    if (streams.empty() || !ca::get(streams[0], kAudioStreamPropertyPhysicalFormat, f) || !ca::is_integer_pcm(f)) return 0;
    return static_cast<int>(f.mBitsPerChannel);
}

// the most integer bits any physical format offers at this rate
int offered_bits(const std::string& uid, int rate) {
    const auto streams = ca::output_streams(device_of(uid));
    int best = 0;
    if (streams.empty()) return 0;
    for (const auto& r : ca::list<AudioStreamRangedDescription>(streams[0], kAudioStreamPropertyAvailablePhysicalFormats))
        if (ca::is_integer_pcm(r.mFormat) && rate >= r.mSampleRateRange.mMinimum - 0.5 && rate <= r.mSampleRateRange.mMaximum + 0.5)
            best = std::max(best, static_cast<int>(r.mFormat.mBitsPerChannel));
    return best;
}

// 0.3 s of a quiet 1 kHz sine as int24-in-32 stereo, then the end
void fill_ring(ByteRing& ring, int rate) {
    const int n = rate * 3 / 10;
    std::vector<int32_t> buf(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<int32_t>(std::lround(0.001 * std::sin(2 * kPi * 1000.0 * i / rate) * 8388607.0)) << 8;
        buf[2 * static_cast<size_t>(i)] = buf[2 * static_cast<size_t>(i) + 1] = v;
    }
    REQUIRE(ring.write(buf.data(), buf.size() * 4) == buf.size() * 4);
}

bool wait_for(const std::function<bool()>& f, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!f()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

}  // namespace

TEST_CASE("Core Audio: exclusive takes hog mode and the rate, and puts both back", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    REQUIRE(before.hog_pid == -1);
    const int want = other_rate(before);
    if (!want) SKIP("the system output offers one rate only");
    INFO(before.name << ": " << before.rate << " Hz -> " << want << " Hz");
    {
        CoreAudioOutput o;
        std::string e;
        REQUIRE(o.open({before.uid, true, 0}, want, 2, &e));
        for (const auto& n : o.take_notes()) UNSCOPED_INFO(n);
        CHECK(o.matches_request());
        CHECK(o.format().rate == want);
        CHECK(o.format().type == SampleType::Int24In32);
        const auto during = find(before.uid);
        CHECK(during.rate == want);
        CHECK(during.hog_pid == getpid());
        if (offered_bits(before.uid, want) >= 24) CHECK(physical_bits(before.uid) >= 24);

        ByteRing ring(static_cast<size_t>(want) * 8);
        fill_ring(ring, want);
        std::atomic<bool> eof{true};
        REQUIRE(o.start(&ring, &eof, &e));
        CHECK(wait_for([&] { return o.drained(); }, 3.0));
        CHECK(o.stats().callbacks > 0);
        CHECK_FALSE(o.failed());
        o.close();
    }
    const auto after = find(before.uid);
    CHECK(after.rate == before.rate);
    CHECK(after.hog_pid == -1);
}

TEST_CASE("Core Audio: shared keeps the device's rate and no hog mode", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    const int want = other_rate(before);
    CoreAudioOutput o;
    std::string e;
    REQUIRE(o.open({before.uid, false, 0}, want ? want : before.rate, 2, &e));
    CHECK(o.format().rate == before.rate);
    CHECK(o.matches_request() == (!want || want == before.rate));
    CHECK(find(before.uid).hog_pid == -1);
    ByteRing ring(static_cast<size_t>(before.rate) * 8);
    fill_ring(ring, before.rate);
    std::atomic<bool> eof{true};
    REQUIRE(o.start(&ring, &eof, &e));
    CHECK(wait_for([&] { return o.drained(); }, 3.0));
    o.close();
    CHECK(find(before.uid).rate == before.rate);
}

TEST_CASE("Core Audio: the player plays a track to the end, exclusive", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    auto sig = std::make_shared<test::Signal>();
    sig->info.rate = 96000;
    sig->info.channels = 2;
    sig->info.frames = 96000 / 2;
    sig->info.codec = "test";
    for (int i = 0; i < 96000 / 2; ++i) {
        const double v = 0.001 * std::sin(2 * kPi * 1000.0 * i / 96000.0);
        sig->x.push_back(v), sig->x.push_back(v);
    }
    PlayerDeps deps;
    deps.open_decoder = [sig](const std::filesystem::path&, std::string*) -> std::unique_ptr<Decoder> {
        return std::make_unique<test::MemoryDecoder>(sig);
    };
    Player p(deps);
    Settings s;
    s.filter.taps = 256;
    std::string e;
    REQUIRE(p.start("sine", s, OutputChoice{OutputApi::CoreAudioExclusive, before.uid}, {}, &e));
    PlayReport rep;
    REQUIRE(p.wait(&rep, &e));
    CHECK(p.ended());
    INFO(rep.device_name << ", " << describe(rep.device));
    CHECK(rep.device.type == SampleType::Int24In32);
    p.stop();
    CHECK(find(before.uid).hog_pid == -1);
    CHECK(find(before.uid).rate == before.rate);
}

TEST_CASE("Core Audio: reopened at another rate right after closing (a player restart)", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    const int want = other_rate(before);
    if (!want) SKIP("the system output offers one rate only");
    std::string e;
    {
        CoreAudioOutput a;
        REQUIRE(a.open({before.uid, true, 0}, before.rate, 2, &e));
        ByteRing ring(static_cast<size_t>(before.rate) * 8);
        std::atomic<bool> eof{false};
        REQUIRE(a.start(&ring, &eof, &e));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        a.stop();
    }   // closed here, as Player::Impl::restart does before opening the new output
    CoreAudioOutput b;
    const bool ok = b.open({before.uid, true, 0}, want, 2, &e);
    INFO("error: " << e);
    for (const auto& n : b.take_notes()) UNSCOPED_INFO(n);
    REQUIRE(ok);
    CHECK(b.format().rate == want);
    CHECK(find(before.uid).hog_pid == getpid());
    b.close();
    CHECK(find(before.uid).rate == before.rate);
    CHECK(find(before.uid).hog_pid == -1);
}
