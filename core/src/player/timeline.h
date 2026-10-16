#pragma once
// Where the stream stands (N4): the device-frame timeline of the chain output, the gapless tracks that are
// decoded but not heard yet, and from those the track and frame being heard.
//
// Device frames count from the current track's frame 0 times the ratio (abs_out: the next frame the
// producer writes). A gapless switch through a new chain (another rate, cross switch) starts a new
// segment whose input frame 0 is device frame at_out; until it is heard, the hearing side stays on
// the old segment, whose parameters the Switch keeps.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>

#include "daphne/engine/chain.h"
#include "daphne/engine/decoder.h"

namespace daphne::playback {

struct Switch {
    int64_t at_in = 0;           // stream input frame where the track starts
    AudioInfo info;
    std::filesystem::path path;
    // gapless through a new chain (another sample rate on the same output, or the same rate with a format
    // the old chain does not fit; cross_switch): the new chain's output starts at device
    // frame at_out and its first sample is heard new_delay later. Until then the hearing side stays on the
    // old segment, whose parameters are kept here; a seek or restart before it goes back to old_chain.
    bool cross = false;
    std::optional<double> old_headroom;   // the headroom before this track's own (set_next), restored on undo
    int64_t at_out = 0;
    int new_delay = 0;
    int old_ratio = 1, old_delay = 0;
    int64_t old_base = 0;
    std::unique_ptr<DspChain> old_chain;
};

struct Timeline {
    int64_t abs_out = 0;             // device frames of the current chain's timeline
    int64_t h_base = 0;              // device frame of the heard segment's input frame 0 (gapless across rates)
    int64_t track_origin_in = 0;     // stream input frame of the current track's frame 0
    std::deque<Switch> switches;     // gapless tracks decoded but not yet heard

    // Device frame of the newest segment's input frame 0 (the last pending cross switch, else the heard one).
    int64_t latest_base() const {
        for (auto it = switches.rbegin(); it != switches.rend(); ++it)
            if (it->cross) return it->at_out;
        return h_base;
    }
    bool cross_pending() const {
        for (const auto& w : switches)
            if (w.cross) return true;
        return false;
    }
    // The chain the heard track plays through: before a pending cross switch, the old one it keeps.
    const DspChain* heard_chain(const DspChain* cur) const {
        for (const auto& w : switches)
            if (w.cross) return w.old_chain.get();
        return cur;
    }
    // Stream input frame being heard, given the chain frame being heard (heard_out) and the current
    // segment's content delay and ratio (the stream starts at the first track's frame 0; after a gapless
    // switch to another sample rate, at that track's frame 0).
    int64_t heard_input(int64_t ho, int delay, int ratio) const {
        for (const auto& w : switches)   // still before a cross switch: that segment's parameters
            if (w.cross && ho < w.at_out + w.new_delay) return (ho - w.old_base - w.old_delay) / std::max(1, w.old_ratio);
        return (ho - latest_base() - delay) / std::max(1, ratio);
    }
};

}  // namespace daphne::playback
