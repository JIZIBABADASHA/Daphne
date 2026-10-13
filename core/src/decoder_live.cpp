// Live input decoder (live_source.h).
#include "daphne/engine/live_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace daphne {

namespace {

constexpr double kTarget = 0.30;     // seconds of buffered input aimed for
constexpr double kOverflow = 1.2;    // beyond this something stalled: restart near the writer
constexpr int kWaitMs = 20;          // running: how long to wait for the producer before a starve
constexpr double kFadeSeconds = 0.005;   // fade over a starve, a restart or the start

// The interpolator: Kaiser-windowed sinc, 64 taps (offsets -31 .. +32 around the read position), 512 phases with
// linear interpolation between neighbouring phases. Cut-off 0.45 fs, beta 10: flat to 19 kHz at 48 kHz (20 kHz
// -0.1 dB), spurious products about -115 dB at any read rate. 4-point Hermite left -28 dB at 10 kHz and -18 dB at
// 15 kHz: the read position moves through every fraction whatever the (tiny) rate correction.
constexpr int kTaps = 64, kHalf = kTaps / 2, kPhases = 512;
constexpr double kCutoff = 0.45, kBeta = 10.0;

std::atomic<uint8_t*> g_base{nullptr};
std::atomic<int> g_cap{0};
std::atomic<int> g_rate{48000};
std::atomic<int> g_readers{0};   // decoders inside the ring right now (set_live_bus waits for them to leave)

// stats of the open decoder (one live decoder at a time)
std::atomic<double> g_fill{0}, g_read{0}, g_adjust{0};
std::atomic<int64_t> g_resyncs{0}, g_starves{0};
std::atomic<int> g_open{0};

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 200 && term > 1e-17 * sum; ++k) {
        const double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
    }
    return sum;
}

// kPhases + 1 rows of kTaps coefficients: row p is the kernel at fraction p / kPhases, each row DC gain 1
const std::vector<double>& kernel() {
    static const std::vector<double> table = [] {
        std::vector<double> t(static_cast<size_t>(kPhases + 1) * kTaps);
        const double i0b = bessel_i0(kBeta), pi = 3.14159265358979323846;
        for (int p = 0; p <= kPhases; ++p) {
            double* row = t.data() + static_cast<size_t>(p) * kTaps;
            double sum = 0;
            for (int j = 0; j < kTaps; ++j) {
                const double x = (j - (kHalf - 1)) - static_cast<double>(p) / kPhases;   // tap position - fraction
                const double r = x / kHalf, wa = 1 - r * r;
                const double win = wa > 0 ? bessel_i0(kBeta * std::sqrt(wa)) / i0b : 0.0;
                const double a = 2 * pi * kCutoff * x;
                row[j] = 2 * kCutoff * (x == 0 ? 1.0 : std::sin(a) / a) * win;
                sum += row[j];
            }
            for (int j = 0; j < kTaps; ++j) row[j] /= sum;
        }
        return t;
    }();
    return table;
}

// a decoder inside the ring: set_live_bus(nullptr) waits until it has left (seq_cst on both sides)
struct RingUse {
    uint8_t* base = nullptr;
    RingUse() {
        g_readers.fetch_add(1, std::memory_order_seq_cst);
        base = g_base.load(std::memory_order_seq_cst);
    }
    ~RingUse() { g_readers.fetch_sub(1, std::memory_order_release); }
};

class LiveDecoder final : public Decoder {
public:
    ~LiveDecoder() override { close(); }

    bool open(const std::wstring&, std::string* error) override {
        if (!g_base.load() || g_cap.load() <= 0) {
            if (error) *error = "live input: the host has not shared its ring";
            return false;
        }
        info_ = {};
        info_.rate = g_rate.load();
        info_.channels = 2;
        info_.codec = "Live";
        info_.frames = -1;
        pos_ = 0;
        synced_ = running_ = jump_ = false;
        refill_ = true;
        gain_ = target_ = 0;
        gain_step_ = 1.0 / std::max(1.0, kFadeSeconds * info_.rate);
        reserve_ = static_cast<int>(std::ceil(kFadeSeconds * info_.rate)) + 2;
        kernel();   // built once, before the first read
        if (!open_) g_open++;
        open_ = true;
        g_resyncs = 0, g_starves = 0, g_read = 0;
        return true;
    }

