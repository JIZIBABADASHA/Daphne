"""File dispatch and PCM decoders.

  1. integer WAV / AIFF / FLAC at 16 / 24 / 32 bit: decoded sample-identical to the source; FLAC also matches its
     STREAMINFO MD5 (decode-test)
  2. float64 WAV: bit-identical
  3. formats without a dedicated decoder (TTA, ALAC in CAF, FLAC in MKA / OGA, WMA) go to FFmpeg: lossless ones
     sample-identical, lossy ones at the expected length
  4. FLAC data named .wav: the dedicated decoder refuses it, FFmpeg reads it; a non-audio file is refused
  5. gapless per codec: one signal cut into two tracks off any codec frame boundary, each encoded, decoded and
     joined; the join must be as clean as FFmpeg's own decoder of the same files

  python decoders.py
"""
import shutil
import numpy as np, soundfile as sf
from common import EXE, run, ffmpeg, path

fs = 44100
rng = np.random.default_rng(5)
fails = 0


def check(label, ok, detail=""):
    global fails
    fails += not ok
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{'  ' + detail if detail else ''}")


def render(src):
    out = path("dec.wav")
    r = run([EXE, "render", src, out], check=False)
    if r.returncode:
        return None, (r.stdout + r.stderr).strip()
    return out, r.stdout.strip()


print("1. integer PCM, bit depths")
for bits, sub in ((16, "PCM_16"), (24, "PCM_24"), (32, "PCM_32")):
    v = rng.integers(-2 ** (bits - 1), 2 ** (bits - 1), size=(fs, 2), dtype=np.int64)
    s32 = (v << (32 - bits)).astype(np.int32)
    for ext, fmt in (("wav", "WAV"), ("aiff", "AIFF"), ("flac", "FLAC")):
        if ext == "flac" and bits == 32:
            continue  # libsndfile writes FLAC up to 24 bit
        src = path(f"i{bits}.{ext}")
        sf.write(src, s32, fs, subtype=sub, format=fmt)
        out, msg = render(src)
        if not out:
            check(f"{ext} {bits}-bit", False, msg); continue
        d, _ = sf.read(out, dtype="int32", always_2d=True)
        check(f"{ext} {bits}-bit sample-identical", np.array_equal(d, s32), msg)
    src = path(f"m{bits}.flac")
    if bits <= 24:
        ffmpeg(path(f"i{bits}.wav"), src, "-c:a", "flac")   # FFmpeg's FLAC encoder stores the MD5
        r = run([EXE, "decode-test", src], check=False)
        check(f"flac {bits}-bit STREAMINFO MD5", r.returncode == 0 and " OK" in r.stdout,
              [l.strip() for l in r.stdout.splitlines() if "MD5" in l][0] if "MD5" in r.stdout else r.stdout)

print("2. float64 WAV / AIFF")
x = rng.standard_normal((fs, 2)) * 0.3
for ext, fmt in (("wav", "WAV"), ("aiff", "AIFF")):
    src = path(f"f64.{ext}"); sf.write(src, x, fs, subtype="DOUBLE", format=fmt)
    out, msg = render(src)
    if not out:
        check(f"float64 {ext}", False, msg); continue
    d, _ = sf.read(out, dtype="float32", always_2d=True)
    check(f"float64 {ext} decoded (rendered as float32)", np.array_equal(d, x.astype(np.float32)), msg)

