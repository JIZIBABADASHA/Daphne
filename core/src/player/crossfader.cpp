#include "crossfader.h"

#include <algorithm>
#include <cmath>

namespace daphne::playback {

bool copy_history(const History& h, int64_t from, int64_t to, std::vector<double>* in) {
    if (from < h.valid_from || to - from > h.cap || from > to) return false;
    in->resize(static_cast<size_t>(to - from) * h.ch);
    for (int64_t f = from; f < to; ++f) {
        const int64_t k = f % h.cap;
        std::copy(h.buf.begin() + static_cast<std::ptrdiff_t>(k * h.ch), h.buf.begin() + static_cast<std::ptrdiff_t>((k + 1) * h.ch),
                  in->begin() + static_cast<std::ptrdiff_t>((f - from) * h.ch));
    }
    return true;
}

void Warm::feed(const double* x, int64_t n, int ch) {
    std::vector<double> o(static_cast<size_t>(chain->max_output_frames()) * out.ch);
    for (int64_t f = 0; f < n;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;   // dropped: its state does not matter
        const int k = static_cast<int>(std::min<int64_t>(kChunk, n - f));
        const int got = chain->process(x + static_cast<size_t>(f) * ch, k, o.data());
        const int d = static_cast<int>(std::min<int64_t>(drop, got));
        drop -= d;
        out.push(o.data() + static_cast<size_t>(d) * out.ch, got - d);
        f += k;
    }
    in_done += n;
}

Warm Crossfader::prepare(std::unique_ptr<DspChain> nx, const History& h, int64_t in_total, int64_t abs_out, int cur_delay, int ratio,
                         int dch, std::vector<double>* in) {
    const int64_t have = std::min<int64_t>(in_total - h.valid_from, h.cap);
    const int64_t start = in_total - have;
    copy_history(h, start, in_total, in);
    Warm w;
    w.out.ch = dch;
    w.target = abs_out - cur_delay + nx->content_delay();   // next-chain index of the content the current chain plays next
    w.drop = w.target - start * ratio;
    if (w.drop < 0) {
        w.out.push_zeros(static_cast<int>(-w.drop));
        w.drop = 0;
    }
    w.in_done = start;
    w.chain = std::move(nx);
    return w;
}

void Crossfader::begin(std::unique_ptr<DspChain> nx, const History& h, int64_t in_total, int64_t abs_out, int cur_delay, int ratio,
                       int64_t fade) {
    std::vector<double> in;
    Warm w = prepare(std::move(nx), h, in_total, abs_out, cur_delay, ratio, fn.ch, &in);
    w.feed(in.data(), in_total - w.in_done, h.ch);
    begin(std::move(w), abs_out, cur_delay, fade);
}

void Crossfader::begin(Warm w, int64_t abs_out, int cur_delay, int64_t fade) {
    const int dc = cur_delay, dn = w.chain->content_delay();
    // the current chain has played on since the warm-up started: its output up to there is not needed
    int64_t late = abs_out - dc + dn - w.target;
    if (late < 0) {   // (not after a seek, which ends the warm-up) earlier content: zeros as with a short history
        Fifo z;
        z.ch = w.out.ch;
        z.push_zeros(static_cast<int>(-late));
        z.push(w.out.data(), w.out.frames());
        w.out = std::move(z);
        late = 0;
    }
    const int pop = static_cast<int>(std::min<int64_t>(late, w.out.frames()));
    w.out.pop(pop);
    fn = std::move(w.out);
    drop_next = w.drop + (late - pop);
    // frames of the current chain still queued for output count as already played
    fc.clear();
    if (carry.frames() > 0) {
        fc.push(carry.data(), carry.frames());
        carry.clear();
    }
    delay_shift = dn - dc;
    next = std::move(w.chain);
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
