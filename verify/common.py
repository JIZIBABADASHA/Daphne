"""Shared helpers for the verification scripts."""
import os, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Release", "daphne.exe")
WORK = tempfile.mkdtemp(prefix="daphne_verify_")


def run(args, check=True):
    r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode:
        raise SystemExit(" ".join(map(str, args)) + "\n" + r.stdout + r.stderr)
    return r


def ffmpeg(src, out, *enc):
    run(["ffmpeg", "-y", "-loglevel", "error", "-i", src, *enc, out])


def path(name):
    return os.path.join(WORK, name)
