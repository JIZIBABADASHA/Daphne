#pragma once
// ASIO output.
//
// open: init (desktop window) -> output channels clamped to the hardware -> kAsioSetIoFormat (PCM
// or DSD) -> setSampleRate, then up to 3 readbacks 5 ms apart (re-setting the rate each time) ->
// preferred buffer size (512 when the driver gives none) -> createBuffers.
// Callbacks: bufferSwitch fills the block from the ring; asioMessage answers reset / resync /
// latency-change requests.
//
//   - the driver callback reads a lock-free ring of device-format bytes (nothing else runs inside it)
//   - the device rate is the driver's readback: if the hardware did not lock the requested rate,
//     the caller sees it (instead of playing at the wrong speed)
//   - every ASIO PCM sample type is written correctly: Int32LSB16..24 right-aligned, MSB types
//     byte-swapped
//   - a driver reset request or buffer size change reopens the device (the player resumes where it
//     was); one in the first 0.5 s after start is ignored (drivers send one while starting). A
//     sample-rate change stops playback with a reason
//   - driver messages, rate changes and late callbacks go to the host's log (take_notes). The driver
//     can call asioMessage / sampleRateDidChange from its own audio thread, so those callbacks neither
//     allocate nor block: a note is formatted into a fixed slot taken with try_lock (dropped and
//     counted when the player is reading the notes at that moment or 64 are queued), the failure
//     reason into a fixed buffer published by failed(), the latency into an atomic
//   - the driver DLL is never unloaded (a driver thread still running after Release can crash the
//     host process)
//   - DSD mode: buffers the ring runs short of are filled with DSD silence (0x69), not zeros
//   - both buffers hold silence (zeros, DSD 0x69) before start, so the first period the driver plays
//     before the first callback is not whatever createBuffers left there
//   - native DSD never ends with a switch to PCM (on some DACs that switch pops, and after a PCM
//     session at close the next native DSD stream can stay silent): the DAC is left in DSD mode, and
//     the next PCM or DoP stream starts with 0.2 s of PCM zeros at its own rate so the DAC leaves it
//     (some DACs beep on one channel when DoP follows native DSD). The first PCM / DoP stream in a
//     process starts that way too (what the DAC was left in is not known)
// Needs the Steinberg ASIO SDK at build time (third_party/asiosdk); without it open() fails.

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "daphne/engine/audio_output.h"

namespace daphne {

struct AsioOptions {
    std::string driver;   // driver name, list index ("0", "1", ...), CLSID, or empty = the first driver
    // ASIO DSD mode (kAsioSetIoFormat kASIODSDFormat): rate = the DSD bit rate, DSDInt8MSB1 /
    // LSB1 buffers; open() fails when the driver has no DSD mode
    bool dsd = false;
};

class AsioOutput final : public AudioOutput {
public:
    AsioOutput();
    ~AsioOutput() override;
    AsioOutput(const AsioOutput&) = delete;
    AsioOutput& operator=(const AsioOutput&) = delete;

    bool open(const AsioOptions& opt, int rate, int channels, std::string* error);

    const DeviceFormat& format() const override { return fmt_; }
    bool matches_request() const override { return matched_; }
    std::string device_name() const override { return name_; }
    double latency_ms() const override { return latency_ms_.load(); }
    int buffer_frames() const override { return buffer_frames_; }

    bool start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) override;
    bool drained() const override { return drained_.load(); }
    void rearm() override { drained_ = false; }
    bool failed() const override { return failed_.load(); }
    std::string failure() const override;
    void stop() override;
    void close();

    const OutputStats& stats() const override { return stats_; }
    std::vector<std::string> take_notes() override;
    bool reset_requested() const override { return reset_.load(); }
    // PCM rates the driver accepts (canSampleRate while opening in PCM mode; empty in DSD mode).
    std::vector<int> supported_rates() const override { return rates_; }
    bool probe_before_design() const override { return true; }

    // driver callbacks (static trampolines forward here)
    void on_buffer(long index);
    void on_rate_changed(double rate);
    long on_message(long selector, long value);

private:
    // printf-style; no allocation, never blocks (see above)
    void note(const char* fmt, ...);

public:

private:
    struct Impl;
    Impl* impl_;
    DeviceFormat fmt_{};
    bool matched_ = false;
    std::string name_;
    std::atomic<double> latency_ms_{0.0};
    int buffer_frames_ = 0;
    ByteRing* ring_ = nullptr;
    const std::atomic<bool>* eof_ = nullptr;
    std::atomic<bool> drained_{false};
    std::atomic<bool> failed_{false};
    char failure_[160] = {};   // written before failed_ is set (release), read after it (acquire)
    std::vector<uint8_t> tmp_;
    std::vector<int> rates_;
    OutputStats stats_;
    std::atomic<bool> reset_{false};
    int64_t period_us_ = 0;   // one buffer
    std::mutex notes_m_;
    std::array<std::array<char, 192>, 64> notes_{};
    int notes_n_ = 0;
    std::atomic<int> notes_dropped_{0};
};

}  // namespace daphne
