"""Recording side of the live tests: the VB Hi-Fi Cable output (the player plays into "Hi-Fi Cable Input"),
WASAPI exclusive, int24 (stored left-justified in int32). The capture must open the cable as int24 (as int32
it only delivers 16 bits), and at most 176.4 kHz (above 192 kHz the cable resamples).
"""
import threading
import numpy as np
import sounddevice as sd


def cable_device():
    for i, d in enumerate(sd.query_devices()):
        if "Hi-Fi Cable Output" in d["name"] and sd.query_hostapis(d["hostapi"])["name"] == "Windows WASAPI" and d["max_input_channels"] >= 2:
            return i
    raise SystemExit("Hi-Fi Cable Output (WASAPI) not found")


class Recorder:
    """Records until the signal has started and then stayed silent for 3 s (or max_s)."""

    def __init__(self, fs, max_s=60):
        self.fs = fs
        self.buf = np.zeros((fs * max_s, 2), np.int32)
        self.n = 0
        self.started = False
        self.quiet_from = None
        self.done = threading.Event()

    def cb(self, indata, frames, t, status):
        m = min(frames, len(self.buf) - self.n)
        r = np.frombuffer(bytes(indata), np.uint8).reshape(-1, 2, 3)[:m]   # int24 -> left-justified int32
        self.buf[self.n:self.n + m] = ((r[:, :, 0].astype(np.int32) << 8) | (r[:, :, 1].astype(np.int32) << 16) |
                                       (r[:, :, 2].astype(np.int8).astype(np.int32) << 24))
        got = self.buf[self.n:self.n + m]
        self.n += m
        if np.abs(got[:, 0]).max(initial=0) > 2 ** 31 * 1e-4:
            self.started, self.quiet_from = True, None
        elif self.started and self.quiet_from is None:
            self.quiet_from = self.n
        if self.n >= len(self.buf) or (self.quiet_from is not None and self.n - self.quiet_from > 3 * self.fs):
            self.done.set()

    def record(self, start_player=None):
        with sd.RawInputStream(device=cable_device(), channels=2, samplerate=self.fs, dtype="int24", callback=self.cb,
                               extra_settings=sd.WasapiSettings(exclusive=True)):
            if start_player:
                start_player()
            self.done.wait()
        return self.buf[:self.n]
