"""daphne-helper's JSON-lines protocol (helper/main.cpp), end to end, on a real output device.

  python helper.py <daphne-helper> [settings JSON]

Two quiet (-60 dBFS) 1 s tracks at 44.1 kHz: loaded paused, the second set as the gapless next track, played:
the "track" event (the second track is heard) comes before "ended"; stop leaves the player idle (the device
released); a missing file fails with code -3 (DC_ERR_FILE); an unknown command is refused; quit ends the process.
The settings choose the device, e.g. '{"output": {"device": "0"}}' for the VB Hi-Fi Cable on Windows
(verify/live.py); the default is the platform's exclusive output on the system device.
"""
import json, math, os, queue, struct, subprocess, sys, tempfile, threading, time, wave

EXE = sys.argv[1]
SETTINGS = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
WORK = tempfile.mkdtemp(prefix="daphne_helper_")


def tone(name, sec=1.0, rate=44100):
    p = os.path.join(WORK, name)
    with wave.open(p, "wb") as w:
        w.setnchannels(2), w.setsampwidth(3), w.setframerate(rate)
        fr = bytearray()
        for i in range(int(rate * sec)):
            v = int(round(0.001 * 8388607 * math.sin(2 * math.pi * 1000 * i / rate)))
            b = v.to_bytes(3, "little", signed=True)
            fr += b + b
        w.writeframes(bytes(fr))
    return p


class Helper:
    def __init__(self):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.replies, self.pushed, self.next_id = {}, queue.Queue(), 1
        self.cv = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: [sys.stderr.write("helper: " + l.decode("utf-8", "replace")) for l in self.p.stderr], daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            m = json.loads(line)
            if "id" in m:
                with self.cv:
                    self.replies[m["id"]] = m
                    self.cv.notify_all()
            else:
                self.pushed.put(m)

    def send(self, cmd, **kw):
        i = self.next_id
        self.next_id += 1
        self.p.stdin.write((json.dumps({"id": i, "cmd": cmd, **kw}) + "\n").encode())
        self.p.stdin.flush()
        return i

    def call(self, cmd, timeout=30, **kw):
        i = self.send(cmd, **kw)
        with self.cv:
            if not self.cv.wait_for(lambda: i in self.replies, timeout):
                raise TimeoutError(cmd)
            return self.replies.pop(i)

    def wait_event(self, types, timeout):
        """pushed events until one of `types` arrives; returns the event types seen, in order"""
        seen, end = [], time.time() + timeout
        while time.time() < end:
            try:
                m = self.pushed.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if m.get("e") == "event":
                t = m["event"].get("type")
                seen.append(t)
                if t in types:
                    break
        return seen


fails = []


def check(ok, what):
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        fails.append(what)


a, b = tone("a.wav"), tone("b.wav")
h = Helper()
hello = h.pushed.get(timeout=10)
check(hello.get("e") == "hello" and hello.get("abi", 0) >= 2, f"hello: {hello}")
devs = h.call("devices")["devices"]
print("  devices: " + ", ".join(f"{k} {len(v)}" for k, v in devs.items()))

r = h.call("load", path=a, settings=SETTINGS, seek=0, play=False)
check(r["ok"], f"load paused: {r.get('error') or 'ok'}")
st = h.call("state")["state"]
check(st.get("state") == "paused", f"state after a paused load: {st.get('state')}, device {st.get('report', {}).get('device_name')}")
h.call("next", path=b)
h.call("pause", paused=False)
seen = h.wait_event({"ended", "failed"}, 10)
check("track" in seen and "ended" in seen and seen.index("track") < seen.index("ended"), f"events while playing: {seen}")
st = h.call("state")["state"]
check(st.get("state") == "ended" and st.get("track") == 1, f"state at the end: {st.get('state')}, track {st.get('track')}")

h.call("load", path=a, settings=SETTINGS, seek=0, play=True)
h.call("stop")
st = h.call("state")["state"]
check(st.get("state") == "idle", f"state after stop: {st.get('state')}")
ca = h.call("devices")["devices"].get("coreaudio", [])
if ca:   # macOS: stop lets go of the device (hog mode released, the rate put back)
    check(all(d.get("hog_pid", -1) == -1 for d in ca), "after stop no Core Audio device is held: " + ", ".join(f"{d['name']} {d['rate']} Hz hog {d['hog_pid']}" for d in ca))

r = h.call("load", path=os.path.join(WORK, "missing.wav"), settings=SETTINGS, seek=0, play=True)
check(not r["ok"] and r.get("code") == -3, f"missing file: ok {r['ok']}, code {r.get('code')}, {r.get('error')}")
r = h.call("frobnicate")
check(not r["ok"] and "unknown" in r.get("error", ""), f"unknown command: {r.get('error')}")

h.call("quit")
try:
    code = h.p.wait(timeout=5)
    check(code == 0, f"quit: exit code {code}")
except subprocess.TimeoutExpired:
    h.p.kill()
    check(False, "quit: the process did not exit")

print("\nall passed" if not fails else f"\n{len(fails)} failed")
sys.exit(1 if fails else 0)