    void close() override {
        if (open_) g_open--;
        open_ = false;
    }

    bool seek(int64_t frame) override {   // a stream: only the position counter moves
        pos_ = std::max<int64_t>(0, frame);
        return true;
    }

    int read(double* out, int frames) override {
        if (!open_ || frames <= 0) return 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        for (;;) {
            const int n = produce(out, frames, false);
            if (n > 0) return advance(n);
            // nothing yet (the player prefills its ring), or refilling after a starve or a restart: silence at once, as
            // much as the player asks for. It asks only when its ring has room, so the silence keeps that ring full;
            // waiting here instead (20 ms per 5 ms of silence) drained it, and once the audio came back the player
            // took its refill out of the jitter buffer, leaving it near empty and the next starve close
            if (!running_ || (refill_ && gain_ == 0)) return silence(out, frames);
            if (std::chrono::steady_clock::now() >= deadline) {
                if (gain_ > 0) {
                    // the producer fell behind: fade out on the frames held back for it, then wait for the
                    // buffer to fill again (silence meanwhile, see above)
                    g_starves++;
                    target_ = 0;
                    const int f = produce(out, frames, true);
                    refill_ = true;
                    if (f > 0) return advance(f);
                    gain_ = 0;
                }
                return silence(out, std::min(frames, std::max(1, info_.rate / 200)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

private:
    int advance(int n) {
        pos_ += n;
        g_read = static_cast<double>(pos_) / info_.rate;
        return n;
    }

    int silence(double* out, int n) {
        std::memset(out, 0, static_cast<size_t>(n) * 2 * sizeof(double));
        return advance(n);
    }

    // Frames interpolated from the ring, or 0 when there is nothing to read now. Normally the last reserve_ frames
    // are held back so that a starve can still fade out on them (use_reserve: fade out on them now). A fade-out
    // ends the call once it reaches silence.
    int produce(double* out, int frames, bool use_reserve) {
        RingUse use;
        uint8_t* base = use.base;
        const uint32_t cap = static_cast<uint32_t>(g_cap.load());
        if (!base || cap == 0) return 0;
        const uint32_t w = *reinterpret_cast<volatile const uint32_t*>(base);
        std::atomic_thread_fence(std::memory_order_acquire);   // the frames before the index
        const float* ring = reinterpret_cast<const float*>(base + 16);
        const int rate = g_rate.load();

        // signed: the index wraps at 2^32 frames (about 25 h at 48 kHz) without counting as going back
        const int32_t ahead = static_cast<int32_t>(w - read_);
        if (!synced_ || ahead < 0 || static_cast<uint32_t>(ahead) > cap - 4096) {
            // first read, page reloaded, or the writer lapped us (what we held is overwritten: no fade-out
            // possible): restart close to the writer and fade in once the buffer has filled
            if (synced_) g_resyncs++;
            read_ = w, frac_ = 0, adj_ = 0, synced_ = true, refill_ = true, jump_ = false;
            gain_ = target_ = 0;
            return 0;
        }
        const int avail = ahead;
        const double fill = static_cast<double>(avail) / rate;
        g_fill = fill;
        if (refill_ && !use_reserve) {
            if (fill < kTarget) return 0;
            refill_ = false, running_ = true, target_ = 1;   // fade in
        }
        if (fill > kOverflow && !jump_) {
            // something stalled (the player was paused): fade out here, then restart near the writer
            g_resyncs++;
            jump_ = true, target_ = 0;
        }

        // proportional control of the buffer level through a tiny rate change
        if (!jump_ && !use_reserve) {
            const double want = std::clamp((fill - kTarget) * 0.02, -0.005, 0.005);
            adj_ += (want - adj_) * 0.02;
            g_adjust = adj_;
        }
        const double step = static_cast<double>(rate) / info_.rate * (1 + adj_);

        const std::vector<double>& table = kernel();
        const int limit = avail - kHalf - (use_reserve ? 0 : reserve_);   // the last tap must be written
        double c[kTaps];
        int produced = 0;
        while (produced < frames) {
            const int i0 = static_cast<int>(frac_);
            if (i0 >= limit) break;
            const double pf = (frac_ - i0) * kPhases;
            const int p = static_cast<int>(pf);
            const double a = pf - p;
            const double* r0 = table.data() + static_cast<size_t>(p) * kTaps;
            const double* r1 = r0 + kTaps;
            for (int j = 0; j < kTaps; ++j) c[j] = r0[j] + (r1[j] - r0[j]) * a;
            const uint32_t b = read_ + static_cast<uint32_t>(i0) - static_cast<uint32_t>(kHalf - 1);
            double l = 0, rr = 0;
            uint32_t k = b % cap;   // the writer's mapping: frame index modulo cap
            for (int j = 0; j < kTaps; ++j) {
                if (b + static_cast<uint32_t>(j) == 0) k = 0;   // the index wraps at 2^32: so does its position
                l += c[j] * ring[2 * static_cast<size_t>(k)];
                rr += c[j] * ring[2 * static_cast<size_t>(k) + 1];
                if (++k == cap) k = 0;
            }
            if (gain_ < target_) gain_ = std::min(target_, gain_ + gain_step_);
            else if (gain_ > target_) gain_ = std::max(target_, gain_ - gain_step_);
            out[2 * produced] = l * gain_;
            out[2 * produced + 1] = rr * gain_;
            ++produced;
            frac_ += step;
            if (gain_ == 0 && target_ == 0) break;   // faded out
        }
        const int whole = static_cast<int>(frac_);
        read_ += static_cast<uint32_t>(whole);
        frac_ -= whole;
        if (jump_ && gain_ == 0) {   // faded out after the stall: restart near the writer, fading in
            read_ = w - static_cast<uint32_t>(kTarget * rate), frac_ = 0, adj_ = 0;
            jump_ = false, target_ = 1;
        }
        return produced;
    }

    bool open_ = false;
    uint32_t read_ = 0;   // integer read position in the ring (frames, wraps like the writer's)
    double frac_ = 0;     // between read_ and read_ + 1
    bool synced_ = false;
    bool running_ = false;   // audio has been produced since open (before that, silence is returned at once)
    bool refill_ = true;     // wait for the buffer to reach kTarget (the start, after a starve or a restart)
    bool jump_ = false;      // fading out to restart near the writer
    double adj_ = 0;         // smoothed rate correction
    double gain_ = 0, target_ = 0, gain_step_ = 1;
    int reserve_ = 0;        // frames held back for a starve's fade-out
};

}  // namespace

void set_live_bus(void* base, int cap_frames, int rate) {
    if (rate > 8000) g_rate = rate;
    if (!base) {
        // detach: no decoder may still be reading the old memory when this returns
        g_base.store(nullptr, std::memory_order_seq_cst);
        while (g_readers.load(std::memory_order_seq_cst) > 0) std::this_thread::yield();
        g_cap = 0;
        return;
    }
    g_cap = cap_frames;
    g_base.store(static_cast<uint8_t*>(base), std::memory_order_release);
}

void set_live_rate(int rate) {
    if (rate > 8000) g_rate = rate;
}

LiveStats live_stats() {
    LiveStats s;
    s.fill = g_fill.load();
    s.read = g_read.load();
    s.resyncs = g_resyncs.load();
    s.starves = g_starves.load();
    s.adjust = g_adjust.load();
    s.active = g_open.load() > 0;
    return s;
}

std::unique_ptr<Decoder> make_live_decoder() { return std::make_unique<LiveDecoder>(); }

}  // namespace daphne
