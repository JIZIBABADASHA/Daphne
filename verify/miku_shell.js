// The macOS MIKU module (miku/extension.js) end to end, without MIKU: a stand-in host and settings object drive the
// engine as MIKU's AudioEngine facade does (mac/app/main/audio.js), on the real daphne-helper and output device.
//
//   node miku_shell.js [exclusive|shared]      (macOS; the helper is found in a build folder, or DAPHNE_HELPER)
//
// Two quiet (-60 dBFS) 1 s tracks and an 8 s one: loaded paused, the second as the gapless next track, played (trackStarted before
// ended); pause / resume / seek; stop releases the device; MIKU's EQ shows in the path card; Daphne's 2x upsampler
// (RPC set) doubles the device rate in exclusive mode and bitPerfect turns it off; exclusive: stop keeps the device and
// release() lets it go (no device in hog mode after it); the device list; dispose.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const mode = process.argv[2] === 'shared' ? 'coreaudio' : 'coreaudio-exclusive';
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'daphne_shell_'));

function tone(name, sec = 1, rate = 44100) {
  const n = Math.round(rate * sec), data = Buffer.alloc(n * 6);
  for (let i = 0; i < n; i++) {
    const v = Math.round(0.001 * 8388607 * Math.sin(2 * Math.PI * 1000 * i / rate));
    for (const off of [i * 6, i * 6 + 3]) data.writeIntLE(v, off, 3);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8); h.write('fmt ', 12);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(2, 22); h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 6, 28); h.writeUInt16LE(6, 32); h.writeUInt16LE(24, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  const p = path.join(work, name);
  fs.writeFileSync(p, Buffer.concat([h, data]));
  return p;
}

const track = (id, p) => ({ id, path: p, title: id, artist: '', album: '', codec: 'WAV', sampleRate: 44100, bits: 24, channels: 2, duration: 1 });
const A = track('a', tone('a.wav')), B = track('b', tone('b.wav'));
const C = { ...track('c', tone('c.wav', 8)), duration: 8 };   // long enough for the settings changes below

const stored = {}, posted = [];
const host = {
  log: m => console.log('    log: ' + m),
  getSetting: k => stored[k] ?? null,
  setSetting: (k, v) => { stored[k] = v; },
  post: (ev, data) => posted.push({ ev, data }),
  isDsd: t => /\.(dsf|dff)$/i.test(t.path || ''),
  isLossy: () => false,
  isLive: t => t && t.id === 'yt-live',
  replayGainDb: () => null,
  openPath: () => Promise.resolve(''),
};
const S = {
  outputMode: mode, deviceId: null, bufferMs: 100, fixedRate: 192000, dsdMode: 'pcm', maxDsd: 0, gapless: true,
  volumeMode: 'fixed', volumeDb: 0, muted: false, replayGain: 'off',
  dsp: { enabled: false, eqOn: true, preampDb: 0, autoPreamp: true, bands: [], presetName: '', crossfeed: { on: false, fc: 700, feed: 4.5 }, balance: 0, invert: false },
};

const fails = [];
const check = (ok, what) => { console.log((ok ? '  PASS  ' : '  FAIL  ') + what); if (!ok) fails.push(what); };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const until = async (f, ms) => { const end = Date.now() + ms; while (!f() && Date.now() < end) await sleep(20); return f(); };

