#pragma once
// What a settings change needs (N4, "播放中改設定"), decided from the settings alone:
//   in place  DspChain::update takes it (gain, crossfeed, EQ, headroom)
//   rebuild   a new chain built in the background and crossfaded in (filter, final quantization)
//   restart   the device is reopened where it was (core, output rate, device, modulator, decoder, buffer)
// The in-place / rebuild split is the chain's own (DspChain::update); the restart is decided here.

#include <filesystem>

#include "daphne/engine/player.h"
#include "daphne/engine/settings.h"

namespace daphne::playback {

struct Playing {
    const Settings& s;                  // the settings playing now
    const OutputChoice& out;
    int want;                           // the device rate asked for when the device was opened
    int source_rate;                    // the heard track's
    const std::filesystem::path& path;  // the heard track's
};

// The decoder is built from these settings (a DSD file converted to PCM: its low-pass).
bool decoder_changes(const Playing& now, const Settings& ns);
// The device buffer the player sets (WASAPI; ASIO keeps the driver's).
bool buffer_changes(const Playing& now, const Settings& ns);
// The device has to be reopened.
bool needs_restart(const Playing& now, const Settings& ns, const OutputChoice& no);
// A DSD file sent directly: only the output can change.
bool dsd_direct_reopens(const Playing& now, const Settings& ns, const OutputChoice& no);

}  // namespace daphne::playback
