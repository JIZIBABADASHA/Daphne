#pragma once
// Two chains joined without a click (N4): a rebuilt chain is warmed up on the recent input, aligned to
// the playing one by content delay and crossfaded in (raised cosine, 50 ms). Also the input history the
// warm-up needs, and the re-quantization of faded frames on integer devices (N21).

#include <cstdint>
#include <memory>
#include <vector>

#include "common.h"
#include "daphne/dsp/quantizer.h"
#include "daphne/engine/chain.h"
#include "daphne/engine/sample_format.h"

namespace daphne::playback {

// The last decoded source frames (about 3 s at 44.1 kHz), for a new chain's warm-up.
struct History {
    std::vector<double> buf;
    int cap = 1 << 17;          // frames
    int ch = 0;
    int64_t valid_from = 0;     // first input frame it holds (after a seek or a new track: from there)

    void reset(int channels) {
        ch = channels;
        buf.assign(static_cast<size_t>(cap) * ch, 0.0);
    }
    // n frames that are input frames at .. at + n - 1
    void store(const double* x, int n, int64_t at) {
        for (int i = 0; i < n; ++i) {
            const int64_t f = at + i;
            std::copy(x + static_cast<size_t>(i) * ch, x + static_cast<size_t>(i + 1) * ch,
                      buf.begin() + static_cast<std::ptrdiff_t>((f % cap) * ch));
        }
    }
};

// The crossfade and the fade-in / -out change the level after the chains' final quantizer: on an
// integer device the frame is quantized again, with TPDF dither at the device word length (N21), instead
// of being rounded plainly by the conversion. Frames at gain 0 or 1 are not touched (the callers' rule).
class FadeQuantizer {
public:
    // modulated: the dsd core (the modulator takes the level change)
    void requantize(double* frame, const DeviceFormat& dev, int dch, bool modulated) {
        if (modulated || is_float(dev.type) || is_dsd(dev.type)) return;   // the modulator / the device take it
        const int bits = effective_bits(dev);
        if (!q_ || q_->bits() != bits || q_->output_format().channels != dch) {
            q_ = std::make_unique<dsp::Quantizer>(bits);
            q_->prepare({dev.rate, dch, 1});
        }
        q_->process(frame, 1, frame);
    }

private:
    std::unique_ptr<dsp::Quantizer> q_;
};

class Crossfader {
public:
    Fifo fc, fn;                     // what the current / next chain produced, not mixed yet
    Fifo carry;                      // chain output to play before decoding more (after a fade or a gapless join)
    std::unique_ptr<DspChain> next;  // the chain fading in
    bool fading = false;
    int64_t pos = 0, len = 0;        // frames mixed, fade length
    int64_t drop_next = 0;           // next-chain frames before the aligned point, still to drop
    int delay_shift = 0;             // content delay next - current: the timeline moves by it at the end

    void set_channels(int dch) { fc.ch = fn.ch = carry.ch = dch; }
    void clear() { fc.clear(), fn.clear(), carry.clear(); }

    // the next chain's output, from the aligned point on
    void push_next(const double* x, int n) {
        const int drop = static_cast<int>(std::min<int64_t>(drop_next, n));
        drop_next -= drop;
        fn.push(x + static_cast<size_t>(drop) * fn.ch, n - drop);
    }

    // Starts the crossfade to nx: warm it up on the recent input (h, up to input frame in_total) and align
    // it to the current chain by content delay. abs_out: the current chain's next output frame; ratio:
    // output frames per input frame; fade: the crossfade's length in output frames.
    void begin(std::unique_ptr<DspChain> nx, const History& h, int64_t in_total, int64_t abs_out, int cur_delay, int ratio,
               int64_t fade);

    // Mixes what both chains have produced into out (room frames at most); returns frames.
    int mix(double* out, int room, FadeQuantizer& fq, const DeviceFormat& dev, bool modulated);

    // The fade is over: the next chain, to be made current; what it produced beyond the mixed part is
    // in carry. The caller moves the timeline by delay_shift.
    std::unique_ptr<DspChain> finish() {
        carry.clear();
        if (fn.frames() > 0) carry.push(fn.data(), fn.frames());
        fc.clear(), fn.clear();
        fading = false;
        return std::move(next);
    }
};

}  // namespace daphne::playback
