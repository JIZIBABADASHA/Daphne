#pragma once
// What the player tells the host (N4): the report (formats, bit-perfect and why, filter, stages), the
// callbacks, the filter design progress and the internal state for logs.

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "daphne/engine/player.h"
#include "daphne/engine/settings.h"

namespace daphne {
class DspChain;
class DsdEncoder;
}  // namespace daphne

namespace daphne::playback {

class Report {
public:
    PlayReport rep;
    PlayCallbacks cb;
    std::atomic<double> design_prog{0.0};   // the filter design's progress sink (dsp::ProgressScope)
    bool shared_mix = false;                 // the open output is a system mixer (OutputCaps): never bit-perfect

    void event(const std::string& e) const {
        if (cb.on_event) cb.on_event(e);
    }
    // the design progress to the host, at most every 250 ms unless forced
    void design(bool force = false);
    // rep.bit_perfect and its reason for the heard track and the chain playing it (PCM path; a DSD file sent
    // directly sets its own). Again whenever either changes: a gapless track, an in-place change, a new chain.
    void bit_perfect(const DspChain* heard);
    // What the resampler is set to, for messages.
    std::string filter(const Settings& s, const DsdEncoder* enc) const;

    // diagnostics (written by the processing thread, read by any)
    bool diag_due();   // 500 ms since the last update
    void set_diag(std::string d);
    std::string diag_json() const;
    void set_asio_rates(std::vector<int> r);
    std::vector<int> asio_rates() const;

private:
    mutable std::mutex m_;
    std::string diag_;
    std::vector<int> asio_rates_;
    std::chrono::steady_clock::time_point diag_at_{}, design_at_{};
};

}  // namespace daphne::playback
