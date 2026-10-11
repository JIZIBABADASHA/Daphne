"""DSD modulator coefficient design (dsp/src/sdm_tables.inc).

NTF: Schreier's synthesizeNTF (optimized zeros, H_inf).
Realization: CIFF (cascade of delaying integrators, resonators g on the odd/even pairs as
Schreier's stuffABCD, feed-forward a to the quantizer, u fed to the first integrator and
directly to the quantizer). a is solved from the loop filter's Markov parameters.
Scaling: diagonal, from simulation, so every state peaks at 1 for inputs up to the maximum
stable amplitude; the core clamps at kClamp only as protection.

Needs the python-deltasigma 0.2.2 sources (patched for numpy 2) on sys.path for
synthesizeNTF: set DELTASIGMA_PATH to the folder that contains the `deltasigma` package.

  python sdm_design.py            -> prints the table and SNRs, writes ../dsp/src/sdm_tables.inc
"""
import os, sys, warnings
import numpy as np
from numba import njit

warnings.filterwarnings("ignore")
sys.path.insert(0, os.environ.get("DELTASIGMA_PATH", "."))
import deltasigma as ds  # noqa: E402
from deltasigma import _ds_optzeros  # noqa: E402

# optimal zeros beyond the toolbox's table (order 15): the roots of the Legendre polynomial
for _n in (15,):
    _r = np.polynomial.legendre.legroots([0] * _n + [1])
    _oz = np.sort(_r[_r >= -1e-12]); _oz[np.abs(_oz) < 1e-12] = 0.0
    _ds_optzeros._oznopt[_n] = {1: _oz, 2: _oz}


def topology(order, g):
    """A (n x n) and Be (n) of the CIFF loop: x' = A x + Be (u - v); y = u + a.x"""
    A = np.eye(order)
    for i in range(1, order):
        A[i, i - 1] = 1.0
    odd = order % 2
    for k, i in enumerate(range(odd, order - 1, 2)):
        A[i, i + 1] = -g[k]
    Be = np.zeros(order); Be[0] = 1.0
    return A, Be


def realize(order, osr, h_inf):
    z, p, _ = ds.synthesizeNTF(order, osr, 1, h_inf, 0)
    z = np.asarray(z, complex); p = np.asarray(p, complex)
    # zeros: real first (odd order), then conjugate pairs with increasing frequency
    zr = [x for x in z if abs(x.imag) < 1e-12]
    zc = sorted([x for x in z if x.imag > 1e-12], key=lambda x: x.imag)
    g = np.array([x.imag ** 2 for x in zc])          # Schreier: zeros moved to Re = 1
    A, Be = topology(order, g)
    # target: 1 / NTF - 1 = D / N - 1 with the zeros actually realized (1 +- j sqrt g)
    zz = np.concatenate([np.ones(len(zr)), np.concatenate([[1 + 1j * np.sqrt(x), 1 - 1j * np.sqrt(x)] for x in g]) if len(g) else []])
    N = np.real(np.poly(zz)); D = np.real(np.poly(p))
    # Markov parameters of (D - N) / N in z^-1
    num = D - N  # num[0] = 0
    h = np.zeros(order + 1)
    for k in range(1, order + 1):
        h[k] = num[k] - sum(N[j] * h[k - j] for j in range(1, k + 1))
    K = np.zeros((order, order)); v = Be.copy()
    for k in range(order):
        K[:, k] = v; v = A @ v
    a = np.linalg.solve(K.T, h[1:])
    return A, Be, a, g, p


@njit(cache=True)
def sim(A, Be, a, u, clamp):
    n = A.shape[0]
    x = np.zeros(n); nx = np.zeros(n)
    out = np.empty(u.shape[0]); xmax = np.zeros(n)
    for t in range(u.shape[0]):
        y = u[t]
        for i in range(n):
            y += a[i] * x[i]
        v = 1.0 if y >= 0.0 else -1.0
        out[t] = v
        e = u[t] - v
        for i in range(n):
            s = Be[i] * e
            for j in range(n):
                s += A[i, j] * x[j]
            nx[i] = s
        for i in range(n):
            xi = nx[i]
            if clamp > 0.0:
                if xi > clamp: xi = clamp
                elif xi < -clamp: xi = -clamp
            x[i] = xi
            ax = abs(xi)
            if ax > xmax[i]: xmax[i] = ax
    return out, xmax


def max_stable(A, Be, a, osr, n=1 << 15):
    """largest sine amplitude (step 0.01) that keeps the states bounded"""
    t = np.arange(n); f = 0.37 / (2 * osr)  # in-band tone
    ok = 0.0; peaks = None
    _, base = sim(A, Be, a, 0.05 * np.sin(2 * np.pi * f * t), 0.0)   # unscaled states can be ~1e6
    for amp in np.arange(0.05, 1.0, 0.01):
        u = amp * np.sin(2 * np.pi * f * t)
        _, xm = sim(A, Be, a, u, 0.0)
        if not np.all(np.isfinite(xm)) or np.max(xm / base) > 20:
            break
        ok = amp; peaks = xm if peaks is None else np.maximum(peaks, xm)
    return ok, peaks


