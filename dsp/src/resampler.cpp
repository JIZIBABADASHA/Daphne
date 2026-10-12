#include "daphne/dsp/resampler.h"

#include <algorithm>
#include <cmath>

#include "dd.h"
#include "dd_conv.h"
#include "fft.h"
#include "parallel.h"
#include "worker_pool.h"

namespace daphne::dsp {

int resampler_block_size(long long L) {
    if (L >= 262144) return 4096;
    if (L >= 65536) return 2048;
    if (L >= 16384) return 1024;
    if (L >= 4096) return 512;
    return 256;
}

Resampler::Resampler(std::vector<double> h, int ratio, int block, const std::vector<double>* h_lo)
    : ratio_(std::max(1, ratio)), block_(std::max(1, block)), kernel_(std::move(h)) {
    taps_ = static_cast<int>((kernel_.size() + ratio_ - 1) / ratio_);
    kernel_.resize(static_cast<size_t>(taps_) * ratio_, 0.0);
    if (h_lo) {
        build_dd(*h_lo);
        peak_ = find_peak();
        return;
    }
    // every phase scaled to DC gain 1 (Neumaier-compensated sum; left alone when |sum| <= 1e-15)
    for (int p = 0; p < ratio_; ++p) {
        double s = 0.0, c = 0.0;
        for (int k = 0; k < taps_; ++k) {
            const double v = kernel_[static_cast<size_t>(k) * ratio_ + p];
            const double t = s + v;
            c += std::abs(s) >= std::abs(v) ? (s - t) + v : (v - t) + s;
            s = t;
        }
        s += c;
        const double scale = std::abs(s) > 1e-15 ? 1.0 / s : 1.0;
        for (int k = 0; k < taps_; ++k) kernel_[static_cast<size_t>(k) * ratio_ + p] *= scale;
    }

    parts_ = (taps_ + block_ - 1) / block_;
    const int n = 2 * block_;
    fft_ = std::make_unique<Fft>(n);
    spectra_.assign(static_cast<size_t>(ratio_) * parts_, std::vector<std::complex<double>>(n));
    const double inv_n = 1.0 / n;   // the inverse FFT is unscaled
    for (int p = 0; p < ratio_; ++p) {
        for (int j = 0; j < parts_; ++j) {
            auto& s = spectra_[static_cast<size_t>(p) * parts_ + j];
            for (int i = 0; i < block_; ++i) {
                const int k = j * block_ + i;
                s[i] = k < taps_ ? kernel_[static_cast<size_t>(k) * ratio_ + p] * inv_n : 0.0;
            }
            fft_->run(s.data(), false);
        }
    }
    peak_ = find_peak();
}

// The double-double kernel: normalized like the double path but in double-double, then every
// partition spectrum computed with the double-double FFT. kernel_ keeps the high parts (peak,
// identity checks).
void Resampler::build_dd(const std::vector<double>& lo) {
    using dd::DD;
    dd_ = true;
    const size_t L = kernel_.size();
    std::vector<DD> k(L);
    for (size_t i = 0; i < L; ++i) k[i] = DD(kernel_[i], i < lo.size() ? lo[i] : 0.0);
    for (int p = 0; p < ratio_; ++p) {
        DD s(0.0);
        for (int t = 0; t < taps_; ++t) s += k[static_cast<size_t>(t) * ratio_ + p];
        const DD scale = std::abs(s.hi) > 1e-15 ? dd::div(DD(1.0), s) : DD(1.0);
        for (int t = 0; t < taps_; ++t) k[static_cast<size_t>(t) * ratio_ + p] = k[static_cast<size_t>(t) * ratio_ + p] * scale;
    }
    for (size_t i = 0; i < L; ++i) kernel_[i] = k[i].hi;

    parts_ = (taps_ + block_ - 1) / block_;
    const int n = 2 * block_;
    dfft_ = std::make_unique<ddc::Fft>(n);
    dspectra_.assign(static_cast<size_t>(ratio_) * parts_, ddc::Planes{});
    const double inv_n = 1.0 / n;   // a power of two: exact in both parts
    for (int p = 0; p < ratio_; ++p)
        for (int j = 0; j < parts_; ++j) {
            ddc::Planes& s = dspectra_[static_cast<size_t>(p) * parts_ + j];
            s.assign(static_cast<size_t>(n));
            for (int i = 0; i < block_; ++i) {
                const int t = j * block_ + i;
                if (t >= taps_) break;
                const DD v = k[static_cast<size_t>(t) * ratio_ + p];
                s.rh[static_cast<size_t>(i)] = v.hi * inv_n;
                s.rl[static_cast<size_t>(i)] = v.lo * inv_n;
            }
            dfft_->run(s, false);
        }
}

Resampler::~Resampler() = default;

void Resampler::prepare(const Format& in) {
    in_ = in;
    // a unit impulse at 1x (no apodizing): copy the samples, the FFT convolution would round them
    bypass_ = is_identity();
    const int ch = std::max(1, in.channels);
    cur_.assign(ch, std::vector<double>(block_, 0.0));
    prev_.assign(ch, std::vector<double>(block_, 0.0));
    fdl_.assign((ch + 1) / 2, std::vector<std::complex<double>>(static_cast<size_t>(parts_) * 2 * block_));
    if (dd_) {
        fdl_.clear();
        dfdl_.assign((ch + 1) / 2, ddc::Planes{});
        for (auto& f : dfdl_) f.assign(static_cast<size_t>(parts_) * 2 * block_);
        dwork_ = std::make_unique<ddc::Planes>();
        dwork_->assign(static_cast<size_t>(2 * block_));
        daccs_.assign(static_cast<size_t>(ratio_), ddc::Planes{});
        for (auto& a : daccs_) a.assign(static_cast<size_t>(2 * block_));
        const int threads = std::min(ratio_, design_threads());
        if (!pool_ || pool_->workers() != threads - 1) pool_ = threads > 1 ? std::make_unique<WorkerPool>(threads - 1) : nullptr;
    } else {
        work_.assign(2 * block_, 0.0);
        acc_.assign(2 * block_, 0.0);
    }
    reset();
}

Format Resampler::output_format() const {
    return {in_.rate * ratio_, in_.channels, max_output_frames(in_.max_frames)};
}

void Resampler::reset() {
    pending_ = 0;
    fdl_pos_ = 0;
    for (auto& v : cur_) std::fill(v.begin(), v.end(), 0.0);
    for (auto& v : prev_) std::fill(v.begin(), v.end(), 0.0);
    for (auto& v : fdl_) std::fill(v.begin(), v.end(), std::complex<double>(0.0));
    for (auto& v : dfdl_) v.zero();
}

int Resampler::find_peak() const {
    if (kernel_.empty()) return 0;
    size_t best = 0;
    for (size_t i = 1; i < kernel_.size(); ++i)
        if (std::abs(kernel_[i]) > std::abs(kernel_[best])) best = i;
    return static_cast<int>(best);
}

bool Resampler::is_identity() const {
    if (ratio_ != 1 || kernel_.empty() || kernel_[0] != 1.0) return false;
    return std::all_of(kernel_.begin() + 1, kernel_.end(), [](double v) { return v == 0.0; });
}

int Resampler::process(const double* in, int frames, double* out) {
    const int ch = static_cast<int>(cur_.size());
    if (bypass_) {
        std::copy(in, in + static_cast<size_t>(frames) * ch, out);
        return frames;
    }
    int produced = 0;
    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < ch; ++c) cur_[c][pending_] = in[static_cast<size_t>(f) * ch + c];
        if (++pending_ == block_) {
            if (dd_) run_block_dd(out + static_cast<size_t>(produced) * ch, block_ * ratio_);
            else run_block(out + static_cast<size_t>(produced) * ch, block_ * ratio_);
            produced += block_ * ratio_;
            pending_ = 0;
        }
    }
    return produced;
}

