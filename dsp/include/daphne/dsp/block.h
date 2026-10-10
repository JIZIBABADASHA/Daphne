#pragma once
// Processing block contract.
//
// Data is float64, interleaved, ±1.0 = full scale.
// Threading: prepare / reset / process are called from one audio thread only.
// set_params may be called from any thread; it never blocks, and the new
// parameters take effect at the start of the next process() call.
// process() must not allocate, lock or throw. Blocks never quantize or clamp; quantization happens
// only in the final Quantizer stage.

#include <memory>

namespace daphne::dsp {

struct Format {
    int rate = 0;
    int channels = 0;
    int max_frames = 0;   // largest frame count process() will be given
};

class Block {
public:
    virtual ~Block() = default;

    // Allocate everything; process() must not allocate afterwards.
    virtual void prepare(const Format& in) = 0;
    // Output format (a resampler changes the rate).
    virtual Format output_format() const = 0;
    // Returns the number of output frames written to out (in == out allowed when
    // the block does not change the frame count).
    virtual int process(const double* in, int frames, double* out) = 0;
    virtual int latency_frames() const = 0;
    // End of stream: writes any output still held back (e.g. a partial block) and returns
    // its frame count. Blocks without internal buffering write nothing.
    virtual int flush(double* out) {
        (void)out;
        return 0;
    }
    // Clear internal state (seek, new track).
    virtual void reset() = 0;
    // True when output == input bit for bit with the current parameters.
    virtual bool is_identity() const = 0;
};

}  // namespace daphne::dsp
