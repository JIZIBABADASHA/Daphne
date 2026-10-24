#include "daphne/engine/file_bytes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>

#include "daphne/platform/platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace daphne {

struct FileBytes::State {
    std::unique_ptr<uint8_t[]> data;
    int64_t size = 0;
    std::string name;   // UTF-8 file name, for the log
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int64_t> loaded{0};   // bytes [0, loaded) are read and never written again
    bool done = false;                // the loader stopped (end, read error or cancel); under m
    std::atomic<bool> cancel{false};
};

namespace {

constexpr size_t kChunk = size_t{2} << 20;   // one disk read
constexpr int64_t kSlowMs = 500;
constexpr size_t kCached = 2;                // the playing track and the prepared next one

std::mutex g_slow_m;
SlowRead g_slow;
std::atomic<int64_t> g_slow_count{0};

// The file for the loader. Windows: shared for delete too, so the file can be renamed or deleted while it is read.
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) {
#ifdef _WIN32
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
#else
        f_ = std::fopen(path.c_str(), "rb");
#endif
    }
    ~Reader() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
        if (f_) std::fclose(f_);
#endif
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    explicit operator bool() const {
#ifdef _WIN32
        return h_ != INVALID_HANDLE_VALUE;
#else
        return f_ != nullptr;
#endif
    }
    size_t read(uint8_t* dst, size_t n) {
#ifdef _WIN32
        DWORD got = 0;
        return ReadFile(h_, dst, static_cast<DWORD>(n), &got, nullptr) ? got : 0;
#else
        return std::fread(dst, 1, n, f_);
#endif
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    FILE* f_ = nullptr;
#endif
};

void load(const std::shared_ptr<FileBytes::State>& s, const std::shared_ptr<Reader>& f) {
    int64_t pos = 0;
    while (pos < s->size && !s->cancel.load(std::memory_order_relaxed)) {
        const size_t n = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(kChunk), s->size - pos));
        const auto t0 = std::chrono::steady_clock::now();
        const size_t got = f->read(s->data.get() + pos, n);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms >= kSlowMs) {
            std::lock_guard<std::mutex> l(g_slow_m);
            g_slow.count++;
            g_slow.ms = ms;
            g_slow.file = s->name;
            g_slow_count.store(g_slow.count, std::memory_order_release);
        }
        if (got == 0) break;   // a read error (or the file shrank): readers get what was read
        pos += static_cast<int64_t>(got);
        {
            std::lock_guard<std::mutex> l(s->m);
            s->loaded.store(pos, std::memory_order_release);
        }
        s->cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> l(s->m);
        s->done = true;
    }
    s->cv.notify_all();
}

struct Entry {
    std::filesystem::path path;
    int64_t size = 0;
    std::filesystem::file_time_type mtime{};
    std::shared_ptr<FileBytes> bytes;
};
std::mutex g_cache_m;
std::deque<Entry> g_cache;   // the most recent first

}  // namespace

FileBytes::FileBytes(std::shared_ptr<State> s) : s_(std::move(s)) {}

FileBytes::~FileBytes() { s_->cancel = true; }

int64_t FileBytes::size() const { return s_->size; }

size_t FileBytes::read(int64_t off, void* dst, size_t n) {
    if (off < 0 || off >= s_->size || n == 0) return 0;
    const int64_t end = std::min(s_->size, off + static_cast<int64_t>(n));
    int64_t have = s_->loaded.load(std::memory_order_acquire);
    if (have < end) {
        std::unique_lock<std::mutex> l(s_->m);
        s_->cv.wait(l, [&] { return s_->loaded.load(std::memory_order_relaxed) >= end || s_->done; });
        have = s_->loaded.load(std::memory_order_acquire);
    }
    if (have <= off) return 0;
    const size_t got = static_cast<size_t>(std::min(end, have) - off);
    std::memcpy(dst, s_->data.get() + off, got);
    return got;
}

std::shared_ptr<FileBytes> FileBytes::open(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > static_cast<uintmax_t>(kMaxBytes)) return nullptr;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return nullptr;
    {
        std::lock_guard<std::mutex> l(g_cache_m);
        for (const auto& e : g_cache)
            if (e.path == path && e.size == static_cast<int64_t>(size) && e.mtime == mtime) return e.bytes;
    }
    auto f = std::make_shared<Reader>(path);
    if (!*f) return nullptr;
    auto s = std::make_shared<State>();
    s->size = static_cast<int64_t>(size);
    s->data.reset(new (std::nothrow) uint8_t[static_cast<size_t>(size)]);
    if (!s->data) return nullptr;
    s->name = platform::path_to_utf8(path.filename());
    std::thread([s, f] { load(s, f); }).detach();
    std::shared_ptr<FileBytes> fb(new FileBytes(s));
    std::lock_guard<std::mutex> l(g_cache_m);
    g_cache.push_front({path, s->size, mtime, fb});
    if (g_cache.size() > kCached) g_cache.pop_back();
    return fb;
}

SlowRead last_slow_read() {
    std::lock_guard<std::mutex> l(g_slow_m);
    return g_slow;
}

int64_t slow_read_count() { return g_slow_count.load(std::memory_order_acquire); }

}  // namespace daphne
