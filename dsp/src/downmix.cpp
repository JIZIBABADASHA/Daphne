#include "daphne/dsp/downmix.h"

#include <algorithm>
#include <bit>

namespace daphne::dsp {

namespace {

constexpr double kK = 0.7071067811865476;     // 1 / sqrt(2)
constexpr double k71 = 0.408248290463863;     // 1 / sqrt(6)

enum : uint32_t {
    FL = 0x1, FR = 0x2, FC = 0x4, LFE = 0x8, BL = 0x10, BR = 0x20, BC = 0x100, SL = 0x200, SR = 0x400
};

}  // namespace

uint32_t Downmix::default_mask(int channels) {
    switch (channels) {
    case 1: return FC;
    case 2: return FL | FR;
    case 3: return FL | FR | FC;
    case 4: return FL | FR | BL | BR;                       // quad
    case 5: return FL | FR | FC | BL | BR;                  // 5.0
    case 6: return FL | FR | FC | LFE | BL | BR;            // 5.1
    case 7: return FL | FR | FC | LFE | BC | SL | SR;       // 6.1
    case 8: return FL | FR | FC | LFE | BL | BR | SL | SR;  // 7.1
    default: return 0;
    }
}

void Downmix::prepare(const Format& in) {
    in_ = in;
    out_ = in;
    const int ch = in.channels;
    cl_.assign(static_cast<size_t>(std::max(ch, 2)), 0.0);
    cr_.assign(cl_.size(), 0.0);
    if (ch == 1) {
        kind_ = Kind::Mono;
        out_.channels = 2;
        cl_[0] = cr_[0] = 1.0;
        return;
    }
    if (ch <= 2) {
        kind_ = Kind::Copy;
        cl_[0] = cr_[1] = 1.0;
        return;
    }
    out_.channels = 2;
    kind_ = Kind::Matrix;

    // roles from the channel mask: stream order = ascending mask bits
    uint32_t m = mask_;
    if (std::popcount(m) != ch) m = default_mask(ch);
    int fl = -1, fr = -1, c = -1, lfe = -1, bl = -1, br = -1, bc = -1, sl = -1, sr = -1;
    if (m == 0) {   // no known layout: the first two channels
        cl_[0] = cr_[1] = 1.0;
        return;
    }
    int idx = 0;
    for (uint32_t bit = 1; bit != 0 && idx < ch; bit <<= 1) {
        if (!(m & bit)) continue;
        switch (bit) {
        case FL: fl = idx; break;
        case FR: fr = idx; break;
        case FC: c = idx; break;
        case LFE: lfe = idx; break;
        case BL: bl = idx; break;
        case BR: br = idx; break;
        case BC: bc = idx; break;
        case SL: sl = idx; break;
        case SR: sr = idx; break;
        default: break;   // front-of-centre and height channels: not mixed
        }
        ++idx;
    }

    // The formula depends only on whether centre / LFE exist and on the number of surround pairs
    // (back, side, back centre); every surround channel enters with the same weight.
    const bool back = bl >= 0 || br >= 0, side = sl >= 0 || sr >= 0, centre = c >= 0 || lfe >= 0;
    const int pairs = (back ? 1 : 0) + (side ? 1 : 0) + (bc >= 0 ? 1 : 0);
    if (pairs == 0 && !centre && fl < 0 && fr < 0) {   // nothing recognised: the first two channels
        cl_[0] = cr_[1] = 1.0;
        return;
    }

    // overall scale and surround weight of the chosen formula
    double g = 1.0;
    if (pairs == 0) g = centre ? kK : 1.0;
    else if (pairs == 1) g = centre ? 0.5 : kK;
    else g = k71;
    const double gs = g * kK;   // every surround channel enters at 0.7071 before the overall scale

    auto add = [](std::vector<double>& v, int k, double x) {
        if (k >= 0) v[static_cast<size_t>(k)] += x;
    };
    add(cl_, fl, g);
    add(cr_, fr, g);
    if (centre) {   // t = 0.7071 C + 0.5 LFE into both sides
        add(cl_, c, g * kK);
        add(cr_, c, g * kK);
        add(cl_, lfe, g * 0.5);
        add(cr_, lfe, g * 0.5);
    }
    if (pairs > 0) {
        add(cl_, bl, gs);
        add(cr_, br, gs);
        add(cl_, sl, gs);
        add(cr_, sr, gs);
        // a back centre is split x 0.7071 into the back pair (or forms it when there is none)
        add(cl_, bc, gs * kK);
        add(cr_, bc, gs * kK);
    }
}

int Downmix::process(const double* in, int frames, double* out) {
    const int ch = in_.channels;
    if (kind_ == Kind::Copy) {
        if (in != out) std::copy(in, in + static_cast<size_t>(frames) * ch, out);
        return frames;
    }
    if (kind_ == Kind::Mono) {
        for (int i = frames - 1; i >= 0; --i) out[2 * i] = out[2 * i + 1] = in[i];   // backwards: in == out allowed
        return frames;
    }
    // Row i is read completely before out[2i], out[2i+1] are written, and 2i+1 < (i+1)*ch, so
    // in == out is allowed.
    const double* cl = cl_.data();
    const double* cr = cr_.data();
    for (int i = 0; i < frames; ++i) {
        const double* x = in + static_cast<size_t>(i) * ch;
        double l = 0.0, r = 0.0;
        for (int k = 0; k < ch; ++k) {
            l += cl[k] * x[k];
            r += cr[k] * x[k];
        }
        out[2 * i] = l;
        out[2 * i + 1] = r;
    }
    return frames;
}

}  // namespace daphne::dsp
