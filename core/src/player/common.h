#pragma once
// What the parts of the player share (N4, N25): the processing chunk, the decisions that only depend on
// the settings, and the interleaved frame FIFO of the crossfade and the gapless join.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "daphne/engine/player.h"
#include "daphne/engine/settings.h"
#include "daphne/platform/platform.h"

namespace daphne::playback {

// For a task started with std::async: MSVC runs those on a thread pool and reuses its threads, so the
// priority and the floating-point control a task sets would stay with the thread for the next task (a
// filter design, say). Restores both when the task ends (the pool's threads run at normal priority).
class PoolThreadScope {
public:
    PoolThreadScope() : fp_(platform::fp_control()) {}
    ~PoolThreadScope() {
        platform::set_fp_control(fp_);
        platform::set_thread_priority(platform::Priority::Normal);
    }
    PoolThreadScope(const PoolThreadScope&) = delete;
    PoolThreadScope& operator=(const PoolThreadScope&) = delete;

private:
    uint64_t fp_;
};

constexpr int kChunk = 4096;           // source frames the processing thread decodes at a time
constexpr double kFadeSeconds = 0.05;  // crossfade to a rebuilt chain
constexpr double kPi = 3.141592653589793;

// What the output API offers, decided before a device is open: native DSD (ASIO), a mixer that is never
// bit-perfect and cannot carry DSD (WASAPI shared), a buffer length the player sets (WASAPI).
inline bool api_has_native_dsd(OutputApi a) { return a == OutputApi::Asio; }
inline bool api_is_shared(OutputApi a) { return a == OutputApi::WasapiShared; }
inline bool api_takes_buffer_ms(OutputApi a) { return a != OutputApi::Asio; }

// The device rate asked for: pcm core, the output rate for the source; dsd core, the DoP rate
// (DSD clock / 16).
int requested_rate(const Settings& s, int fs_in);

// Whether a chain for a source at `rate` plays on the output now running (chain output rate
// chain_rate) without reopening the device: pcm, a device rate the settings give the new source
// (stepped down by powers of two as the output session does); dsd, the same modulator input rate (the
// DSD clock does not depend on the source).
bool same_output(const Settings& cs, int rate, int chain_rate);

// Modulator settings that need a new modulator (a device restart).
std::string modulator_key(const Settings& s);

// Settings as the chain sees them. osr > 0: the DSD multiple the device runs (the dsd core may have
// stepped down; the chain's limiter follows the modulator's stable range).
Settings chain_settings(const Settings& s, int osr = 0);

// "4096 taps, minimum phase (alpha 1.00), apodize" for messages.
std::string describe_filter(const Settings& x);
// "a -> b -> c", "none" when empty.
std::string join(const std::vector<std::string>& v);

// Interleaved frame FIFO for the crossfade.
struct Fifo {
    std::vector<double> d;
    size_t off = 0;   // values
    int ch = 1;
    int frames() const { return static_cast<int>((d.size() - off) / ch); }
    void push(const double* x, int n) { d.insert(d.end(), x, x + static_cast<size_t>(n) * ch); }
    void push_zeros(int n) { d.insert(d.end(), static_cast<size_t>(n) * ch, 0.0); }
    const double* data() const { return d.data() + off; }
    void pop(int n) {
        off += static_cast<size_t>(n) * ch;
        if (off >= d.size()) d.clear(), off = 0;
        else if (off > (1u << 20)) d.erase(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(off)), off = 0;
    }
    void clear() { d.clear(), off = 0; }
};

}  // namespace daphne::playback
