#include "daphne/dsp/modulator.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace daphne::dsp {

namespace {

struct SdmTable {
    int order, osr;
    double h_inf, umax;
    double a[15];   // feed-forward to the quantizer (scaled states)
    double k[15];   // input gain of state i (state 0: u - v)
    double g[7];    // resonator feedback into the pair head
};

const SdmTable kTables[] = {
#include "sdm_tables.inc"
};

constexpr double kClamp = 1.5;        // states peak at 1 for inputs up to umax
constexpr int kClampRunReset = 4096;  // bits in a row with a clamped state -> unstable, reset
constexpr double kDither = 0.01;      // at the quantizer input, shaped by the NTF
constexpr double kPi = 3.14159265358979323846;

inline uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

const SdmTable* table_for(int osr_in, int order) {
    const int run = order >= 8 ? 8 : 5;
    int osr = 64;
    while (osr < 1024 && osr * 2 <= osr_in) osr *= 2;
    const SdmTable* t = &kTables[0];
    for (const auto& e : kTables)
        if (e.order == run && e.osr == osr) t = &e;
    return t;
}

// 4-point Lagrange weights for the point t between the two middle samples (t in [0, 1))
inline void lagrange(double t, double* w) {
    w[0] = -t * (t - 1.0) * (t - 2.0) * (1.0 / 6.0);
    w[1] = (t + 1.0) * (t - 1.0) * (t - 2.0) * 0.5;
    w[2] = -(t + 1.0) * t * (t - 2.0) * 0.5;
    w[3] = (t + 1.0) * t * (t - 1.0) * (1.0 / 6.0);
}

}  // namespace

double DsdModulator::max_stable_input(int osr, int order) { return table_for(osr, order)->umax; }

DsdModulator::DsdModulator(int fs_in, int osr, int order) {
    const SdmTable* t = table_for(osr, order);
    n_ = t->order;
    std::copy(t->a, t->a + 15, a_);
    std::copy(t->k, t->k + 15, k_);
    std::copy(t->g, t->g + 7, g_);
    umax_ = t->umax;
    ulim_ = 0.95 * umax_;
    clock_ = 44100 * osr;
    dt_ = static_cast<double>(fs_in) / clock_;
    // DC blocker: pole 1 - 0.0005 at 44.1 kHz (3.5 Hz), the same corner at any input rate
    dc_pole_ = std::exp(-2.0 * kPi * (0.0005 * 44100.0 / (2.0 * kPi)) / fs_in);
    // whole power-of-two bits per input sample (705.6 kHz into DSD128: 8): the phase steps through
    // k / m exactly (dt is a power of two), so the weights come from a table with the same bits
    if (clock_ % fs_in == 0) {
        const int m = clock_ / fs_in;
        if (m >= 1 && m <= 4096 && (m & (m - 1)) == 0) {
            period_ = m;
            w_.resize(static_cast<size_t>(4) * m);
            double phase = 0.0;
            for (int k = 0; k < m; ++k, phase += dt_) lagrange(phase, &w_[static_cast<size_t>(4) * k]);
        }
    }
    ch_[0].rng = 0x2545f491u;   // independent dither per channel
    ch_[1].rng = 0x9e3779b9u;
    reset();
}

DsdModulator::~DsdModulator() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> l(m_);
            quit_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
}

void DsdModulator::reset() {
    for (Ch& h : ch_) {
        for (double& v : h.x) v = 0.0;
        h.s_prev = h.y_prev = h.hp_prev = h.ip[0] = h.ip[1] = h.ip[2] = 0.0;
        h.phase = 0.0;
        h.acc = 0;
        h.nbits = 0;
        h.clamp_run = 0;
    }
}

