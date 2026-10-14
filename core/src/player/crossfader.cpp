#include "crossfader.h"

#include <algorithm>
#include <cmath>

namespace daphne::playback {

void Crossfader::begin(std::unique_ptr<DspChain> nx, const History& h, int64_t in_total, int64_t abs_out, int cur_delay, int ratio,
                       int64_t fade) {
    const int ch = h.ch, dch = fn.ch;
    const int64_t have = std::min<int64_t>(in_total - h.valid_from, h.cap);
    const int64_t start = in_total - have;
    fn.clear();
    std::vector<double> in(static_cast<size_t>(kChunk) * ch);
    std::vector<double> o(static_cast<size_t>(nx->max_output_frames()) * dch);
    const int dc = cur_delay, dn = nx->content_delay();
    const int64_t target = abs_out - dc + dn;   // next-chain index of the content the current chain plays next
    drop_next = target - start * ratio;
    if (drop_next < 0) {
        fn.push_zeros(static_cast<int>(-drop_next));
        drop_next = 0;
    }
    for (int64_t f = start; f < in_total;) {
        const int n = static_cast<int>(std::min<int64_t>(kChunk, in_total - f));
        for (int i = 0; i < n; ++i) {
            const int64_t k = (f + i) % h.cap;
            std::copy(h.buf.begin() + static_cast<std::ptrdiff_t>(k * ch), h.buf.begin() + static_cast<std::ptrdiff_t>((k + 1) * ch),
                      in.begin() + static_cast<std::ptrdiff_t>(i) * ch);
        }
        const int got = nx->process(in.data(), n, o.data());
        push_next(o.data(), got);
        f += n;
    }
    // frames of the current chain still queued for output count as already played
    fc.clear();
    if (carry.frames() > 0) {
        fc.push(carry.data(), carry.frames());
        carry.clear();
    }
    delay_shift = dn - dc;
    next = std::move(nx);
    len = fade;
    pos = 0;
    fading = true;
}

int Crossfader::mix(double* out, int room, FadeQuantizer& fq, const DeviceFormat& dev, bool modulated) {
    const int dch = fc.ch;
    const int n = std::min({fc.frames(), fn.frames(), room});
    const double* a = fc.data();
    const double* b = fn.data();
    for (int i = 0; i < n; ++i) {
        const double t = std::min(1.0, (static_cast<double>(pos + i) + 0.5) / static_cast<double>(len));
        // raised cosine (gains sum to 1): the two chains carry the same, aligned content
        const double gb = 0.5 - 0.5 * std::cos(t * kPi), ga = 1.0 - gb;
        for (int c = 0; c < dch; ++c) {
            const size_t k = static_cast<size_t>(i) * dch + c;
            out[k] = a[k] * ga + b[k] * gb;
        }
        if (gb > 0.0 && gb < 1.0) fq.requantize(out + static_cast<size_t>(i) * dch, dev, dch, modulated);
    }
    fc.pop(n);
    fn.pop(n);
    pos += n;
    return n;
}

}  // namespace daphne::playback
