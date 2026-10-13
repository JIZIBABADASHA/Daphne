#include "daphne/engine/filters.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "daphne/dsp/filter_design.h"

namespace daphne {

namespace {

constexpr char kMagic[8] = {'D', 'A', 'P', 'H', 'N', 'E', 'F', '1'};

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

std::filesystem::path cache_file(const std::string& key) {
    const std::wstring dir = filter_cache_dir();
    if (dir.empty()) return {};
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.flt", static_cast<unsigned long long>(fnv1a(key)));
    return std::filesystem::path(dir) / name;
}

// marks a cache file as used now (prune_filter_cache deletes the least recently used first)
void touch(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now(), ec);
}

int ratio_of(int fs_in, int fs_out) { return std::max(1, fs_in > 0 ? fs_out / fs_in : 1); }

// ratio 1 is a unit impulse (not worth a cache file) unless apodizing
bool cacheable(const FilterSettings& s, int fs_in, int fs_out) { return fs_out != fs_in || s.apodize; }

}  // namespace

std::string filter_cache_key(const FilterSettings& s, int fs_in, int fs_out) {
    std::ostringstream k;
    k << std::setprecision(17);   // phase_custom: values that differ must not share a file
    k << "v" << kFilterDesignVersion << "|taps=" << s.taps << "|in=" << fs_in << "|out=" << fs_out << "|phase=" << s.phase_alpha()
      << "|apod=" << s.apodize;
    return k.str();
}

std::wstring filter_cache_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"DAPHNE_FILTER_CACHE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        if (_wcsicmp(buf, L"off") == 0) return {};
        return buf;
    }
    const DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (m == 0 || m >= MAX_PATH) return {};
    return (std::filesystem::path(buf) / L"daphne" / L"filters").wstring();
}

bool load_cached_filter(const std::string& key, std::vector<double>& h) {
    const auto path = cache_file(key);
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8];
    uint32_t klen = 0;
    uint64_t n = 0;
    if (!f.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0 || !f.read(reinterpret_cast<char*>(&klen), 4) || klen > 4096)
        return false;
    std::string k(klen, '\0');
    if (!f.read(k.data(), klen) || k != key) return false;   // hash collision or another version
    if (!f.read(reinterpret_cast<char*>(&n), 8) || n == 0 || n > (1ull << 26)) return false;
    h.resize(static_cast<size_t>(n));
    if (!f.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(n * 8))) return false;
    f.close();
    touch(path);
    return true;
}

namespace {
// the file for key exists and is its own (the header only; load_cached_filter checks the rest)
bool cache_has(const std::string& key) {
    const auto path = cache_file(key);
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    uint32_t klen = 0;
    if (!f || !f.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0 || !f.read(reinterpret_cast<char*>(&klen), 4) || klen > 4096)
        return false;
    std::string k(klen, '\0');
    return f.read(k.data(), klen) && k == key;
}
}  // namespace

bool filter_cached(const FilterSettings& s, int fs_in, int fs_out) {
    if (!cacheable(s, fs_in, fs_out)) return true;
    const std::string key = filter_cache_key(s, fs_in, fs_out);
    return cache_has(key) && (!s.double_double || cache_has(key + "_lo"));
}

void store_cached_filter(const std::string& key, const std::vector<double>& h) {
    const auto path = cache_file(key);
    if (path.empty() || h.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        const uint32_t klen = static_cast<uint32_t>(key.size());
        const uint64_t n = h.size();
        f.write(kMagic, 8);
        f.write(reinterpret_cast<const char*>(&klen), 4);
        f.write(key.data(), klen);
        f.write(reinterpret_cast<const char*>(&n), 8);
        f.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(n * 8));
        if (!f) {
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);   // atomic replace: a reader sees the old file or the new one
    if (ec) std::filesystem::remove(tmp, ec);
}

std::vector<double> design_filter(const FilterSettings& s, int fs_in, int fs_out, std::vector<double>* lo) {
    const int ratio = ratio_of(fs_in, fs_out);
    const std::string key = cacheable(s, fs_in, fs_out) ? filter_cache_key(s, fs_in, fs_out) : std::string();
    std::vector<double> h;
    if (!key.empty() && load_cached_filter(key, h)) {
        if (!lo) return h;
        if (load_cached_filter(key + "_lo", *lo) && lo->size() == h.size()) return h;
    }
    // the low parts are stored only when asked for (they double the file size)
    h = dsp::design_lowpass(s.taps, ratio, fs_in, fs_out, s.phase_alpha(), s.apodize, lo);
    if (!key.empty()) {
        store_cached_filter(key, h);
        if (lo) store_cached_filter(key + "_lo", *lo);
    }
    return h;
}

std::unique_ptr<dsp::Resampler> make_resampler(const FilterSettings& s, int fs_in, int fs_out) {
    const int ratio = ratio_of(fs_in, fs_out);
    std::vector<double> lo;
    auto h = design_filter(s, fs_in, fs_out, s.double_double ? &lo : nullptr);
    return std::make_unique<dsp::Resampler>(std::move(h), ratio,
                                            dsp::resampler_block_size(static_cast<long long>(s.taps) * ratio),
                                            s.double_double ? &lo : nullptr);
}

std::vector<std::string> filter_cache_keys_in_use(const Settings& s) {
    std::vector<std::string> keys;
    auto add = [&](int fs_in, int fs_out) {
        if (!cacheable(s.filter, fs_in, fs_out)) return;
        keys.push_back(filter_cache_key(s.filter, fs_in, fs_out));
        keys.push_back(keys.back() + "_lo");
    };
    for (int fs_in : {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000}) {
        add(fs_in, pcm_output_rate(fs_in, s.output.rate));
        add(fs_in, modulator_input_rate(fs_in));
    }
    return keys;
}

CachePruneResult prune_filter_cache(uint64_t limit_bytes, const std::vector<std::string>& keep) {
    CachePruneResult r;
    const std::wstring dir = filter_cache_dir();
    if (dir.empty()) return r;
    std::vector<std::filesystem::path> kept;
    for (const auto& k : keep) kept.push_back(cache_file(k).filename());
    struct Entry {
        std::filesystem::path path;
        uint64_t size;
        std::filesystem::file_time_type used;
    };
    std::vector<Entry> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != L".flt") continue;
        const uint64_t size = e.file_size(ec);
        if (ec) continue;
        r.files++, r.bytes += size;
        if (std::find(kept.begin(), kept.end(), e.path().filename()) != kept.end()) continue;
        files.push_back({e.path(), size, e.last_write_time(ec)});
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const auto& f : files) {
        if (r.bytes <= limit_bytes) break;
        if (!std::filesystem::remove(f.path, ec) || ec) continue;   // in use (being read): keep it
        r.files--, r.bytes -= f.size;
        r.removed_files++, r.removed_bytes += f.size;
    }
    return r;
}

void trim_filter_cache(const Settings& s) {
    if (s.design.cache_limit_mb > 0)
        prune_filter_cache(static_cast<uint64_t>(s.design.cache_limit_mb) << 20, filter_cache_keys_in_use(s));
}

}  // namespace daphne
