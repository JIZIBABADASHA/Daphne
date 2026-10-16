#pragma once
// The resampling filter from the settings, with the disk cache.
//
// Every design is keyed by all of its inputs plus a design-algorithm version and kept in
// <user cache>/daphne/filters (platform::user_cache_dir: %LOCALAPPDATA% on Windows, ~/Library/Caches on
// macOS, $XDG_CACHE_HOME on Linux; DAPHNE_FILTER_CACHE=<dir> moves it, =off disables it). A filter is
// designed only when playback needs it: a later track at another rate designs its own.
//
// The cache is kept under design.cache_limit_mb: every load or store marks the file as used now,
// and prune_filter_cache deletes the least recently used files above the limit, never the ones the
// current settings use (filter_cache_keys_in_use). The player runs trim_filter_cache after playback
// starts and after a settings change.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "daphne/dsp/resampler.h"
#include "daphne/engine/settings.h"

namespace daphne {

// Bump whenever the design changes its output: old cache files are then ignored.
constexpr int kFilterDesignVersion = 1;

// Designs the filter for fs_in -> fs_out (integer ratio; the ratio is fs_out / fs_in, at least 1),
// or loads it from the cache. Returns taps * ratio coefficients with DC gain 1; lo gets the
// double-double low parts (cached next to them). Ratio 1 without apodizing is a unit impulse and
// is not cached.
std::vector<double> design_filter(const FilterSettings& s, int fs_in, int fs_out, std::vector<double>* lo = nullptr);

// The resampler for fs_in -> fs_out (double-double convolution with s.double_double).
std::unique_ptr<dsp::Resampler> make_resampler(const FilterSettings& s, int fs_in, int fs_out);

// ------------------------------------------------------------------ cache

std::string filter_cache_key(const FilterSettings& s, int fs_in, int fs_out);
// Cache directory, or empty when the cache is disabled.
std::filesystem::path filter_cache_dir();
bool load_cached_filter(const std::string& key, std::vector<double>& h);
// Whether design_filter / make_resampler will find the filter in the cache (with its low parts for
// double-double; a ratio-1 filter without apodizing is a unit impulse, never designed). Reads the headers only.
bool filter_cached(const FilterSettings& s, int fs_in, int fs_out);
void store_cached_filter(const std::string& key, const std::vector<double>& h);

// Cache keys the settings use: the PCM filter and the modulator's pre-filter for the source rates
// 44.1k .. 384k (with their low parts), whichever core is chosen.
std::vector<std::string> filter_cache_keys_in_use(const Settings& s);

struct CachePruneResult {
    uint64_t files = 0, bytes = 0;            // left in the cache
    uint64_t removed_files = 0, removed_bytes = 0;
};
// Deletes the least recently used cache files until the cache is at most limit_bytes (0 = delete
// all it may), keeping the files of `keep` (cache keys) whatever their size.
CachePruneResult prune_filter_cache(uint64_t limit_bytes, const std::vector<std::string>& keep);
// prune_filter_cache with design.cache_limit_mb, keeping the settings' filters (nothing when the limit is 0).
void trim_filter_cache(const Settings& s);

}  // namespace daphne
