"""Resampling filter design: frequency response of design_lowpass over taps, ratios, phase and apodizing.

  pass ripple   max |20 log10 |H|| up to 20 kHz (0.4535 fs_in with apodizing)
  stop          highest level from 0.5 fs_in (the input Nyquist) up to fs_out / 2
  @22.05k       level at 0.5 fs_in exactly
  delay         group delay at 1 kHz in output samples (linear phase: (L - 1) / 2)
  AVX2 = scalar the vector and scalar double-double paths give the same bits

  python filter_design.py
"""
import os, subprocess
import numpy as np
from common import EXE, path, run

def design(fi, fo, taps, phase, apod, env=None):
    h, l = path("h.f64"), path("l.f64")
    args = [EXE, "filter-dump", str(fi), str(fo), h, l, "--taps", str(taps), "--phase", str(phase)] + (["--apodize"] if apod else [])
    r = subprocess.run(args, capture_output=True, text=True, env=env)
    if r.returncode: raise SystemExit(r.stdout + r.stderr)
    return np.fromfile(h), np.fromfile(l), r.stdout.strip().split("designed in ")[1]

def response(h, fo, n=1 << 22):
    H = np.fft.rfft(h, n)
    f = np.fft.rfftfreq(n, 1 / fo)
    return f, H

fails = 0
print(f"{'fs_in->fs_out':>15} {'taps':>6} {'phase':>5} {'apod':>4} {'L':>7} {'ripple dB':>10} {'stop dB':>8} {'@Nyq dB':>8} {'delay':>9} {'time':>9}")
for fi, fo in ((44100, 176400), (44100, 352800), (96000, 384000)):
    for taps in (1024, 4096, 16384):
        for phase in (0.0, 0.5, 1.0):
            for apod in (False, True):
                h, lo, t = design(fi, fo, taps, phase, apod)
                f, H = response(h, fo)
                db = 20 * np.log10(np.abs(H) + 1e-300)
                pe = 0.4535 * fi if apod else min(20000.0, 0.4535 * fi)
                ripple = np.max(np.abs(db[f <= pe]))
                stop = np.max(db[f >= 0.5 * fi])
                nyq = db[np.argmin(np.abs(f - 0.5 * fi))]
                k = np.argmin(np.abs(f - 1000))
                ph = np.unwrap(np.angle(H[:k + 3]))
                gd = -(ph[k + 1] - ph[k - 1]) / (2 * np.pi * (f[k + 1] - f[k - 1])) * fo
                print(f"{fi:>6}->{fo:<7} {taps:>6} {phase:>5} {'yes' if apod else 'no':>4} {len(h):>7} {ripple:>10.2e} {stop:>8.1f} {nyq:>8.1f} {gd:>9.1f} {t:>9}")
                if abs(np.sum(h) - 1.0) > 1e-12: fails += 1; print("   FAIL: DC gain", np.sum(h))
                if phase == 0.0 and not np.allclose(h, h[::-1], atol=1e-18): fails += 1; print("   FAIL: not symmetric")
# vector and scalar double-double paths
a = design(44100, 176400, 8192, 0.5, False)
b = design(44100, 176400, 8192, 0.5, False, env=dict(os.environ, DAPHNE_DD_SCALAR="1"))
same = np.array_equal(a[0], b[0]) and np.array_equal(a[1], b[1])
fails += not same
print(f"\nAVX2 = scalar: {'yes' if same else 'NO'}")
print("all passed" if not fails else f"{fails} FAILED")
