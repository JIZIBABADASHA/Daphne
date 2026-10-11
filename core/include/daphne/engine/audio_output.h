#pragma once
// Common interface of the device outputs (WASAPI, ASIO): the player writes device-format
// bytes into a lock-free ring, the output's render thread / driver callback reads them.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "daphne/engine/ring_buffer.h"
#include "daphne/engine/ring_feeder.h"
#include "daphne/engine/sample_format.h"

namespace daphne {

struct OutputStats {
    std::atomic<int64_t> callbacks{0};
    std::atomic<int64_t> underruns{0};        // callbacks that found too little data before EOF
    std::atomic<int64_t> event_timeouts{0};   // WASAPI: > 50 ms without a device event
    std::atomic<int64_t> max_interval_us{0};
    std::atomic<int64_t> late_callbacks{0};   // ASIO: callbacks more than 1.5 buffer periods apart
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual const DeviceFormat& format() const = 0;
    // The device runs at the requested rate and channel count.
    virtual bool matches_request() const = 0;
    virtual std::string device_name() const = 0;
    virtual double latency_ms() const = 0;
    virtual int buffer_frames() const = 0;

    // Start reading from ring. When eof is set and the ring runs dry, drained() becomes true
    // (the device keeps getting silence until stop()).
    virtual bool start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) = 0;
    virtual bool drained() const = 0;
    // A seek refilled the ring after the end of the stream: drained() becomes false again.
    virtual void rearm() = 0;
    virtual bool failed() const = 0;
    // Why failed() became true.
    virtual std::string failure() const { return "device invalidated during playback"; }
    virtual void stop() = 0;

    virtual const OutputStats& stats() const = 0;

    // Diagnostics for the host's log (driver messages, rate changes), oldest first; the player
    // drains them while playing.
    virtual std::vector<std::string> take_notes() { return {}; }
    // The driver asked to be opened again (ASIO reset request / buffer size change): the player
    // reopens the device and resumes where it was.
    virtual bool reset_requested() const { return false; }

    // Pause without stopping the device (ring_feeder.h): PCM fades out / in, DSD gets silence.
    void set_paused(bool p) { feeder_.set_paused(p); }
    bool paused() const { return feeder_.paused(); }
    // The stream is DoP: pausing keeps the marker sequence going.
    void set_dop(bool d) { feeder_.set_dop(d); }
    // Peak of what is being heard since the last call (PCM).
    void meter(double& l, double& r) { feeder_.meter(l, r); }

protected:
    RingFeeder feeder_;
};

}  // namespace daphne
