#pragma once
// PCM -> DSD modulator: a 1-bit delta-sigma loop.
//
//   loop        CIFF structure (cascade of integrators, resonators on the integrator pairs,
//               feed-forward to the quantizer) with coefficients from an NTF design (Schreier's
//               synthesizeNTF, optimized zeros) and diagonal state scaling, so every state stays
//               within +-1 up to the maximum stable input (tools/sdm_design.py, sdm_tables.inc).
//               Orders 5, 8 and 9; the table for the DSD multiple (64 .. 1024) is picked.
//   input       gain 0.5 (full scale -> 50% modulation, the SACD reference level), a DC blocker
//               (3.5 Hz), and a clip at 0.95 x the maximum stable input (the chain's limiter keeps
//               the signal below it; the clip is protection)
//   clock       44100 x osr whatever the source; any input rate is interpolated to it with a
//               4-point (cubic) Lagrange interpolator (the chain feeds 705.6 / 768 kHz, already
//               upsampled by the resampling filter)
//   dither      high-pass TPDF (difference of consecutive uniform values), 0.01 at the quantizer
//               input, shaped by the NTF
//   protection  states clamp at +-1.5 (never reached within the stable range); a channel whose loop
//               stays clamped for 4096 bits is reset
//
// Output: DSD bytes, MSB first (oldest bit), interleaved L R (L only for mono). Stereo: the right
// channel runs on a worker thread (the channels are independent loops: the same bytes either way).

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace daphne::dsp {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)   // Ch is padded to whole cache lines on purpose
#endif
class DsdModulator {
public:
    // order 5 | 8 | 9 (others round to the nearest of them), osr 64 .. 1024.
    DsdModulator(int fs_in, int osr, int order);
    ~DsdModulator();
    DsdModulator(const DsdModulator&) = delete;
    DsdModulator& operator=(const DsdModulator&) = delete;

    int dsd_rate() const { return clock_; }
    int order() const { return n_; }
    double max_input() const { return umax_; }
    // Gain applied to the input (the SACD reference level).
    static constexpr double kGain = 0.5;
    // The maximum stable input (after the gain) of the loop for these settings.
    static double max_stable_input(int osr, int order);

    void process(const double* in, int frames, int channels, std::vector<uint8_t>& out);
    void reset();
    void set_threaded(bool on) { threaded_ = on; }

    // protection counters (normally 0), both channels
    uint64_t clamped_bits() const { return ch_[0].clamped + ch_[1].clamped; }
    uint64_t loop_resets() const { return ch_[0].resets + ch_[1].resets; }
    uint64_t input_clips() const { return ch_[0].input_clips + ch_[1].input_clips; }

private:
    // one channel's loop. Each on its own cache lines: the two channels run on different threads,
    // and a shared line (false sharing) made the threaded loop slower than one thread
    struct alignas(64) Ch {
        double x[15]{};
        double s_prev = 0, y_prev = 0;
        double ip[3]{};   // the last three input samples (oldest first), for the cubic interpolation
        double hp_prev = 0;
        double phase = 0;
        uint32_t rng = 0;
        uint8_t acc = 0;
        int nbits = 0;
        int clamp_run = 0;
        uint64_t clamped = 0, resets = 0, input_clips = 0;
    };
    template <int N>
    void run_channel(Ch& h, const double* in, int frames, int channels, int c, std::vector<uint8_t>& bytes);
    void run(Ch& h, const double* in, int frames, int channels, int c, std::vector<uint8_t>& bytes);
    void worker_loop();

    int clock_ = 0, n_ = 5;
    double dt_ = 0.0;
    double a_[15]{}, k_[15]{}, g_[7]{};
    double umax_ = 0.65, ulim_ = 0.6, dc_pole_ = 0.9995;
    // interpolation weights per bit when the bits per input sample are a whole power of two
    // (the phase then takes the same m values in every sample): w_[4 * k + i] for phase k / m
    int period_ = 0;
    std::vector<double> w_;
    Ch ch_[2];
    std::vector<uint8_t> bytes_[2];
    bool threaded_ = true;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    bool job_ = false, done_ = false, quit_ = false;
    const double* job_in_ = nullptr;
    int job_frames_ = 0, job_channels_ = 0;
    unsigned job_csr_ = 0;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace daphne::dsp
