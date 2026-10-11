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
//   - driver messages, rate changes and late callbacks go to the host's log (take_notes)
//   - the driver DLL is never unloaded (a driver thread still running after Release can crash the
//     host process)
//   - DSD mode: buffers the ring runs short of are filled with DSD silence (0x69), not zeros
//   - after native DSD, close() runs a 0.2 s PCM 44.1 kHz session of zeros so the DAC leaves DSD
//     mode (some DACs otherwise beep on one channel)
// Needs the Steinberg ASIO SDK at build time (third_party/asiosdk); without it open() fails.

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
    double latency_ms() const override { return latency_ms_; }
    int buffer_frames() const override { return buffer_frames_; }

    bool start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) override;
    bool drained() const override { return drained_.load(); }
    void rearm() override { drained_ = false; }
    bool failed() const override { return failed_.load(); }
    std::string failure() const override { return failure_; }
    void stop() override;
    void close();

    const OutputStats& stats() const override { return stats_; }
    std::vector<std::string> take_notes() override;
    bool reset_requested() const override { return reset_.load(); }
    // PCM rates the driver accepts (canSampleRate while opening in PCM mode; empty in DSD mode).
    const std::vector<int>& supported_rates() const { return rates_; }

    // driver callbacks (static trampolines forward here)
    void on_buffer(long index);
    void on_rate_changed(double rate);
    long on_message(long selector, long value);

private:
    void pcm_silence_session();
    void note(const std::string& s);

public:

private:
    struct Impl;
    Impl* impl_;
    DeviceFormat fmt_{};
    bool matched_ = false;
    std::string name_;
    double latency_ms_ = 0;
    int buffer_frames_ = 0;
    ByteRing* ring_ = nullptr;
    const std::atomic<bool>* eof_ = nullptr;
    std::atomic<bool> drained_{false};
    std::atomic<bool> failed_{false};
    std::string failure_;
    std::vector<uint8_t> tmp_;
    std::vector<int> rates_;
    OutputStats stats_;
    std::atomic<bool> reset_{false};
    int64_t period_us_ = 0;   // one buffer
    std::mutex notes_m_;
    std::vector<std::string> notes_;
};

}  // namespace daphne
