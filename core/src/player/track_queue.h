#pragma once
// The next track (N4, gapless): what the host set with set_next, and its preparation on a background
// thread as soon as it is known: opened (a decoder, or a DSD file), and for a PCM track the current chain
// does not fit (another sample rate on the same output, or the same rate with another word length /
// DSD-to-PCM / layout: DspChain::fits) its chain built too, so the boundary does not wait for a slow disk
// or a filter design. The boundary uses what is ready and otherwise behaves as without it.

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "daphne/engine/chain.h"
#include "daphne/engine/decoder.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/player.h"
#include "daphne/engine/settings.h"

namespace daphne::playback {

struct Prep {
    std::filesystem::path path;
    std::atomic<bool> opened{false}, done{false}, cancel{false};
    std::unique_ptr<Decoder> dec;
    std::unique_ptr<DsdFile> dsdf;
    std::unique_ptr<DspChain> chain;
    std::string key;             // what the chain was built for (chain_key)
};

class TrackQueue {
public:
    // What the current playback looks like, for preparing the next track.
    struct Context {
        bool dsd = false;            // a DSD file sent directly: the next one is opened as a DSD file
        Settings cs;                 // the chain's settings
        DeviceFormat cf{};           // the chain's output format
        int chain_rate = 0;
        int channels = 0;            // the source's
        int cur_rate = 0;            // the decoding track's rate
        DspChain::Shape shape{};     // what the current chain fits
        DecoderOpener opener;
    };

    // ---- the host's choice (any thread)
    void set(const std::filesystem::path& path, std::optional<double> headroom_db);
    std::pair<std::filesystem::path, std::optional<double>> get() const;
    // np has become the decoding track: no next track any more (unless the host set another meanwhile)
    void consumed(const std::filesystem::path& np);
    // a track decoded ahead was undone (seek, restart): it is the next one again, unless the host set one
    void restore(const std::filesystem::path& path);

    // ---- preparation (processing thread)
    // Starts preparing the host's next track when it changed; make() is called only then.
    void kick(const std::function<Context()>& make);
    void drop();
    // The preparation for np, once it has opened the track (waiting up to 3 s), or null; the preparation is
    // consumed either way.
    std::shared_ptr<Prep> take(const std::filesystem::path& np, const std::atomic<bool>& cancel);
    // At the end: stops and joins every preparation thread.
    void join_all();

    static std::string chain_key(const Settings& cs, const DeviceFormat& cf);

private:
    mutable std::mutex m_;
    std::filesystem::path next_path_;      // gapless candidate (set_next)
    std::optional<double> next_headroom_;  // its headroom_db (ReplayGain), applied at the decode boundary
    std::shared_ptr<Prep> prep_;
    std::vector<std::pair<std::shared_ptr<Prep>, std::thread>> threads_;
};

}  // namespace daphne::playback
