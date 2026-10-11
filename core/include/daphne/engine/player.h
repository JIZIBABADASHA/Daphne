#pragma once
// Playback: decoder -> processing chain (chain.h) -> device-format conversion -> lock-free ring ->
// WASAPI / ASIO.
//
// Settings can change during playback (Player::apply), on the processing thread at chunk boundaries:
//   in place  EQ, crossfeed, volume, headroom: the gain ramps, the EQ glides or crossfades
//   rebuild   filter, final quantization: the new chain is built on a background thread, warmed up
//             on the recent input, aligned by content delay and crossfaded in (raised cosine, 50 ms)
//   restart   core, output rate, output device: the device is reopened and playback resumes at
//             the current position

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "daphne/engine/asio_output.h"
#include "daphne/engine/decoder.h"
#include "daphne/engine/sample_format.h"
#include "daphne/engine/settings.h"
#include "daphne/engine/wasapi_output.h"

namespace daphne {

struct PlayReport {
    AudioInfo source;
    DeviceFormat device;
    std::string device_name;
    bool bit_perfect = false;
    std::string not_bit_perfect_reason;
    int64_t frames_played = 0;   // device frames written
    int64_t underruns = 0;
    int64_t event_timeouts = 0;
    int64_t clipped = 0;
    int64_t max_interval_us = 0;
    double latency_ms = 0;
    std::vector<std::string> chain;   // active DSP stages
    std::string filter;               // resampling filter in use, e.g. "4096 taps, minimum phase"
};

struct PlayCallbacks {
    std::function<void(const PlayReport&)> on_start;              // format decided, playback starting (again after a restart)
    std::function<void(int64_t frame, int64_t total)> on_progress; // from the DSP thread
    std::function<void(const std::string&)> on_event;              // a settings change was applied
    std::function<void(double fraction)> on_design;                // filter design progress 0..1 (1 = done)
    std::function<void(const PlayReport&)> on_track_started;       // gapless: the next track (set_next) is now heard
    std::function<void()> on_ended;                                // the end of the last track was heard
    std::function<void(const PlayReport&)> on_changed;             // the active filter / DSP chain changed (rebuild done, in-place update)
    std::function<void(const std::string&)> on_failed;             // playback stopped with an error (device lost, ...)
};

// Device choice; the CLI fills it from the settings plus its overrides.
struct OutputChoice {
    OutputApi api = OutputApi::WasapiExclusive;
    std::string device;   // WASAPI endpoint / index, or ASIO driver name / index / CLSID
    bool operator==(const OutputChoice& o) const { return api == o.api && device == o.device; }
};

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Opens the file and the device on the DSP thread; returns once playback has started (or failed).
    // start_seconds: position to start from.
    bool start(const std::wstring& path, const Settings& s, const OutputChoice& out, PlayCallbacks cb, std::string* error,
               double start_seconds = 0.0);
    // New settings, applied by the DSP thread; the latest call wins.
    void apply(const Settings& s, const OutputChoice& out);
    void stop();
    bool finished() const;
    // Waits for the end of playback.
    bool wait(PlayReport* report, std::string* error);
    // Source frame being heard (approximately), in the current track.
    int64_t position() const;
    double position_seconds() const;

    // Transport (handled by the DSP thread at chunk boundaries; the device stays open).
    // Pause: the device keeps running, PCM fades out (20 ms), DSD gets DSD / DoP silence.
    void pause(bool paused);
    bool paused() const;
    void seek(double seconds);
    // Gapless: the track to play when this one ends, when it has the same format (PCM: rate and
    // channels; DSD: DSD rate and channels); otherwise playback ends and the host starts it.
    // Empty clears it. on_track_started reports when it is heard. headroom_db: the next track's own
    // headroom (ReplayGain per track), applied where its first sample is decoded, so a gapless album in
    // track mode gets each track's gain on time; nullopt keeps the current one.
    void set_next(const std::wstring& path, std::optional<double> headroom_db = std::nullopt);
    // Ended by reaching the end of the last track (not stopped, not failed).
    bool ended() const;
    // start() failed because the output device could not be opened (not because of the file).
    bool last_failure_was_device() const;
    // Peak level heard since the last call (0..1; PCM only).
    void meter(double& l, double& r);
    // ASIO: PCM rates the open driver accepts (empty when not ASIO / DSD mode).
    std::vector<int> asio_rates() const;
    // Internal state for logs (JSON).
    std::string debug_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Plays one file to the end (or until cancel becomes true). Returns false on error.
bool play_file(const std::wstring& path, const Settings& s, const OutputChoice& out, const std::atomic<bool>& cancel,
               const PlayCallbacks& cb, PlayReport* report, std::string* error);

}  // namespace daphne
