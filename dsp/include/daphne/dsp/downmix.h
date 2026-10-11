#pragma once
// Downmix to stereo (ITU-R BS.775 coefficients), run right after decoding when the source has more
// than two channels; a mono source is copied to both sides.
//
//   3.0 / 3.1 / 2.1   L = 0.7071 (FL + t),                     t = 0.7071 C + 0.5 LFE
//   quad / 4.0        L = 0.7071 (FL + 0.7071 S)
//   5.0 / 5.1         L = 0.5 (FL + t + 0.7071 S)
//   6.1 / 7.1         L = 0.408248 (FL + t + 0.7071 (S1 + S2))
//
// Channel roles come from the WAVEFORMATEXTENSIBLE mask (stream order = ascending mask bits); a
// missing mask, or one whose bit count does not match the channel count, is replaced by the default
// layout for the channel count. The formula is picked by which channels exist (centre / LFE, one or
// two surround pairs); absent channels count as 0. A back-centre channel is split equally (x 0.7071)
// into the back pair. Front-of-centre and height channels are not mixed. With no known layout the
// first two channels are kept.
//
// The whole mix is one 2 x N coefficient matrix built in prepare(); process() is a dot product per
// output channel.

#include <cstdint>
#include <vector>

#include "daphne/dsp/block.h"

namespace daphne::dsp {

class Downmix final : public Block {
public:
    // channel_mask: WAVEFORMATEXTENSIBLE dwChannelMask of the source (0 = unknown).
    explicit Downmix(uint32_t channel_mask) : mask_(channel_mask) {}

    void prepare(const Format& in) override;
    Format output_format() const override { return out_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override {}
    bool is_identity() const override { return in_.channels == 2; }

    // The layout assumed when the source gives no usable mask.
    static uint32_t default_mask(int channels);

    // Coefficient of source channel k in the left / right output (for tests and diagnostics).
    double coef_left(int k) const { return cl_[static_cast<size_t>(k)]; }
    double coef_right(int k) const { return cr_[static_cast<size_t>(k)]; }

private:
    enum class Kind { Copy, Mono, Matrix };

    uint32_t mask_;
    Format in_{}, out_{};
    Kind kind_ = Kind::Copy;
    std::vector<double> cl_, cr_;   // one coefficient per source channel
};

}  // namespace daphne::dsp
