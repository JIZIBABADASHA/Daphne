#pragma once
// A track's file read into memory by a background thread (read ahead), so the decoders read memory and a slow
// disk cannot starve the ring. A library on a USB hard disk that stopped answering for ~3 s made the device play
// gaps: the decoder read the file on the playback thread and the ring holds only 0.5 s without a filter.
//
// The decoder waits only for bytes the loader has not reached yet: the first read of a file, or a seek past what
// is loaded. The loader reads at the disk's speed, far ahead of playback, and the next track is opened (and read)
// while the current one plays. Files larger than FileBytes::kMaxBytes are read directly by the decoder, as before.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace daphne {

class FileBytes {
public:
    static constexpr int64_t kMaxBytes = int64_t{1} << 30;

    // The file, being read into memory. Shared: opening the same unchanged file again (the FFmpeg retry, a seek
    // back to the top, the prepared next track) gets the bytes already read. nullptr when the file cannot be
    // opened, is empty, or is larger than kMaxBytes.
    static std::shared_ptr<FileBytes> open(const std::filesystem::path& path);

    ~FileBytes();   // stops the loader (it is not waited for: a stalled disk must not stall the caller)
    FileBytes(const FileBytes&) = delete;
    FileBytes& operator=(const FileBytes&) = delete;

    int64_t size() const;
    // Up to n bytes at off, waiting for the loader when it has not got there yet. Fewer at the end of the file,
    // 0 past it or past what a failed read left.
    size_t read(int64_t off, void* dst, size_t n);

    struct State;

private:
    explicit FileBytes(std::shared_ptr<State> s);
    std::shared_ptr<State> s_;
};

// A read position over FileBytes, for the decoders' read / seek / tell callbacks.
class FileCursor {
public:
    explicit operator bool() const { return f_ != nullptr; }
    void reset(std::shared_ptr<FileBytes> f) {
        f_ = std::move(f);
        pos_ = 0;
    }
    int64_t size() const { return f_ ? f_->size() : 0; }
    int64_t tell() const { return pos_; }
    size_t read(void* dst, size_t n) {
        const size_t got = f_ ? f_->read(pos_, dst, n) : 0;
        pos_ += static_cast<int64_t>(got);
        return got;
    }
    // whence: 0 from the start, 1 from the current position, 2 from the end (SEEK_SET / SEEK_CUR / SEEK_END)
    bool seek(int64_t off, int whence) {
        const int64_t base = whence == 1 ? pos_ : whence == 2 ? size() : 0;
        if (!f_ || base + off < 0 || base + off > size()) return false;
        pos_ = base + off;
        return true;
    }

private:
    std::shared_ptr<FileBytes> f_;
    int64_t pos_ = 0;
};

// The last disk read of the read ahead that took 500 ms or more, for the log: count goes up with each one.
// Playback does not wait for these unless the decoder caught up with the loader.
struct SlowRead {
    int64_t count = 0;
    int64_t ms = 0;
    std::string file;   // UTF-8 file name
};
SlowRead last_slow_read();
int64_t slow_read_count();   // last_slow_read().count, without the lock (polled by the playback loop)

}  // namespace daphne
