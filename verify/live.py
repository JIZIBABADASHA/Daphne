"""Live playback through the VB Hi-Fi Cable (WASAPI exclusive into "Hi-Fi Cable Input", device 0 in `daphne devices`).

  1. bit-perfect: random 24- and 16-bit files (full-scale extremes included), default settings: every sample
     arrives unchanged
  2. gapless across sample rates: A 44.1 kHz, B 44.1 / 88.2 kHz, one continuous 1 kHz tone, output.rate 176400
     (A x4, B x2, the device stays open); fitted with one continuous sine in 40 ms windows. The linear-phase
     residual at the boundary is the test signal's own limit (two files sampled at different rates, each with an
     abrupt end / start: an offline splice with ideal upsampling gives the same, about -66 dB)
  3. per-track gain across a gapless boundary through the C API (dc_load with headroom -6, dc_set_next_gain -12):
     the level steps from -9 to -15 dBFS at the boundary (the gain ramps over 20 ms), not when the switch is
     reported
  4. DoP through pause / resume / seek: the DoP marker sequence never breaks (DSD64, 176.4 kHz)

  python live.py
"""
import ctypes, json, os, struct, subprocess, threading, time
import numpy as np, soundfile as sf
from capture import Recorder
from common import EXE, ROOT, path, run

DLL = os.path.join(ROOT, "build", "bin", "Release", "daphne_core.dll")
fails = 0


def check(label, ok, detail=""):
    global fails
    fails += not ok
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{'  ' + detail if detail else ''}", flush=True)


def play(args, rec_fs, max_s=15):
    proc = []
    r = Recorder(rec_fs, max_s).record(lambda: proc.append(subprocess.Popen([EXE, "play", "--device", "0", *args],
                                                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)))
    log = proc[0].communicate(timeout=60)[0]
    return r, log


# ---------------------------------------------------------------- 1. bit-perfect
print("1. bit-perfect (44.1 kHz, default settings)")
rng = np.random.default_rng(7)
js = path("plain.json")
json.dump({}, open(js, "w"))
for bits in (24, 16):
    n = 44100 * 4
    lim = 2 ** (bits - 1)
    v = rng.integers(-lim, lim, size=(n, 2), dtype=np.int64)
    v[:200] = 0
    v[1000:1010, :] = [[lim - 1, -lim]] * 10
    f = path(f"bp_{bits}.wav")
    sf.write(f, (v << (32 - bits)).astype(np.int32), 44100, subtype=f"PCM_{bits}")
    r, log = play(["--settings", js, f], 44100, 12)
    rec = r.astype(np.int64) >> 8
    want = v << (24 - bits)
    key = want[200:2200, 0]
    start = next((i - 200 for i in range(len(rec) - len(key)) if rec[i, 0] == key[0] and np.array_equal(rec[i:i + len(key), 0], key)), None)
    if start is None:
        check(f"{bits}-bit", False, "the file was not found in the recording")
        continue
    diff = np.count_nonzero(rec[start:start + n] != want)
    check(f"{bits}-bit: {n} frames x 2 ch, {diff} samples differ", diff == 0 and "bit-perfect: yes" in log)

# ---------------------------------------------------------------- 2. gapless across rates
print("2. gapless across sample rates (output.rate 176400)")
F0, AMP, SEC = 1000.0, 0.3, 3.0
RFS = 176400


def tone(fs, t0, sec):
    t = t0 + np.arange(int(round(fs * sec))) / fs
    return AMP * np.sin(2 * np.pi * F0 * t)


for rate_b, phase, limit in ((44100, "linear", -120), (88200, "linear", -60), (88200, "intermediate", -38)):
    fa, fb = path("gx_a.wav"), path("gx_b.wav")
    sf.write(fa, tone(44100, 0.0, SEC), 44100, subtype="PCM_24")
    sf.write(fb, tone(rate_b, SEC, SEC), rate_b, subtype="PCM_24")
    gj = path("gx.json")
    json.dump({"output": {"rate": "176400"}, "filter": {"phase": phase}}, open(gj, "w"))
    r, log = play(["--settings", gj, "--next", fb, fa], RFS)
    y = r[:, 0] / 2 ** 31
    on = np.flatnonzero(np.abs(y) > 0.01)
    y = y[on[0]:on[-1]]
    w = int(0.04 * RFS)
    worst, at = -300.0, 0
    tt = np.arange(w) / RFS
    for k in range(int(0.2 * RFS) // w, (len(y) - int(0.2 * RFS)) // w):   # the start and end of playback left out
        seg = y[k * w:(k + 1) * w]
        A = np.stack([np.sin(2 * np.pi * F0 * tt), np.cos(2 * np.pi * F0 * tt), np.ones(w)], 1)
        res = seg - A @ np.linalg.lstsq(A, seg, rcond=None)[0]
        lvl = 20 * np.log10(np.sqrt(np.mean(res ** 2)) / (AMP / np.sqrt(2)) + 1e-30)
        if lvl > worst:
            worst, at = lvl, k * w / RFS
    gap = "gapless: next track" in log
    check(f"B {rate_b} Hz, {phase}: worst residual {worst:.1f} dB at {at:.2f} s (boundary {SEC:.2f} s)", gap and worst < limit)

# ---------------------------------------------------------------- 3. per-track gain through the C API
print("3. per-track gain across a gapless boundary (C API)")
dc = ctypes.CDLL(DLL)
dc.dc_create.restype = ctypes.c_void_p
dc.dc_destroy.argtypes = dc.dc_stop.argtypes = [ctypes.c_void_p]
dc.dc_load.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_double, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
dc.dc_set_next_gain.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_double]
dc.dc_poll_event.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
fs = 44100
t = np.arange(int(fs * SEC)) / fs
x = 10 ** (-3 / 20) * np.sin(2 * np.pi * 1000 * t)
fa, fb = path("gg_a.wav"), path("gg_b.wav")
sf.write(fa, np.stack([x, x], 1), fs, subtype="PCM_24")
sf.write(fb, np.stack([np.sin(2 * np.pi * 1000 * (t + SEC)) * 10 ** (-3 / 20)] * 2, 1), fs, subtype="PCM_24")
p = dc.dc_create()
events = []
polling = threading.Event()
poller = []


