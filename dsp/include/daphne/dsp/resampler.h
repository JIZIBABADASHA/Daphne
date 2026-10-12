#pragma once
// Integer-ratio polyphase resampler.
//
// The L = taps * ratio coefficient filter is split into `ratio` phases of `taps` coefficients;
// every phase is convolved at the input rate by uniformly partitioned overlap-save FFT
// convolution (block B, FFT 2B) and the phase outputs are interleaved. Two channels share one
// complex FFT (left = real, right = imaginary).
//
// - output sample j corresponds to input time j / ratio (no block latency is inserted; output
//   appears a block at a time)
// - every phase is normalized to DC gain 1, so a constant input gives the same constant out of
//   every phase
// - flush() outputs the buffered input at the end of a stream: N input frames give exactly
//   N * ratio output frames
// - a unit impulse at ratio 1 is a bypass: the samples are copied (an FFT round trip would change
//   their last bit)
// - the output is never clamped: intersample peaks above 1.0 reach the volume and the final
//   quantizer intact

#include <complex>
#include <memory>
#include <vector>

#include "daphne/dsp/block.h"

namespace daphne::dsp {

class Fft;
class WorkerPool;
namespace ddc {
class Fft;
struct Planes;
}  // namespace ddc

// Partition block size (input samples) by filter length L = taps * ratio.
int resampler_block_size(long long L);

class Resampler : public Block {
public:
    // h: designed coefficients (DC gain about 1), length taps * ratio.
    // h_lo: the double-double low parts of h. Given, the whole convolution runs in double-double
    // (normalization, kernel spectra, FFTs and the multiply-accumulate, AVX2 + FMA3 when the CPU
    // has them); each output is rounded to double once at the end. The phases of a block then run
    // on several cores (design_threads(), at most one per phase); the result does not depend on
    // the thread count.
    Resampler(std::vector<double> h, int ratio, int block, const std::vector<double>* h_lo = nullptr);
    ~Resampler() override;

    void prepare(const Format& in) override;
    Format output_format() const override;
    // out must hold max_output_frames(frames) * channels values.
    int process(const double* in, int frames, double* out) override;
    // End of stream: outputs the buffered input (pending * ratio frames) and resets.
    int flush(double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override;

    int ratio() const { return ratio_; }
    bool double_double() const { return dd_; }
    int block() const { return block_; }
    // Content delay in output frames: the position of the kernel's largest coefficient (found once,
    // when the kernel is built: the player asks for it on every loop pass).
    int peak_index() const { return peak_; }
    // Upper bound of the frames process() can return for `frames` input frames.
    int max_output_frames(int frames) const { return (frames + block_) * ratio_; }

private:
    void run_block(double* out, int frames_out);
    void run_block_dd(double* out, int frames_out);
    void build_dd(const std::vector<double>& lo);
    int find_peak() const;

    int ratio_, block_, taps_, parts_;
    int peak_ = 0;                                                // peak_index()
    bool bypass_ = false;                                          // is_identity(): process() copies
    std::vector<double> kernel_;                                   // scaled coefficients, L
    std::vector<std::vector<std::complex<double>>> spectra_;       // [phase * parts + j], FFT 2B
    std::unique_ptr<Fft> fft_;
    Format in_{};
    int pending_ = 0;                                             // input frames in the current block
    std::vector<std::vector<double>> cur_, prev_;                 // per channel, B samples
    std::vector<std::vector<std::complex<double>>> fdl_;          // per pair: parts spectra ring
    int fdl_pos_ = 0;
    std::vector<std::complex<double>> work_, acc_;
    // double-double path
    bool dd_ = false;
    std::unique_ptr<ddc::Fft> dfft_;
    std::vector<ddc::Planes> dspectra_;                           // [phase * parts + j], n values
    std::vector<ddc::Planes> dfdl_;                               // per pair: parts * n values
    std::unique_ptr<ddc::Planes> dwork_;
    std::vector<ddc::Planes> daccs_;                              // one accumulator per phase
    std::unique_ptr<WorkerPool> pool_;                            // phases run in parallel
};

}  // namespace daphne::dsp
