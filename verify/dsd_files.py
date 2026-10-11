"""DSD files end to end, self-contained: a 997 Hz tone through the modulator (modulator-test, order 8) written as
DSF (1 bit, LSB first, last block zero-padded, ID3 chunk after the data) and DFF, then converted back to PCM with
render (DSD -> PCM at 176.4 kHz).

  1. DSF and DFF of the same bytes decode sample-identically, for DSD64 and DSD128
  2. length: exactly the sample count (no block padding, no ID3 played)
  3. level: the tone comes back at its PCM level (the modulator's 0.5 gain and the converter's +6.02 dB cancel)
  4. every low-pass setting (24 / 30 / 50 kHz) gives the tone at the same level

  python dsd_files.py
"""
import struct
import numpy as np, soundfile as sf
from common import EXE, run, path

fails = 0


def check(label, ok, detail=""):
    global fails
    fails += not ok
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{'  ' + detail if detail else ''}")


def write_dsf(fn, L, R, rate):
    blk = 4096
    rev = np.array([int(f"{i:08b}"[::-1], 2) for i in range(256)], np.uint8)
    n = len(L)
    data = bytearray()
    for k in range((n + blk - 1) // blk):
        for ch in (L, R):
            seg = rev[ch[k * blk:(k + 1) * blk]]
            data += seg.tobytes() + bytes(blk - len(seg))
    id3 = b"ID3\x03\x00\x00\x00\x00\x00\x0a" + b"\xaa\x55" * 5
    hdr = b"DSD " + struct.pack("<QQQ", 28, 28 + 52 + 12 + len(data) + len(id3), 28 + 52 + 12 + len(data))
    fmt = b"fmt " + struct.pack("<QIIIIIIQII", 52, 1, 0, 2, 2, rate, 1, n * 8, blk, 0)
    open(fn, "wb").write(hdr + fmt + b"data" + struct.pack("<Q", 12 + len(data)) + bytes(data) + id3)


def write_dff(fn, L, R, rate):
    inter = np.empty(2 * len(L), np.uint8)
    inter[0::2] = L
    inter[1::2] = R
    cmpr = b"DSD " + bytes([14]) + b"not compressed" + b"\x00"
    prop = (b"SND " + b"FS  " + struct.pack(">QI", 4, rate) + b"CHNL" + struct.pack(">QH", 2 + 8, 2) + b"SLFTSRGT" +
            b"CMPR" + struct.pack(">Q", len(cmpr)) + cmpr)
    body = (b"FVER" + struct.pack(">QI", 4, 0x01050000) + b"PROP" + struct.pack(">Q", len(prop)) + prop +
            b"DSD " + struct.pack(">Q", len(inter)) + inter.tobytes() + (b"\x00" if len(inter) & 1 else b""))
    open(fn, "wb").write(b"FRM8" + struct.pack(">Q", 4 + len(body)) + b"DSD " + body)


fs, sec, db = 44100, 4.0, -20.0
t = np.arange(int(fs * sec)) / fs
x = 10 ** (db / 20) * np.sin(2 * np.pi * 997 * t)
ramp = int(0.05 * fs)
x[:ramp] *= np.linspace(0, 1, ramp)
x[-ramp:] *= np.linspace(1, 0, ramp)
np.stack([x, x], 1).astype(np.float64).tofile(path("tone.f64"))

for osr in (64, 128):
    print(f"DSD{osr}")
    run([EXE, "modulator-test", str(fs), str(osr), "8", "2", path("tone.f64"), path("tone.bin"), "4096"])
    b = np.frombuffer(open(path("tone.bin"), "rb").read(), np.uint8)
    L, R = b[0::2].copy(), b[1::2].copy()
    rate = 44100 * osr
    write_dsf(path("t.dsf"), L, R, rate)
    write_dff(path("t.dff"), L, R, rate)
    out = {}
    for lp in (24, 30, 50):
        for ext in ("dsf", "dff"):
            run([EXE, "render", path(f"t.{ext}"), path(f"o_{ext}_{lp}.wav"), "--dsd-lowpass", str(lp)])
            out[ext, lp] = sf.read(path(f"o_{ext}_{lp}.wav"), always_2d=True)
    a, fa = out["dsf", 24]
    check("DSF and DFF decode identically (all low-pass settings)",
          all(np.array_equal(out["dsf", lp][0], out["dff", lp][0]) for lp in (24, 30, 50)))
    want = len(L) * 8 // (rate // fa)
    check("length = the sample count", len(a) == want, f"{len(a)} frames at {fa} Hz (expected {want})")
    for lp in (24, 30, 50):
        y = out["dsf", lp][0][fa // 2:-fa // 2, 0]
        n = len(y)
        w = np.hanning(n)
        k = int(round(997 * n / fa))
        spec = np.abs(np.fft.rfft(y * w)) / (w.sum() / 2)
        level = 20 * np.log10(spec[k - 3:k + 4].max())
        check(f"low-pass {lp} kHz: tone at {db:.0f} dBFS", abs(level - db) < 0.1, f"{level:.2f} dBFS")

print("\nall passed" if not fails else f"\n{fails} FAILED")