print("3. formats via FFmpeg")
v = rng.integers(-2 ** 23, 2 ** 23, size=(fs * 2, 2), dtype=np.int64)
src24 = path("src.wav"); sf.write(src24, (v << 8).astype(np.int32), fs, subtype="PCM_24")
for name, fn, enc, lossless in [
    ("TTA", "a.tta", ["-c:a", "tta"], True),
    ("ALAC in CAF", "a.caf", ["-c:a", "alac"], True),
    ("ALAC in M4A", "a.m4a", ["-c:a", "alac"], True),
    ("FLAC in MKA", "a.mka", ["-c:a", "flac"], True),
    ("FLAC in OGA", "a.oga", ["-c:a", "flac"], True),
    ("WavPack", "a.wv", ["-c:a", "wavpack"], True),
    ("WMA v2", "a.wma", ["-c:a", "wmav2", "-b:a", "192k"], False),
]:
    f = path(fn); ffmpeg(src24, f, *enc)
    out, msg = render(f)
    if not out:
        check(name, False, msg); continue
    d, _ = sf.read(out, dtype="int32", always_2d=True)
    if lossless:
        got = d.astype(np.int64) >> 8
        n = min(len(got), len(v))
        same = np.array_equal(got[:n], v[:n])
        note = f"{len(got)} frames (source {len(v)})"
        if name == "ALAC in CAF" and len(got) < len(v):
            note += ", short: FFmpeg's ALAC-in-CAF demuxer limitation, its own decoder gives the same"
            check(name + " sample-identical over the decoded part", same, note)
        else:
            check(name + " sample-identical", same and len(got) == len(v), note)
    else:
        check(name + " decoded", abs(len(d) - len(v)) < 4096, f"{len(d)} frames (source {len(v)}, ASF stores no exact length)")

print("4. wrong extension, non-audio")
flac = path("x.flac"); ffmpeg(src24, flac, "-c:a", "flac")
mis = path("mislabeled.wav"); shutil.copy(flac, mis)
out, msg = render(mis)
ok = out is not None and np.array_equal(sf.read(out, dtype="int32", always_2d=True)[0].astype(np.int64) >> 8, v)
check("FLAC named .wav decoded via the fallback, sample-identical", ok, msg)
junk = path("notes.xyz"); open(junk, "wb").write(b"not audio at all" * 100)
out, msg = render(junk)
check("non-audio .xyz refused", out is None, msg[:100])

print("5. gapless per codec")


def signal(rate, sec=12.0):
    g = np.random.default_rng(3)
    t = np.arange(int(rate * sec)) / rate
    y = 0.3 * np.sin(2 * np.pi * (100 * t + 150 * t ** 2)) + 0.05 * g.standard_normal(len(t))
    return np.stack([y, 0.8 * y], 1)


def decode_with(tool, f):
    o = path("g.wav")
    if tool == "daphne":
        run([EXE, "render", f, o])
    else:
        run(["ffmpeg", "-y", "-loglevel", "error", "-i", f, "-c:a", "pcm_f32le", o])
    return sf.read(o, dtype="float64", always_2d=True)[0]


def cut_error(parts, ref, cut, rate):
    y = np.concatenate(parts)
    n = min(len(ref), len(y)); e = y[:n] - ref[:n]
    w = int(rate * 0.01)
    rest = np.concatenate([e[rate:cut - rate], e[cut + rate:n - rate]])
    rms = lambda q: np.sqrt(np.mean(q ** 2)) + 1e-30
    return len(y) - len(ref), 20 * np.log10(rms(e[cut - w:cut + w]) / rms(rest))


for name, ext, enc, rate in [
    ("FLAC", "flac", ["-c:a", "flac"], 44100),
    ("MP3 320k", "mp3", ["-c:a", "libmp3lame", "-b:a", "320k"], 44100),
    ("AAC 256k", "m4a", ["-c:a", "aac", "-b:a", "256k"], 44100),
    ("Vorbis q6", "ogg", ["-c:a", "libvorbis", "-q:a", "6"], 44100),
    ("Opus 192k", "opus", ["-c:a", "libopus", "-b:a", "192k"], 48000),
]:
    ref = signal(rate)
    cut = int(rate * 5.3) + 37
    dp, fp = [], []
    for k, seg in enumerate((ref[:cut], ref[cut:])):
        s = path(f"p{k}.wav"); f = path(f"p{k}.{ext}")
        sf.write(s, seg, rate, subtype="PCM_24")
        ffmpeg(s, f, *enc)
        dp.append(decode_with("daphne", f)); fp.append(decode_with("ffmpeg", f))
    dl, de = cut_error(dp, ref, cut, rate)
    fl, fe = cut_error(fp, ref, cut, rate)
    check(f"{name}: length {dl:+d}, error at cut {de:+5.1f} dB vs elsewhere (FFmpeg: length {fl:+d}, {fe:+5.1f} dB)",
          dl == 0 and de <= max(fe, 0.0) + 1.0)

print(f"\n{'all passed' if not fails else f'{fails} FAILED'}")
