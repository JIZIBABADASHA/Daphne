#include "daphne/engine/ring_feeder.h"

#include "daphne/engine/audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace daphne {

namespace {

constexpr double kPi = 3.14159265358979323846;

uint64_t bits_of(double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    return b;
}
double double_of(uint64_t b) {
    double v;
    std::memcpy(&v, &b, 8);
    return v;
}
void raise_peak(std::atomic<uint64_t>& a, double v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (double_of(cur) < v && !a.compare_exchange_weak(cur, bits_of(v), std::memory_order_relaxed)) {
    }
}

}  // namespace

void RingFeeder::configure(const DeviceFormat& f) {
    f_ = f;
    ss_ = std::max(1, f.container_bits / 8);
    block_ = std::max(1, f.block_align());
    pcm_ = !is_dsd(f.type);
    ramp_ = std::max(1, f.frame_rate() / 50);   // 20 ms
    reset();
}

void RingFeeder::reset() {
    state_ = want_pause_.load() ? kPaused : kPlaying;
    pos_ = 0;
    last_marker_ = 0;
    idle_frames_ = 0;
    peak_l_ = bits_of(0.0);
    peak_r_ = bits_of(0.0);
}

double RingFeeder::read_sample(const uint8_t* p) const {
    switch (f_.type) {
        case SampleType::Float32: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        case SampleType::Float64: {
            double v;
            std::memcpy(&v, p, 8);
            return v;
        }
        case SampleType::Int16: return static_cast<int16_t>(p[0] | p[1] << 8) / 32768.0;
        case SampleType::Packed24: return static_cast<int32_t>(static_cast<uint32_t>(p[0] << 8 | p[1] << 16 | p[2] << 24)) / 2147483648.0;
        case SampleType::Int32Low: {
            int32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<int32_t>(static_cast<uint32_t>(v) << (32 - f_.valid_bits)) / 2147483648.0;
        }
        default: {   // Int32 / Int24In32: left-aligned
            int32_t v;
            std::memcpy(&v, p, 4);
            return v / 2147483648.0;
        }
    }
}

void RingFeeder::write_sample(uint8_t* p, double v) const {
    switch (f_.type) {
        case SampleType::Float32: {
            const float x = static_cast<float>(v);
            std::memcpy(p, &x, 4);
            return;
        }
        case SampleType::Float64: std::memcpy(p, &v, 8); return;
        case SampleType::Int16: {
            const int32_t x = static_cast<int32_t>(std::lrint(std::clamp(v * 32768.0, -32768.0, 32767.0)));
            p[0] = static_cast<uint8_t>(x), p[1] = static_cast<uint8_t>(x >> 8);
            return;
        }
        case SampleType::Packed24: {
            const int32_t x = static_cast<int32_t>(std::lrint(std::clamp(v * 8388608.0, -8388608.0, 8388607.0)));
            p[0] = static_cast<uint8_t>(x), p[1] = static_cast<uint8_t>(x >> 8), p[2] = static_cast<uint8_t>(x >> 16);
            return;
        }
        case SampleType::Int32Low: {
            const double s = std::ldexp(1.0, f_.valid_bits - 1);
            const int32_t x = static_cast<int32_t>(std::llrint(std::clamp(v * s, -s, s - 1)));
            std::memcpy(p, &x, 4);
            return;
        }
        default: {
            const int32_t valid = f_.type == SampleType::Int24In32 ? 24 : 32;
            const double s = std::ldexp(1.0, valid - 1);
            int64_t x = std::llrint(std::clamp(v * s, -s, s - 1));
            const int32_t y = static_cast<int32_t>(x << (32 - valid));
            std::memcpy(p, &y, 4);
            return;
        }
    }
}

// The DoP marker sits in the top byte of the 24-bit word.
uint8_t RingFeeder::marker_of(const uint8_t* frame) const {
    switch (f_.type) {
        case SampleType::Packed24: return frame[2];
        case SampleType::Int32Low: return frame[2];
        default: return frame[3];
    }
}

void RingFeeder::put_dop_idle(uint8_t* frame) {
    const uint8_t m = last_marker_ == 0x05 ? 0xFA : 0x05;
    const uint32_t w = static_cast<uint32_t>(m) << 16 | 0x6969;
    for (int c = 0; c < f_.channels; ++c) {
        uint8_t* p = frame + static_cast<size_t>(c) * ss_;
        if (f_.type == SampleType::Packed24) {
            p[0] = static_cast<uint8_t>(w), p[1] = static_cast<uint8_t>(w >> 8), p[2] = static_cast<uint8_t>(w >> 16);
        } else {
            const uint32_t v = f_.type == SampleType::Int32Low ? w : w << 8;
            std::memcpy(p, &v, 4);
        }
    }
    last_marker_ = m;
    ++idle_frames_;
}

