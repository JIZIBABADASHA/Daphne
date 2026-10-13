#pragma once
// Live input: a float32 stereo ring in memory the host shares with an audio producer it does not
// control (a browser page's audio tap), read as a decoder opened with the path kLivePath.
//
// Ring layout (set_live_bus): uint32 write index in frames (wraps) | 12 bytes reserved | float32
// stereo frames, cap_frames of them. The producer writes the frames, then the index.
//
// The producer runs on its own clock and delivers in bursts, so the decoder keeps a jitter buffer
// of 0.3 s in the ring and holds it there by changing its read rate by at most 0.5 % (smoothed;
// in practice well under 0.3 %). The read position is interpolated with a 64-tap Kaiser-windowed
// sinc (512 phases): flat to 19 kHz at 48 kHz, spurious products about -115 dB. While it runs
// nothing is dropped or repeated.
//
// The exceptions fade (5 ms) instead of stepping:
//   - start: silence at once until 0.3 s is buffered (the player prefills its ring), then a fade-in
//   - starve (the producer fell behind): after waiting up to 20 ms, a fade-out on the last 5 ms held
//     back for it, then 5 ms of silence per call until 0.3 s is buffered again, then a fade-in
//   - more than 1.2 s piled up (a stall: the player was paused): fade out, restart close to the
//     writer, fade in
//   - lapped by the writer or the page reloaded: restart close to the writer and fade in (what was
//     held is overwritten, so this one cannot fade out)
// The write index is compared as a signed difference, so its wrap at 2^32 frames is not a restart.
//
// Output: info().rate = the producer's rate when opened, 2 channels, frames -1 (no end), not
// seekable (seek() only moves the position counter: the stream goes on from now).

#include <cstdint>
#include <memory>

#include "daphne/engine/decoder.h"

namespace daphne {

inline constexpr const wchar_t* kLivePath = L"live:";

// The host's shared ring and the producer's sample rate. base = nullptr detaches it: the call returns
// once no decoder is reading the old memory, so the host may free it then.
void set_live_bus(void* base, int cap_frames, int rate);
void set_live_rate(int rate);

struct LiveStats {
    double fill = 0;        // seconds buffered in the ring (written, not yet read)
    double read = 0;        // seconds the live decoder has read (its position)
    int64_t resyncs = 0;    // restarts close to the writer
    int64_t starves = 0;    // times the producer fell behind while running (each faded out, then refilled)
    double adjust = 0;      // the read rate correction (relative)
    bool active = false;    // a live decoder is open
};
LiveStats live_stats();

std::unique_ptr<Decoder> make_live_decoder();

}  // namespace daphne