(async () => {
  const ext = require(path.join(__dirname, '..', 'miku', 'extension.js'));
  ext.start(host);
  const e = ext.core.createEngine(S);
  const events = [];
  for (const ev of ['changed', 'loading', 'failed', 'trackStarted', 'ended']) e.on(ev, a => { if (ev !== 'changed') events.push(ev + (a && a.id ? ':' + a.id : '')); });
  e.peekNext = () => (e.track && e.track.id === 'a' ? B : null);   // MIKU's queue: B follows A, nothing follows B
  e.start();
  console.log(`mode: ${mode}`);

  await e.load(A, 0, false);
  check(e.isLoaded && !e.isPlaying, `loaded paused (loaded ${e.isLoaded}, playing ${e.isPlaying}, failed ${e.loadFailed})`);
  check(e.holdsDevice === (mode === 'coreaudio-exclusive'), `holdsDevice while loaded: ${e.holdsDevice}`);
  check(!!e.signal && !!e.signal.custom && e.signal.custom.sections.length === 3, `signal: ${e.signal && e.signal.mode}, ${e.signal && e.signal.outputFormat}, quality ${e.signal && e.signal.quality}`);
  e.resume();
  await until(() => events.includes('ended'), 10000);
  check(events.indexOf('trackStarted:b') >= 0 && events.indexOf('trackStarted:b') < events.indexOf('ended'), `events: ${events.join(', ')}`);
  check(!e.isLoaded, 'not loaded after the end');

  await e.load(C, 0, true);
  await sleep(300);
  e.pause();
  await sleep(300);   // the position comes with the state, every 100 ms
  const p1 = e.position;
  await sleep(300);
  check(Math.abs(e.position - p1) < 0.05 && !e.isPlaying, `paused at ${p1.toFixed(2)} s`);
  e.resume();
  await e.seek(0.5);
  await sleep(200);
  check(e.isPlaying && e.position >= 0.5, `resumed and seeked: ${e.position.toFixed(2)} s`);

  S.dsp.enabled = true;
  S.dsp.bands = [{ type: 'PK', fc: 1000, gain: -3, q: 1, on: true }];
  let changed = 0;
  e.on('changed', () => changed++);
  e.applyDsp();
  await until(() => e.signal && e.signal.dspSummary, 3000);
  check(e.signal && e.signal.dspSummary === 'EQ', `EQ in the path: ${e.signal && e.signal.dspSummary}`);
  S.dsp.enabled = false;
  e.applyDsp();

  let st = await ext.handleRpc('set', { upsampler: 'pcm', target: '2x' });
  check(st.options.upsampler === 'pcm' && stored.options && stored.options.upsampler === 'pcm', 'RPC set: options saved');
  if (mode === 'coreaudio-exclusive') {
    await until(() => e.report && e.report.device && e.report.device.rate === 88200, 8000);
    check(e.report && e.report.device.rate === 88200, `2x upsampling: device ${e.report && e.report.device.rate} Hz`);
  }
  st = await ext.handleRpc('bitPerfect');
  check(st.options.upsampler === 'off', 'RPC bitPerfect: upsampler off');
  check(!!st.version && st.cache && typeof st.cache.files === 'number', `RPC state: ${st.version}, commit ${st.commit}, cache ${st.cache && st.cache.files} files`);

  e.stop();
  await sleep(500);
  if (mode === 'coreaudio-exclusive') {
    const k = await e.client.request('state', {}, 2000);
    check(!e.isLoaded && e.holdsDevice && k.kept === true, `stopped: the device kept (holdsDevice ${e.holdsDevice}, kept ${k.kept})`);
    e.release();
    await sleep(2500);   // the rate put back (one relock), hog mode released
    const k2 = await e.client.request('state', {}, 2000);
    const hogged = (await e.coreAudioDevices()).filter(d => d.hog_pid >= 0).map(d => d.name);
    check(!e.holdsDevice && k2.kept === false && !hogged.length, `released: nothing held${hogged.length ? ' (' + hogged.join(', ') + ' in hog mode)' : ''}`);
  } else {
    check(!e.isLoaded && !e.holdsDevice, 'stopped: nothing held');
  }
  const list = await e.listDevices();
  check(list.length > 0, 'devices: ' + list.map(d => d.name + (d.isDefault ? ' (default)' : '')).join(', '));
  const probe = await e.probeDevice(null);
  check(probe && !probe.ownerName, `probe after stop: ${probe && probe.rate} Hz, ${probe && probe.physicalFormat}, held by ${probe && probe.ownerName || 'nobody'}`);

  const proc = e.client.proc;
  await e.dispose();
  check(!e.client.running && proc.exitCode === 0, `dispose: helper exit ${proc.exitCode}`);
  console.log(fails.length ? `\n${fails.length} failed` : '\nall passed');
  process.exit(fails.length ? 1 : 0);
})().catch(err => { console.error(err); process.exit(2); });