def scale(A, Be, a, s):
    """states x' = x / s"""
    S = np.diag(s); Si = np.diag(1 / s)
    return Si @ A @ S, Si @ Be, a * s


def snr(bits, osr, k0, nb):
    w = np.blackman(len(bits))
    S = np.abs(np.fft.rfft((bits - bits.mean()) * w)) ** 2
    band = len(bits) // (2 * osr)
    sig = S[k0 - nb:k0 + nb + 1].sum()
    noise = S[1:band].sum() - sig
    return 10 * np.log10(sig / noise)


def evaluate(A, Be, a, osr, gain, clamp, levels=(-1, -20, -60), n=1 << 18):
    k0 = int(round(997 / 22050 * n / (2 * osr)))  # 997 Hz at fs_in 44.1k
    t = np.arange(n)
    out = []
    for db in levels:
        u = gain * 10 ** (db / 20) * np.sin(2 * np.pi * k0 / n * t)
        bits, xm = sim(A, Be, a, u, clamp)
        out.append(snr(bits, osr, k0, 4))
    return out


def design(order, osr, h_inf):
    A, Be, a, g, p = realize(order, osr, h_inf)
    if np.max(np.abs(p)) >= 1:
        return None
    umax, peaks = max_stable(A, Be, a, osr)
    if peaks is None:
        return None
    As, Bs, as_ = scale(A, Be, a, peaks)
    return dict(order=order, osr=osr, h_inf=h_inf, umax=umax, A=As, Be=Bs, a=as_, g=g, s=peaks)


KCLAMP = 1.5
GAIN = 0.5
# H_inf fixed by measurement instead of the SNR score (an in-chain sweep of the ultrasonic noise):
# the Scarlet Book (Annex D4) asks 40 - 100 kHz < -20 dB re the SACD reference; a 1-bit loop's ultrasonic noise there
# changes with the signal level. Order 8 at DSD128: the scored 1.30 measured -20.1 dB (-20 dBFS input), 1.25 -20.8,
# 1.35 -31.2 with the best SNR (umax 0.70: the limiter's ceiling is +1.5 dB re the SACD reference). Order 8 at DSD64
# keeps the scored 1.40 (-18.3 dB with a signal): no H_inf meets D4 there without a worse loop (1.35 / 1.30: -18.9 on
# silence and more 20 - 40 kHz noise; 1.25: 20 - 40 kHz -33 dB, SNR 108 dB).
H_FIXED = {(8, 128): 1.35}

if __name__ == "__main__":
    rows = []
    # Two orders: 5 (least ultrasonic noise) and 8 (the quietest audio band). Higher orders lower an audio-band
    # noise floor that is already far below any DAC's analog noise, at the cost of more ultrasonic noise.
    for osr in (64, 128, 256, 512, 1024):
        for order in (5, 8):
            best = None
            hs = (H_FIXED[(order, osr)],) if (order, osr) in H_FIXED else (1.5, 1.45, 1.4, 1.35, 1.3, 1.25, 1.2, 1.15, 1.1)
            for h in hs:
                d = design(order, osr, h)
                if d is None or d["umax"] < 0.65:   # gain 0.5 leaves >= 2.3 dB above full scale
                    continue
                s = evaluate(d["A"], d["Be"], d["a"], osr, GAIN, KCLAMP)
                if s[0] < 60:   # diverged (the stability scan is fooled when even the baseline is unstable)
                    continue
                d["snr"] = s
                print(f"  order {order} osr {osr} H_inf {h}: umax {d['umax']:.2f}  SNR {s[0]:.1f} / {s[1]:.1f} / {s[2]:.1f}", flush=True)
                d["score"] = min(s[0] - 110, s[1] - 90, s[2] - 50)   # worst margin to the SNR targets (-1 / -20 / -60 dBFS)
                if best is None or d["score"] > best["score"]:
                    best = d
            if best is None:
                print(f"order {order} osr {osr}: no stable design", flush=True)
                continue
            print(f"order {order} osr {osr}: best H_inf {best['h_inf']} umax {best['umax']:.2f} SNR {best['snr']}", flush=True)
            rows.append(best)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dsp", "src", "sdm_tables.inc")
    with open(out, "w") as f:
        f.write("// Generated by tools/sdm_design.py -- do not edit.\n")
        f.write("// {order, osr, h_inf, umax, a[15], k[15] (input of state i), g[7] (feedback into the pair head)}\n")
        for d in rows:
            n = d["order"]; A = d["A"]
            k = [d["Be"][0]] + [A[i, i - 1] for i in range(1, n)]
            odd = n % 2
            gg = [-A[i, i + 1] for i in range(odd, n - 1, 2)]
            fmt = lambda v, m: "{" + ", ".join(float(x).hex() for x in list(v) + [0.0] * (m - len(v))) + "}"
            f.write(f"{{{n}, {d['osr']}, {d['h_inf']}, {d['umax']:.2f}, {fmt(d['a'], 15)},\n  {fmt(k, 15)},\n  {fmt(gg, 7)}}},\n")
