#pragma once
// The resampling / reconstruction low-pass, designed in double-double.
//
// L = taps * ratio coefficients (ratio = the integer upsampling factor), cutoff in cycles per
// output sample, DC gain 1 (the resampler multiplies by ratio).
//
//   prototype   Kaiser-windowed sinc, centred, the window 0 at both ends
//     default   attenuation target A = clamp(16 log10(max(Lp, 128)) + 105, 115, 185) dB,
//               beta = 0.1102 (A - 8.7), transition width (A - 7.95) / (14.36 taps) of the input
//               rate, stop band from 0.5 fs_in (the cutoff half a transition below Nyquist)
//     apodize   pass band to 0.4535 fs_in (20 kHz at 44.1 kHz), stop band from 0.5 fs_in, the
//               window as narrow as that transition allows (beta = pi NW with a half bandwidth of
//               1.5 x half the transition), so the filter rings briefly; also applied at ratio 1
//   phase       0 = linear, 1 = minimum, in between = partial minimum phase: the cepstrum of
//               log|H| (|H|^2 floored at -400 dB) has its causal part scaled by 1 + phase and its
//               anti-causal part by 1 - phase; the L output samples are placed where the response
//               just outside both ends is smallest, with raised-cosine fades of min(L/16, 32)
//               samples at both ends. With a phase the prototype gets 75% of the taps, so the
//               longer response fits.
//
// ratio 1 without apodizing is a unit impulse (nothing to filter: bit-perfect).
// Returns the high parts; *lo gets the low parts (for the double-double convolution).
// Reports progress (progress.h) and runs on design_threads() threads; the result does not depend
// on the thread count.

#include <vector>

namespace daphne::dsp {

std::vector<double> design_lowpass(int taps, int ratio, double fs_in, double fs_out, double phase, bool apodize,
                                   std::vector<double>* lo = nullptr);

}  // namespace daphne::dsp
