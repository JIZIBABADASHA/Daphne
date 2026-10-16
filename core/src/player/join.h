#pragma once
// The gapless join through a new chain (N4), made a chunk at a time on the processing thread instead of
// all at the boundary: the old chain's tail (zeros in, then its flush) with the next track through the
// new chain added on top, aligned so that its first sample follows the last one of the track before.

#include <cstdint>
#include <memory>

#include "common.h"
#include "daphne/engine/chain.h"

namespace daphne::playback {

struct Join {
    DspChain* old = nullptr;                 // the previous chain: its Switch owns it until the join is heard,
    std::unique_ptr<DspChain> owned;         // then the join
    int64_t zeros_left = 0;                  // input frames of zeros still to feed it
    bool old_done = false;                   // flushed: its tail is all in `a`
    Fifo a, b;                               // its tail / the new chain's output, from the join's next frame on
    int64_t t = 0;                           // join frames made
    int64_t b_from = 0;                      // join frame of the new chain's output (frames before: the tail alone)
    int64_t skip_b = 0;                      // new-chain frames before the join's first frame, to drop
    bool b_ended = false;                    // the next track ended inside the join

    bool active() const { return old != nullptr; }
    void clear() { *this = Join{}; }
};

}  // namespace daphne::playback