bool RingFeeder::fill(uint8_t* dst, int frames, ByteRing* ring, bool eof, OutputStats& stats) {
    const bool want = want_pause_.load(std::memory_order_acquire);
    const bool dop = dop_.load(std::memory_order_acquire);
    int st = state_.load(std::memory_order_relaxed);
    if (want && (st == kPlaying || st == kFadingIn)) st = pcm_ && !dop ? kFadingOut : kPaused, pos_ = 0, idle_frames_ = 0;
    if (!want && (st == kPaused || st == kFadingOut)) {
        // DoP: the next real frame must not repeat the last silence frame's marker (after a seek the
        // ring holds a new stretch of the stream, so look at it rather than counting)
        if (dop && frames > 0 && ring->readable() >= static_cast<size_t>(block_)) {
            uint8_t first[64];
            ring->peek(first, std::min<size_t>(block_, sizeof first));
            if (marker_of(first) == last_marker_) {
                put_dop_idle(dst);
                dst += block_;
                --frames;
            }
        }
        st = pcm_ && !dop ? kFadingIn : kPlaying;
        pos_ = 0;
    }

    bool drained = false;
    if (st == kPaused) {
        if (!pcm_) std::memset(dst, 0x69, static_cast<size_t>(frames) * block_);
        else if (dop)
            for (int i = 0; i < frames; ++i) put_dop_idle(dst + static_cast<size_t>(i) * block_);
        else std::memset(dst, 0, static_cast<size_t>(frames) * block_);
        state_.store(st, std::memory_order_release);
        return false;
    }

    const size_t bytes = static_cast<size_t>(frames) * block_;
    size_t take = bytes;
    if (st == kFadingOut) take = std::min<size_t>(bytes, static_cast<size_t>(ramp_ - pos_) * block_);
    // whole frames only: the producer may have written part of a frame so far
    const size_t got = ring->read(dst, std::min(take / block_, ring->readable() / block_) * block_);
    const int n = static_cast<int>(got / block_);
    if (dop && n > 0) last_marker_ = marker_of(dst + static_cast<size_t>(n - 1) * block_);
    if (got < take) {
        // DoP running short mid-stream: DoP silence keeps the DAC in DSD mode. At the end (eof) the
        // stream closes with PCM silence on purpose (the DAC leaves DSD mode), so zeros stay there
        if (pcm_ && dop && !eof)
            for (size_t i = got / block_; i < take / block_; ++i) put_dop_idle(dst + i * block_);
        else std::memset(dst + got, pcm_ ? 0 : 0x69, take - got);
        if (!eof) stats.underruns++;
        else drained = true;
    }
    if (take < bytes) std::memset(dst + take, 0, bytes - take);   // after the fade-out: silence

    if (pcm_ && !dop) {
        double pl = 0, pr = 0;
        for (int i = 0; i < n; ++i) {
            uint8_t* fr = dst + static_cast<size_t>(i) * block_;
            double g = 1.0;
            if (st == kFadingIn || st == kFadingOut) {
                const double t = std::min(1.0, (pos_ + i + 0.5) / ramp_);
                g = st == kFadingIn ? 0.5 - 0.5 * std::cos(kPi * t) : 0.5 + 0.5 * std::cos(kPi * t);
            }
            for (int c = 0; c < f_.channels; ++c) {
                uint8_t* p = fr + static_cast<size_t>(c) * ss_;
                double v = read_sample(p);
                if (g != 1.0) {
                    v *= g;
                    write_sample(p, v);
                }
                if (c == 0) pl = std::max(pl, std::abs(v));
                else if (c == 1) pr = std::max(pr, std::abs(v));
            }
        }
        raise_peak(peak_l_, pl);
        raise_peak(peak_r_, f_.channels > 1 ? pr : pl);
        if (st == kFadingIn || st == kFadingOut) {
            pos_ += n;
            if (st == kFadingOut && pos_ >= ramp_) st = kPaused;
            if (st == kFadingIn && pos_ >= ramp_) st = kPlaying;
        }
    }
    state_.store(st, std::memory_order_release);
    return drained;
}

void RingFeeder::meter(double& l, double& r) {
    l = double_of(peak_l_.exchange(bits_of(0.0)));
    r = double_of(peak_r_.exchange(bits_of(0.0)));
}

}  // namespace daphne
