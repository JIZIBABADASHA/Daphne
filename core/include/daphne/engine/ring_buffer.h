#pragma once
#pragma warning(disable : 4324)  // alignas padding is intended
// Single-producer / single-consumer lock-free byte ring between the player and the device callback.
// The audio thread only reads; it never blocks, skips or rewinds.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace daphne {

class ByteRing {
public:
    explicit ByteRing(size_t min_bytes = 1 << 16) {
        size_t cap = 1;
        while (cap < min_bytes) cap <<= 1;
        buf_.assign(cap, 0);
        mask_ = cap - 1;
    }

    size_t capacity() const { return buf_.size(); }
    size_t readable() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed); }
    size_t writable() const { return capacity() - (write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire)); }

    // Producer side. Writes up to n bytes; returns bytes written.
    size_t write(const void* src, size_t n) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        n = std::min(n, capacity() - (w - r));
        copy_in(w, static_cast<const uint8_t*>(src), n);
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Reads up to n bytes; returns bytes read.
    size_t read(void* dst, size_t n) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        copy_out(r, static_cast<uint8_t*>(dst), n);
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Copies up to n bytes without consuming them.
    size_t peek(void* dst, size_t n) const {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        copy_out(r, static_cast<uint8_t*>(dst), n);
        return n;
    }

    // Only when neither side is running (or the consumer is paused and not touching the ring).
    void clear() {
        read_.store(0);
        write_.store(0);
    }

private:
    void copy_in(size_t pos, const uint8_t* src, size_t n) {
        const size_t i = pos & mask_;
        const size_t first = std::min(n, capacity() - i);
        std::memcpy(buf_.data() + i, src, first);
        std::memcpy(buf_.data(), src + first, n - first);
    }
    void copy_out(size_t pos, uint8_t* dst, size_t n) const {
        const size_t i = pos & mask_;
        const size_t first = std::min(n, capacity() - i);
        std::memcpy(dst, buf_.data() + i, first);
        std::memcpy(dst + first, buf_.data(), n - first);
    }

    std::vector<uint8_t> buf_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

}  // namespace daphne
