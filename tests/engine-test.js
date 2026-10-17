// Offline tests for the Throat engine. Run: node tests/engine-test.js [--wav outdir]
'use strict';
const path = require('path');
const fs = require('fs');
const { ThroatEngine } = require(path.join(__dirname, '..', 'reference', 'throat-engine.js'));

const SR = 48000;
const args = process.argv.slice(2);
const wavDir = args[0] === '--wav' ? args[1] : null;
let failures = 0;
function check(name, ok, info) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? '  ' + info : ''));
  if (!ok) failures++;
}

// render a list of events [{t, on|off, note, vel}] plus param changes [{t, id, v}]
function render(params, events, seconds, opts) {
  const e = new ThroatEngine(opts && opts.sr || SR, 1234);
  for (const k in params) e.setParam(k, params[k]);
  const sr = e.sr, n = Math.round(seconds * sr), out = new Float32Array(n);
  const ev = events.slice().sort((a, b) => a.t - b.t);
  const blk = (opts && opts.block) || 128;
  let i = 0, k = 0;
  while (i < n) {
    while (k < ev.length && Math.round(ev[k].t * sr) <= i) {
      const x = ev[k++];
      if (x.on != null) e.noteOn(x.on, x.vel || 100);
      else if (x.off != null) e.noteOff(x.off);
      else if (x.id) e.setParam(x.id, x.v);
    }
    let m = Math.min(blk, n - i);
    if (k < ev.length) m = Math.min(m, Math.max(1, Math.round(ev[k].t * sr) - i));
    e.process(out.subarray(i, i + m), m);
    i += m;
  }
  return { out, e };
}
const rms = (a, s, t) => { s = s || 0; t = t || a.length; let x = 0; for (let i = s; i < t; i++) x += a[i] * a[i]; return Math.sqrt(x / (t - s)); };
const peak = a => { let p = 0; for (const v of a) p = Math.max(p, Math.abs(v)); return p; };
const finite = a => { for (const v of a) if (!Number.isFinite(v)) return false; return true; };
const db = x => 20 * Math.log10(Math.max(x, 1e-12));
const mtof = m => 440 * Math.pow(2, (m - 69) / 12);

function writeWav(name, a, sr) {
  if (!wavDir) return;
  fs.mkdirSync(wavDir, { recursive: true });
  const b = Buffer.alloc(44 + a.length * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + a.length * 2, 4); b.write('WAVE', 8); b.write('fmt ', 12);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(sr, 24);
  b.writeUInt32LE(sr * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write('data', 36);
  b.writeUInt32LE(a.length * 2, 40);
  for (let i = 0; i < a.length; i++) b.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(a[i] * 32767))), 44 + i * 2);
  fs.writeFileSync(path.join(wavDir, name + '.wav'), b);
}

// single-bin DFT magnitude (Goertzel-ish) over a segment
function toneMag(a, s, n, f, sr) {
  let re = 0, im = 0;
  for (let i = 0; i < n; i++) {
    const w = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1));
    const ph = 2 * Math.PI * f * i / sr;
    re += a[s + i] * w * Math.cos(ph); im -= a[s + i] * w * Math.sin(ph);
  }
  return Math.hypot(re, im) / (n / 4);
}

const hold = (note, len) => [{ t: 0.05, on: note }, { t: 0.05 + len, off: note }];
const base = { attack: 0.01, decay: 0.1, sustain: 1, release: 0.1, volume: 0 };

// 1. sanity across every knob at its extremes, both sources
const knobs = ['voicing', 'breathy', 'tense', 'creaky', 'thin', 'growl', 'jitter', 'shimmer'];
const cases = [{}];
for (const k of knobs) cases.push({ [k]: k === 'voicing' ? 0 : 1 });
cases.push({ breathy: 1, tense: 1, creaky: 1, thin: 1, growl: 1, jitter: 1, shimmer: 1 });
cases.push({ voicing: 0.5, breathy: 0.5, tense: 0.5 });
let allFinite = true, maxPeak = 0;
const levels = [];
for (const src of [0, 1]) for (const c of cases) for (const note of [36, 48, 60, 72, 84]) {
  const { out } = render(Object.assign({}, base, c, { source: src }), hold(note, 0.5), 0.8);
  if (!finite(out)) allFinite = false;
  maxPeak = Math.max(maxPeak, peak(out));
  if (note === 48) levels.push([src ? 'deriv' : 'flow ', JSON.stringify(c), db(rms(out, 9600, 26000)).toFixed(1)]);
}
check('all knob extremes and pitches produce finite audio', allFinite);
check('no clipping at 0 dB volume, one voice', maxPeak < 0.99, 'peak ' + maxPeak.toFixed(3));
console.log('  level at C2 (dB RMS):');
for (const l of levels) console.log('   ', l[0], l[2].padStart(6), l[1]);

