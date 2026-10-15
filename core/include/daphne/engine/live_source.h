#pragma once
// Live input: a float32 stereo ring in memory the host shares with an audio producer it does not
// control (a browser page's audio tap), read as a decoder opened with the path kLivePath.
//
// Ring layout (set_live_bus): uint32 write index in frames (wraps) | 12 bytes reserved | float32
// stereo frames, cap_frames of them. The producer writes the frames, then the index.
//
// The producer runs on its own clock and delivers in bursts, so the decoder keeps a jitter buffer
// of 0.3 s in the ring and holds it there by changing its read rate by at most 0.5 % (smoothed;
// in practice well under 0.3 %), with 4-point Hermite interpolation. Nothing is dropped or
// repeated, so there are no clicks. It restarts close to the writer when it was lapped, the page
// reloaded, or more than 1.2 s piled up (a stall: the player was paused). With nothing written yet
// it returns silence at once (the player prefills its ring); once running, it waits up to 20 ms
// for the producer and then returns 5 ms of silence (a starve), so the player never blocks for long.
//
// Output: info().rate = the producer's rate when opened, 2 channels, frames -1 (no end), not
// seekable (seek() only moves the position counter: the stream goes on from now).

#include <cstdint>
#include <memory>

#include "daphne/engine/decoder.h"

namespace daphne {

inline constexpr char kLivePath[] = "live:";
inline bool is_live_path(const std::filesystem::path& p) { return p == std::filesystem::path(kLivePath); }

// The host's shared ring (base = nullptr detaches it) and the producer's sample rate.
void set_live_bus(void* base, int cap_frames, int rate);
void set_live_rate(int rate);
// What the player takes from the live decoder at once when it refills its ring after a seek, in seconds: a
// seek goes back to that much more than the target behind the writer, so the decoder holds the target once
// the player's ring is full again (the player sets it for each live source it opens). Priming (the start,
// after a starve) leaves only the target: the player's ring is already full of the silence returned meanwhile.
void set_live_reserve(double seconds);

struct LiveStats {
    double fill = 0;        // seconds buffered in the ring (written, not yet read)
    double read = 0;        // seconds the live decoder has read (its position)
    int64_t resyncs = 0;    // restarts close to the writer
    int64_t starves = 0;    // silence returned while running (the producer fell behind)
    double adjust = 0;      // the read rate correction (relative)
    double target = 0;      // the buffer it aims for now (seconds; 0.15, more after starves, kept between sources, N2)
    bool active = false;    // a live decoder is open
};
LiveStats live_stats();
// The live decoder keeps the jitter buffer target it has learned for the life of the process (a later live
// source starts from it); this goes back to the minimum (tests).
void live_forget();

std::unique_ptr<Decoder> make_live_decoder();

}  // namespace daphne
