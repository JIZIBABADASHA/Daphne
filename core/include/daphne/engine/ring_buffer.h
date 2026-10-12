#pragma once
#pragma warning(disable : 4324)  // alignas padding is intended
// Single-producer / single-consumer lock-free byte ring between the player and the device callback.
// The audio thread only reads; it never blocks, skips or rewinds.
//
// clear() may run while the device is running: the consumer brackets each callback's use of the ring
// with begin_read() / end_read(), and clear() waits until it is outside that bracket (at most one
// callback), so the indices are never reset under a read in progress. A consumer that finds a clear
// running leaves the ring alone for that callback (begin_read() returns false).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
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

    // Producer side (or with both sides stopped). Empties the ring; waits while the consumer is inside
    // begin_read() / end_read().
    void clear() {
        clearing_.store(true, std::memory_order_seq_cst);
        while (reading_.load(std::memory_order_seq_cst)) std::this_thread::yield();
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        clearing_.store(false, std::memory_order_release);
    }

    // Consumer side, around everything one callback does with the ring. begin_read() is false while a
    // clear() runs: leave the ring alone (and do not call end_read()). The seq_cst store-then-load on
    // both sides means at least one of them sees the other.
    bool begin_read() {
        reading_.store(true, std::memory_order_seq_cst);
        if (clearing_.load(std::memory_order_seq_cst)) {
            reading_.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }
    void end_read() { reading_.store(false, std::memory_order_release); }

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
    alignas(64) std::atomic<bool> reading_{false};
    std::atomic<bool> clearing_{false};
};

}  // namespace daphne