def start():
    err = ctypes.create_string_buffer(512)
    st = {"output": {"device": "0"}, "headroom_db": -6.0}
    rc = dc.dc_load(p, fa, json.dumps(st).encode(), 0.0, 1, err, len(err))
    if rc != 0:
        print("dc_load failed", rc, err.value)
    dc.dc_set_next_gain(p, fb, -12.0)

    def poll():
        buf = ctypes.create_string_buffer(4096)
        while not polling.is_set():
            while dc.dc_poll_event(p, buf, len(buf)) > 0:
                events.append(buf.value.decode())
            time.sleep(0.025)
    poller.append(threading.Thread(target=poll))
    poller[0].start()


r = Recorder(fs, 15).record(start)
polling.set()
poller[0].join()
dc.dc_stop(p)
dc.dc_destroy(p)
y = r[:, 0] / 2 ** 31
on = np.flatnonzero(np.abs(y) > 0.01)[0]
y = y[on:]
lvl = lambda a, b: 20 * np.log10(np.sqrt(2 * np.mean(y[int(a * fs):int(b * fs)] ** 2)))
before, after = lvl(0.9, 1.1), lvl(4.9, 5.1)
env = np.sqrt(2 * np.convolve(y ** 2, np.ones(441) / 441, "same"))
cross = np.flatnonzero(20 * np.log10(env + 1e-12) < -12)
cross = cross[cross > int(1.5 * fs)][0] / fs
check(f"level {before:.2f} -> {after:.2f} dBFS (expect -9 / -15)", abs(before + 9) < 0.05 and abs(after + 15) < 0.05)
check(f"the step crosses -12 dBFS at {cross:.3f} s (boundary {SEC:.3f} s, the ramp's midpoint is 10 ms in)", abs(cross - SEC - 0.012) < 0.006)
check("the track event arrived", any('"track"' in e for e in events))

# ---------------------------------------------------------------- 4. DoP transport
print("4. DoP through pause / resume / seek (DSD64 at 176.4 kHz)")
x = 10 ** (-20 / 20) * np.sin(2 * np.pi * 997 * np.arange(44100 * 6) / 44100)
np.stack([x, x], 1).astype(np.float64).tofile(path("dop.f64"))
run([EXE, "modulator-test", "44100", "64", "8", "2", path("dop.f64"), path("dop.bin"), "4096"])
b = np.frombuffer(open(path("dop.bin"), "rb").read(), np.uint8)
L, R = b[0::2], b[1::2]
inter = np.empty(2 * len(L), np.uint8)
inter[0::2], inter[1::2] = L, R
cmpr = b"DSD " + bytes([14]) + b"not compressed" + b"\x00"
prop = (b"SND " + b"FS  " + struct.pack(">QI", 4, 2822400) + b"CHNL" + struct.pack(">QH", 10, 2) + b"SLFTSRGT" +
        b"CMPR" + struct.pack(">Q", len(cmpr)) + cmpr)
body = (b"FVER" + struct.pack(">QI", 4, 0x01050000) + b"PROP" + struct.pack(">Q", len(prop)) + prop +
        b"DSD " + struct.pack(">Q", len(inter)) + inter.tobytes())
dff = path("dop.dff")
open(dff, "wb").write(b"FRM8" + struct.pack(">Q", 4 + len(body)) + b"DSD " + body)
r, log = play(["--settings", js, dff, "--script", "1.5:pause,2.5:resume,3.5:seek=1,4.5:pause,5:seek=0.5,5.5:resume"], 176400, 20)
u = (r.astype(np.int64) >> 8) & 0xFFFFFF
mk = (u[:, 0] >> 16) & 0xFF
valid = (mk == 0x05) | (mk == 0xFA)
idx = np.flatnonzero(valid)
# from the end of the 100 ms DSD lead-in on (the virtual cable may drop a few ms while its stream starts; the
# lead-in silence is there for exactly that, on a real DAC its lock time)
first = idx[0] + int(0.12 * 176400)
seg = mk[first:idx[-1] + 1]
alternating = bool(np.all(seg[1:] != seg[:-1])) and bool(np.all((seg == 0x05) | (seg == 0xFA)))
lr = int(np.count_nonzero(((u[first:idx[-1] + 1, 0] >> 16) & 0xFF) != ((u[first:idx[-1] + 1, 1] >> 16) & 0xFF)))
check(f"DoP markers alternate over {len(seg) / 176400:.2f} s, L/R marker mismatches {lr}", alternating and lr == 0 and "underruns 0" in log)

print("\nall passed" if not fails else f"\n{fails} FAILED")