int Resampler::flush(double* out) {
    if (pending_ == 0) return 0;
    for (auto& v : cur_) std::fill(v.begin() + pending_, v.end(), 0.0);
    const int frames = pending_ * ratio_;
    if (dd_) run_block_dd(out, frames);
    else run_block(out, frames);
    reset();
    return frames;
}

// One block: FFT of [previous block, current block] per channel pair, frequency-domain
// delay line, per phase multiply-accumulate and inverse FFT, keep the second half.
void Resampler::run_block(double* out, int frames_out) {
    const int ch = static_cast<int>(cur_.size());
    const int B = block_, n = 2 * B;
    fdl_pos_ = (fdl_pos_ + parts_ - 1) % parts_;   // newest spectrum
    for (int q = 0; q < static_cast<int>(fdl_.size()); ++q) {
        const int a = 2 * q, b = 2 * q + 1;
        const bool has_b = b < ch;
        for (int i = 0; i < B; ++i) {
            work_[i] = {prev_[a][i], has_b ? prev_[b][i] : 0.0};
            work_[B + i] = {cur_[a][i], has_b ? cur_[b][i] : 0.0};
        }
        fft_->run(work_.data(), false);
        std::copy(work_.begin(), work_.end(), fdl_[q].begin() + static_cast<size_t>(fdl_pos_) * n);
        for (int p = 0; p < ratio_; ++p) {
            std::fill(acc_.begin(), acc_.end(), std::complex<double>(0.0));
            for (int j = 0; j < parts_; ++j) {
                const auto* X = fdl_[q].data() + static_cast<size_t>((fdl_pos_ + j) % parts_) * n;
                const auto* H = spectra_[static_cast<size_t>(p) * parts_ + j].data();
                for (int k = 0; k < n; ++k) acc_[k] += X[k] * H[k];
            }
            fft_->run(acc_.data(), true);
            for (int i = 0; i < B; ++i) {
                const int frame = i * ratio_ + p;
                if (frame >= frames_out) break;
                const double ya = acc_[B + i].real(), yb = acc_[B + i].imag();
                out[static_cast<size_t>(frame) * ch + a] = ya;
                if (has_b) out[static_cast<size_t>(frame) * ch + b] = yb;
            }
        }
    }
    std::swap(cur_, prev_);
}

