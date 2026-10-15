'use strict';
// The daphne-helper child process (helper/main.cpp, N4): one JSON object per line each way. request() resolves with
// the reply of the same id; pushed messages ({"e": ...}) are emitted as 'event' / 'design' / 'hello'; 'exit' when the
// process ends (the engine starts a new one and continues where playback was).
const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { spawn } = require('child_process');
const { EventEmitter } = require('events');

/** The helper next to the module (an installed app: <module>/bin), or in a build folder of the checkout. */
function helperPath(moduleDir) {
  const exe = process.platform === 'win32' ? 'daphne-helper.exe' : 'daphne-helper';
  const root = path.join(moduleDir, '..');
  const candidates = [process.env.DAPHNE_HELPER, path.join(moduleDir, 'bin', exe)];
  try {
    for (const d of fs.readdirSync(root))
      if (/^build/.test(d)) candidates.push(path.join(root, d, 'bin', exe), path.join(root, d, 'bin', 'Release', exe));
  } catch { }
  return candidates.find(p => { if (!p) return false; try { fs.accessSync(p, fs.constants.X_OK); return true; } catch { return false; } }) || null;
}

class HelperClient extends EventEmitter {
  constructor(exe, log) {
    super();
    this.exe = exe; this.log = log || (() => { });
    this.proc = null; this.nextId = 1; this.waiters = new Map();
  }

  get running() { return !!this.proc; }

  start() {
    if (this.proc) return;
    const proc = this.proc = spawn(this.exe, [], { stdio: ['pipe', 'pipe', 'pipe'] });
    proc.stdin.on('error', () => { });
    readline.createInterface({ input: proc.stdout }).on('line', line => {
      if (this.proc !== proc) return;
      let m; try { m = JSON.parse(line); } catch { this.log('helper: not JSON: ' + line.slice(0, 200)); return; }
      if (m.id != null && this.waiters.has(m.id)) { const done = this.waiters.get(m.id); this.waiters.delete(m.id); done(m); }
      else if (m.e) this.emit(m.e, m);
    });
    let partial = '';
    proc.stderr.on('data', d => {
      const lines = (partial + String(d)).split('\n'); partial = lines.pop();
      for (const l of lines) if (l.trim()) this.log('helper: ' + l.slice(0, 2000));
    });
    const ended = why => {
      if (this.proc !== proc) return;
      this.proc = null;
      for (const done of this.waiters.values()) done({ ok: false, error: 'Daphne 的 helper 程序已停止', code: -100 });
      this.waiters.clear();
      this.emit('exit', why);
    };
    proc.on('error', e => ended(e.message));
    proc.on('exit', (code, signal) => ended(`exit ${code}${signal ? ' ' + signal : ''}`));
  }

  /** Sends a command; resolves with its reply ({ ok, ... }), or { ok: false } when the process is gone or the time is up. */
  request(cmd, args = {}, timeout = 0) {
    if (!this.proc || this.proc.stdin.destroyed) return Promise.resolve({ ok: false, error: 'Daphne 的 helper 程序沒有執行', code: -100 });
    const id = this.nextId++;
    return new Promise(resolve => {
      let timer = null;
      this.waiters.set(id, m => { if (timer) clearTimeout(timer); resolve(m); });
      if (timeout > 0) timer = setTimeout(() => { if (this.waiters.delete(id)) resolve({ ok: false, error: 'timeout', code: -101 }); }, timeout);
      this.proc.stdin.write(JSON.stringify({ id, cmd, ...args }) + '\n');
    });
  }

  /** quit, then the process ends; killed when it does not within the time. */
  async stop(timeout = 3000) {
    const proc = this.proc;
    if (!proc) return;
    const exited = new Promise(resolve => proc.once('exit', resolve));
    this.request('quit');
    const t = setTimeout(() => { try { proc.kill(); } catch { } }, timeout);
    await exited;
    clearTimeout(t);
  }
}

module.exports = { HelperClient, helperPath };