// 2. modal voice is perfectly periodic with every irregularity knob at zero
{
  const { out } = render(Object.assign({}, base, { source: 1 }), hold(48, 1), 1.2);
  const T = SR / mtof(48);
  const s = 24000, len = Math.round(T * 10);
  let diff = 0, ref = 0;
  // compare 10 periods to the 10 periods 20 periods later (period is non-integer: use fractional delay)
  const lag = T * 20;
  for (let i = 0; i < len; i++) {
    const x = s + i + lag, i0 = Math.floor(x), fr = x - i0;
    const d = out[i0] + (out[i0 + 1] - out[i0]) * fr;
    diff += (out[s + i] - d) ** 2; ref += out[s + i] ** 2;
  }
  check('steady modal voice repeats exactly (no hidden irregularity)', db(Math.sqrt(diff / ref)) < -40, db(Math.sqrt(diff / ref)).toFixed(1) + ' dB residual');
}

// 3. pitch: fundamental lands on the played note, transpose and detune shift it
function f0Of(out, s, n, expect) {
  let best = 0, bf = 0;
  for (let c = -60; c <= 60; c += 1) {
    const f = expect * Math.pow(2, c / 1200), m = toneMag(out, s, n, f, SR);
    if (m > best) { best = m; bf = f; }
  }
  return bf;
}
{
  const r1 = render(Object.assign({}, base), hold(57, 1), 1.1).out;
  const f = f0Of(r1, 12000, 24000, 220);
  check('A2 plays at 220 Hz', Math.abs(1200 * Math.log2(f / 220)) <= 2, f.toFixed(2) + ' Hz');
  const r2 = render(Object.assign({}, base, { transpose: 7, detune: -30 }), hold(57, 1), 1.1).out;
  const expect = 220 * Math.pow(2, (7 - 0.3) / 12);
  const f2 = f0Of(r2, 12000, 24000, expect);
  check('transpose +7 and detune −30 cents', Math.abs(1200 * Math.log2(f2 / expect)) <= 2, f2.toFixed(2) + ' Hz vs ' + expect.toFixed(2));
}

// 4. aliasing: tense voice at a high note, energy between harmonics
{
  const note = 84, f0 = mtof(note);
  const { out } = render(Object.assign({}, base, { tense: 1, source: 1 }), hold(note, 1), 1.1);
  const s = 12000, n = 32768;
  // power spectrum by brute DFT on a grid is too slow; use a radix-2 FFT
  const re = new Float64Array(n), im = new Float64Array(n);
  for (let i = 0; i < n; i++) re[i] = out[s + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1)));
  fft(re, im);
  let harm = 0, other = 0;
  for (let k = 1; k < n / 2; k++) {
    const f = k * SR / n;
    if (f > 20000) break;
    const p = re[k] * re[k] + im[k] * im[k];
    const h = f / f0, dist = Math.abs(h - Math.round(h)) * f0;
    if (dist < 25) harm += p; else if (dist > 60) other += p;
  }
  check('tense C5: inharmonic (aliased) energy is tiny', db(Math.sqrt(other / harm)) < -60, db(Math.sqrt(other / harm)).toFixed(1) + ' dB below harmonics');
}

// 5. clicks: note on/off and big knob jumps while playing
function maxStep(a, s, t) { let m = 0; for (let i = s + 1; i < t; i++) m = Math.max(m, Math.abs(a[i] - a[i - 1])); return m; }
function envJump(a, s, t, w) {   // largest change between consecutive short-window RMS values (dB)
  let prev = null, m = 0;
  for (let i = s; i + w <= t; i += w) { const r = db(rms(a, i, i + w)); if (prev !== null) m = Math.max(m, Math.abs(r - prev)); prev = r; }
  return m;
}
{
  // voiced only (no noise): every jump should be no rougher than the end state played steadily
  const ev = hold(48, 0.6).concat([
    { t: 0.2, id: 'tense', v: 1 }, { t: 0.28, id: 'creaky', v: 1 }, { t: 0.36, id: 'growl', v: 1 },
    { t: 0.44, id: 'source', v: 1 }, { t: 0.5, id: 'volume', v: 6 }, { t: 0.55, id: 'volume', v: -20 }]);
  const { out } = render(Object.assign({}, base, { attack: 0.005, release: 0.01 }), ev, 0.9);
  const endState = render(Object.assign({}, base, { tense: 1, creaky: 1, growl: 1, source: 1, volume: 6 }), hold(48, 0.6), 0.9).out;
  const ms = maxStep(out, 0, out.length), ref = maxStep(endState, 4800, 26000);
  check('voiced knob jumps and fast envelopes: no clicks', ms <= ref * 1.05 + 0.005, 'max step ' + ms.toFixed(3) + ' vs steady end state ' + ref.toFixed(3));
  // with noise: loudness moves smoothly (20 ms windows) when voicing and breath are switched hard
  const ev2 = hold(48, 0.6).concat([{ t: 0.2, id: 'breathy', v: 1 }, { t: 0.3, id: 'voicing', v: 0 }, { t: 0.4, id: 'tense', v: 1 }, { t: 0.5, id: 'voicing', v: 1 }]);
  const o2 = render(Object.assign({}, base), ev2, 0.9).out;
  const j2 = envJump(o2, 4800, Math.round(0.64 * SR), 960);
  check('breath and voicing switches: level glides, no jumps', j2 < 4, 'largest 20 ms step ' + j2.toFixed(1) + ' dB');
  const tail = out.subarray(Math.round(0.8 * SR));
  check('release ends in silence', peak(tail) < 1e-3, 'tail peak ' + peak(tail).toExponential(1));
}