// The double-double block: the same steps as run_block, every value a double-double; each output
// rounded to double (its high part) at the end.
void Resampler::run_block_dd(double* out, int frames_out) {
    const int ch = static_cast<int>(cur_.size());
    const int B = block_, n = 2 * B;
    ddc::Planes& w = *dwork_;
    fdl_pos_ = (fdl_pos_ + parts_ - 1) % parts_;   // newest spectrum
    for (int q = 0; q < static_cast<int>(dfdl_.size()); ++q) {
        const int a = 2 * q, b = 2 * q + 1;
        const bool has_b = b < ch;
        w.zero();
        for (int i = 0; i < B; ++i) {
            w.rh[static_cast<size_t>(i)] = prev_[a][i];
            w.rh[static_cast<size_t>(B + i)] = cur_[a][i];
            if (has_b) {
                w.ih[static_cast<size_t>(i)] = prev_[b][i];
                w.ih[static_cast<size_t>(B + i)] = cur_[b][i];
            }
        }
        dfft_->run(w, false);
        ddc::Planes& f = dfdl_[static_cast<size_t>(q)];
        const size_t at = static_cast<size_t>(fdl_pos_) * n;
        std::copy(w.rh.begin(), w.rh.end(), f.rh.begin() + at);
        std::copy(w.rl.begin(), w.rl.end(), f.rl.begin() + at);
        std::copy(w.ih.begin(), w.ih.end(), f.ih.begin() + at);
        std::copy(w.il.begin(), w.il.end(), f.il.begin() + at);
        // every phase is independent (own accumulator, own output frames): one job each
        auto phase = [&](int p) {
            ddc::Planes& acc = daccs_[static_cast<size_t>(p)];
            acc.zero();
            for (int j = 0; j < parts_; ++j)
                ddc::mac(acc, f, static_cast<size_t>((fdl_pos_ + j) % parts_) * n, dspectra_[static_cast<size_t>(p) * parts_ + j], 0, n);
            dfft_->run(acc, true);
            for (int i = 0; i < B; ++i) {
                const int frame = i * ratio_ + p;
                if (frame >= frames_out) break;
                const double ya = acc.rh[static_cast<size_t>(B + i)], yb = acc.ih[static_cast<size_t>(B + i)];
                out[static_cast<size_t>(frame) * ch + a] = ya;
                if (has_b) out[static_cast<size_t>(frame) * ch + b] = yb;
            }
        };
        if (pool_) pool_->run(ratio_, phase);
        else
            for (int p = 0; p < ratio_; ++p) phase(p);
    }
    std::swap(cur_, prev_);
}

}  // namespace daphne::dsp
