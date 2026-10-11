#pragma once
// Parametric EQ: biquad coefficient design, automatic preamp, and the biquad cascade.
//
// Band design (fs = the rate the EQ runs at; f clamped to [10 Hz, 0.495 fs]):
//   Peaking     RBJ below 0.2 fs; from 0.2 fs, where the bilinear transform cramps the bell, the
//               matched design (Vicanek 2016) of the same analog prototype: poles matched, zeros
//               fitted to the analog magnitude at DC and at f0 with zero slope there. |gain| <
//               1e-4 dB is a pass-through section. Q clamped to [0.05, 40].
//   LowShelf / HighShelf   RBJ shelves; q is the slope S, clamped to [0.1, 10].
//   HighPass12 / LowPass12 RBJ, Q clamped to [0.1, 10].
//   HighPass24 / LowPass24 two RBJ sections with Q 0.7071 (Linkwitz-Riley, -6 dB at f).
//   Notch / AllPass / BandPass  RBJ, Q clamped to [0.1, 30]; the band-pass peaks at 0 dB.
//
// Automatic preamp (<= 0 dB): the peak of the cascade's magnitude on a 501-point log grid 20 Hz ..
// 20 kHz plus the band frequencies, plus an estimate of the step overshoot of boosting peaking /
// shelf bands with Q > 0.7071, plus 0.2 dB. A manual preamp only lowers it: min(manual, auto).

#include <memory>
#include <vector>

#include "daphne/dsp/block.h"
#include "daphne/dsp/mailbox.h"

namespace daphne::dsp {

// y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 (a0 = 1)
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

enum class PeqType { Peaking, LowShelf, HighShelf, HighPass12, HighPass24, LowPass12, LowPass24, Notch, AllPass, BandPass };

struct PeqBand {
    bool enabled = true;
    PeqType type = PeqType::Peaking;
    double freq = 1000.0;
    double gain_db = 0.0;
    double q = 0.7071;   // Q, or the slope S for the shelves
};

// The sections of the enabled bands, in order (the 24 dB types give two).
std::vector<Biquad> design_peq(const std::vector<PeqBand>& bands, double fs);
Biquad peq_peaking(double fs, double f, double q, double gain_db);
Biquad peq_shelf(bool high, double fs, double f, double slope, double gain_db);
Biquad peq_highpass(double fs, double f, double q);
Biquad peq_lowpass(double fs, double f, double q);

// Automatic preamp in dB (<= 0).
double peq_auto_preamp_db(const std::vector<PeqBand>& bands, const std::vector<Biquad>& sections, double fs);

// |H(f)| of the cascade.
double peq_magnitude(const std::vector<Biquad>& sections, double f, double fs);

struct PeqParams {
    bool enabled = false;
    std::vector<Biquad> sections;
    double preamp = 1.0;   // linear
};

// out = cascade(x * preamp), per channel.
//
// set_params() may be called from any (one) control thread at any time; it never blocks the audio
// thread (mailbox.h). Coefficient and preamp changes with the same section count glide: every
// 15 ms each coefficient moves 0.28 of the remaining distance, snapping to the target below 1e-5.
// A change in the section count or turning the EQ on / off crossfades the old and the new
// configuration over 50 ms (raised cosine). A section whose state becomes non-finite or reaches
// 1e9 (an unstable or overflowing filter) is cleared and outputs silence for that block.
// The cascade runs section by section over each block (the recursion state stays in registers).
class Peq final : public Block {
public:
    static constexpr int kMaxSections = 128;

    void set_params(std::unique_ptr<PeqParams> p);

    void prepare(const Format& in) override;
    Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override;

private:
    void apply(const PeqParams& p, bool jump);
    void glide_step();
    void filter_run(const Biquad* secs, int nsec, double* state, double pre, const double* in, int frames, double* out);

    Format fmt_{};
    Mailbox<PeqParams> box_;
    std::atomic<bool> want_active_{false};       // the newest posted parameters filter (for is_identity)
    std::vector<Biquad> cur_, target_;           // coefficients in use / gliding towards
    int nsec_ = 0;
    double preamp_ = 1.0, target_pre_ = 1.0;
    bool gliding_ = false;
    int tick_frames_ = 1, tick_pos_ = 0;
    std::vector<double> state_;                  // per section, per channel: x1 x2 y1 y2
    // crossfade from the previous configuration (count change / on-off)
    bool active_ = false, fading_ = false, old_active_ = false;
    int old_nsec_ = 0, fade_len_ = 1, fade_pos_ = 0;
    double old_pre_ = 1.0;
    std::vector<Biquad> old_;
    std::vector<double> old_state_, tmp_;
};

}  // namespace daphne::dsp