// One channel through the gain, DC blocker, input clip, interpolation and the loop; whole DSD bytes
// are appended to `bytes` (the partial byte stays in the channel). Both channels see the same frames
// and phase steps, so they emit the same number of bytes. N: the loop order (a compile-time loop
// count, so the state updates unroll).
template <int N>
void DsdModulator::run_channel(Ch& h, const double* in, int frames, int channels, int c, std::vector<uint8_t>& bytes) {
    constexpr int odd = N % 2;
    const int col = c < channels ? c : 0;
    double phase = h.phase;
    double x[N];
    for (int i = 0; i < N; ++i) x[i] = h.x[i];
    double a[N], k[N], g[N / 2 + 1];
    for (int i = 0; i < N; ++i) a[i] = a_[i], k[i] = k_[i];
    for (int i = 0; i <= N / 2; ++i) g[i] = g_[i];
    const double* wt = period_ ? w_.data() : nullptr;
    int slot = period_ ? static_cast<int>(std::lround(phase / dt_)) : 0;   // phase k / m when tabulated
    uint32_t rng = h.rng;
    double hp_prev = h.hp_prev;
    uint8_t acc = h.acc;
    int nbits = h.nbits, clamp_run = h.clamp_run;
    for (int f = 0; f < frames; ++f) {
        const double s = in[static_cast<size_t>(f) * channels + col] * kGain;
        const double d = (s - h.s_prev) + dc_pole_ * h.y_prev;
        h.s_prev = s;
        h.y_prev = d;
        double y = d;
        if (y > ulim_ || y < -ulim_) {   // beyond the stable range: clip the input, not the loop
            y = y > 0 ? ulim_ : -ulim_;
            ++h.input_clips;
        }
        // between the two middle points of ip[0..2], y (one sample more delay than linear)
        while (phase < 1.0) {
            double wl[4];
            const double* w = wl;
            if (wt) w = wt + static_cast<size_t>(4) * slot++;
            else lagrange(phase, wl);
            const double u = w[0] * h.ip[0] + w[1] * h.ip[1] + w[2] * h.ip[2] + w[3] * y;
            const double r1 = xorshift(rng) * (1.0 / 4294967296.0);
            const double dq = kDither * (r1 - hp_prev);   // high-pass TPDF
            hp_prev = r1;
            double q = u + dq;
            for (int i = 0; i < N; ++i) q += a[i] * x[i];
            const bool bit = q >= 0.0;
            const double e = u - (bit ? 1.0 : -1.0);
            double nx[N];
            nx[0] = x[0] + k[0] * e;
            for (int i = 1; i < N; ++i) nx[i] = x[i] + k[i] * x[i - 1];
            for (int p = 0, kk = odd; kk + 1 < N; ++p, kk += 2) nx[kk] -= g[p] * x[kk + 1];
            bool clamped = false;
            for (int i = 0; i < N; ++i) {
                double v = nx[i];
                if (v > kClamp) v = kClamp, clamped = true;
                else if (v < -kClamp) v = -kClamp, clamped = true;
                x[i] = v;
            }
            if (clamped) {
                ++h.clamped;
                if (++clamp_run >= kClampRunReset) {   // this channel's loop is unstable: reset it
                    for (double& v : x) v = 0.0;
                    clamp_run = 0;
                    ++h.resets;
                }
            } else {
                clamp_run = 0;
            }
            acc = static_cast<uint8_t>((acc << 1) | (bit ? 1 : 0));
            if (++nbits == 8) {
                bytes.push_back(acc);
                acc = 0;
                nbits = 0;
            }
            phase += dt_;
        }
        phase -= 1.0;
        slot = 0;
        h.ip[0] = h.ip[1], h.ip[1] = h.ip[2], h.ip[2] = y;
    }
    for (int i = 0; i < N; ++i) h.x[i] = x[i];
    h.phase = phase;
    h.rng = rng;
    h.hp_prev = hp_prev;
    h.acc = acc;
    h.nbits = nbits;
    h.clamp_run = clamp_run;
}

void DsdModulator::run(Ch& h, const double* in, int frames, int channels, int c, std::vector<uint8_t>& bytes) {
    switch (n_) {
        case 8: run_channel<8>(h, in, frames, channels, c, bytes); break;
        default: run_channel<5>(h, in, frames, channels, c, bytes); break;
    }
}

void DsdModulator::worker_loop() {
#ifdef _WIN32
    // the caller is the player's processing thread (MMCSS "Audio"): the right channel runs in the same class
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Audio", &task);
#endif
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
        cv_.wait(l, [this] { return quit_ || job_; });
        if (quit_) break;
        l.unlock();
#if defined(_M_X64) || defined(__x86_64__)
        _mm_setcsr(job_csr_);   // the caller's FTZ / DAZ (denormals flushed as in its thread)
#endif
        run(ch_[1], job_in_, job_frames_, job_channels_, 1, bytes_[1]);
        l.lock();
        job_ = false;
        done_ = true;
        cv_.notify_all();
    }
#ifdef _WIN32
    if (mm) AvRevertMmThreadCharacteristics(mm);
#endif
}

void DsdModulator::process(const double* in, int frames, int channels, std::vector<uint8_t>& out) {
    if (frames <= 0 || channels <= 0) return;
    bytes_[0].clear();
    bytes_[1].clear();
    if (channels == 1) {   // mono: the left channel only
        run(ch_[0], in, frames, channels, 0, bytes_[0]);
        out.insert(out.end(), bytes_[0].begin(), bytes_[0].end());
        return;
    }
    if (threaded_) {
        if (!worker_.joinable()) worker_ = std::thread([this] { worker_loop(); });
        {
            std::lock_guard<std::mutex> l(m_);
            job_in_ = in, job_frames_ = frames, job_channels_ = channels;
#if defined(_M_X64) || defined(__x86_64__)
            job_csr_ = _mm_getcsr();
#endif
            done_ = false;
            job_ = true;
        }
        cv_.notify_all();
        run(ch_[0], in, frames, channels, 0, bytes_[0]);
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return done_; });
    } else {
        run(ch_[0], in, frames, channels, 0, bytes_[0]);
        run(ch_[1], in, frames, channels, 1, bytes_[1]);
    }
    const size_t nb = bytes_[0].size();   // the same for both channels
    const size_t at = out.size();
    out.resize(at + 2 * nb);
    for (size_t i = 0; i < nb; ++i) out[at + 2 * i] = bytes_[0][i], out[at + 2 * i + 1] = bytes_[1][i];
}

}  // namespace daphne::dsp