// 6. mono: legato glide, fallback to held note, poly: chord of 8
{
  const p = Object.assign({}, base, { mode: 0, glide: 0.1 });
  const ev = [{ t: 0.05, on: 48 }, { t: 0.4, on: 55 }, { t: 0.8, off: 55 }, { t: 1.2, off: 48 }];
  const { out } = render(p, ev, 1.4);
  const mid = f0Of(out, Math.round(0.6 * SR), 12000, mtof(55));
  const back = f0Of(out, Math.round(0.98 * SR), 9600, mtof(48));
  check('mono: second key glides up to G2', Math.abs(1200 * Math.log2(mid / mtof(55))) <= 3, mid.toFixed(1) + ' Hz');
  check('mono: releasing it falls back to the held C2', Math.abs(1200 * Math.log2(back / mtof(48))) <= 3, back.toFixed(1) + ' Hz');
  const early = f0Of(out, Math.round(0.405 * SR), 2400, mtof(51));
  check('mono: pitch is between notes during the glide', early > mtof(48) * 1.01 && early < mtof(55) * 0.99, early.toFixed(1) + ' Hz');
  const chord = [48, 52, 55, 59, 62, 64, 67, 71];
  const ev2 = chord.map(nn => ({ t: 0.05, on: nn })).concat(chord.map(nn => ({ t: 0.7, off: nn })));
  const r = render(Object.assign({}, base, { mode: 1, volume: -6 }), ev2, 1.0);
  const active = r.e.voices.filter(v => v.stage !== 0).length;
  check('poly: 8-note chord is finite and releases', finite(r.out) && active === 0, 'peak ' + peak(r.out).toFixed(2));
  const ev3 = ev2.concat([{ t: 0.3, on: 74 }]);
  const r3 = render(Object.assign({}, base, { mode: 1, volume: -6 }), ev3, 1.0);
  check('poly: 9th note steals a voice without breaking', finite(r3.out));
}

// 7. buffer size independence
{
  const p = Object.assign({}, base, { growl: 0.6, creaky: 0.5, jitter: 0.5, breathy: 0.4 });
  const a = render(p, hold(45, 0.6), 0.8, { block: 128 }).out;
  const b = render(p, hold(45, 0.6), 0.8, { block: 37 }).out;
  let d = 0; for (let i = 0; i < a.length; i++) d = Math.max(d, Math.abs(a[i] - b[i]));
  check('same sound at host block sizes 128 and 37', d < 1e-6, 'max diff ' + d.toExponential(1));
}

// 8. CPU: 8 voices with everything on, real-time factor
{
  const p = Object.assign({}, base, { mode: 1, breathy: 0.5, growl: 0.5, creaky: 0.3, thin: 0.3, jitter: 0.3, shimmer: 0.3 });
  const chord = [36, 43, 48, 52, 55, 60, 64, 67];
  const ev = chord.map(nn => ({ t: 0, on: nn }));
  const t0 = process.hrtime.bigint();
  render(p, ev, 5);
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  check('8 voices, all knobs on: fast enough for real time', ms / 5000 < 0.25, (ms / 5000 * 100).toFixed(1) + '% of real time');
}

// 9. sample rates
for (const sr of [44100, 96000]) {
  const { out } = render(Object.assign({}, base, { tense: 1 }), hold(60, 0.4), 0.6, { sr });
  check('works at ' + sr + ' Hz', finite(out) && peak(out) > 0.01 && peak(out) < 1);
}

// listening renders (for spectrogram inspection)
if (wavDir) {
  const phr = [];
  const demos = {
    modal: {}, breathy: { breathy: 1 }, tense: { tense: 1 }, creaky: { creaky: 1 }, thin: { thin: 1 },
    growl: { growl: 1 }, whisper: { voicing: 0, tense: 1 }, h_breath: { voicing: 0, breathy: 1 }, jitter: { jitter: 1, shimmer: 1 },
  };
  for (const name in demos) for (const src of [0, 1]) {
    const note = name === 'creaky' ? 41 : name === 'thin' ? 64 : 48;
    const { out } = render(Object.assign({}, base, demos[name], { source: src }), hold(note, 1.5), 1.8);
    writeWav(name + (src ? '_deriv' : '_flow'), out, SR);
  }
}

console.log(failures ? failures + ' FAILED' : 'all passed');
process.exit(failures ? 1 : 0);

function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let j = 0; j < len / 2; j++) {
        const ur = re[i + j], ui = im[i + j];
        const vr = re[i + j + len / 2] * cr - im[i + j + len / 2] * ci;
        const vi = re[i + j + len / 2] * ci + im[i + j + len / 2] * cr;
        re[i + j] = ur + vr; im[i + j] = ui + vi;
        re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}
