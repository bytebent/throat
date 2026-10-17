// Checks that the C++ engine (source/ThroatDSP.h) sounds the same as the JS reference.
// Run: g++ -O2 -std=c++17 -ffp-contract=off -o dsp_cli tests/dsp_cli.cpp && node tests/compare.js ./dsp_cli
'use strict';
const fs = require('fs'), os = require('os'), path = require('path'), { execFileSync } = require('child_process');
const { ThroatEngine } = require(path.join(__dirname, '..', 'reference', 'throat-engine.js'));
const cli = path.resolve(process.argv[2] || './dsp_cli');

function runJs(script) {
  const lines = script.trim().split('\n'), head = lines[0].split(/\s+/), h = {};
  for (let i = 0; i < head.length; i += 2) h[head[i]] = +head[i + 1];
  const e = new ThroatEngine(h.sr, h.seed), out = [];
  for (const l of lines.slice(1)) {
    const a = l.trim().split(/\s+/);
    if (a[0] === 'p') e.setParam(a[1], +a[2]);
    else if (a[0] === 'on') e.noteOn(+a[1], +a[2]);
    else if (a[0] === 'off') e.noteOff(+a[1]);
    else if (a[0] === 'panic') e.allNotesOff();
    else if (a[0] === 'run') {
      let left = +a[1];
      while (left > 0) { const m = Math.min(h.block, left), b = new Float32Array(m); e.process(b, m); out.push(b); left -= m; }
    }
  }
  const n = out.reduce((s, b) => s + b.length, 0), r = new Float32Array(n);
  let o = 0; for (const b of out) { r.set(b, o); o += b.length; }
  return r;
}
function runCpp(script) {
  const f = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'throat-')), 's.txt');
  fs.writeFileSync(f, script);
  const buf = execFileSync(cli, [f], { maxBuffer: 1 << 30 });
  return new Float32Array(buf.buffer, buf.byteOffset, buf.length / 4);
}

const S = 48000;
const sec = s => Math.round(s * S);
function script(head, steps) { return head + '\n' + steps.join('\n') + '\n'; }
const H = (sr = 48000, block = 128, seed = 1234) => `sr ${sr} block ${block} seed ${seed}`;
const knobs = ['voicing', 'breathy', 'tense', 'creaky', 'thin', 'growl', 'jitter', 'shimmer'];
const cases = [];
cases.push(['modal note, flow', script(H(), ['on 48 100', `run ${sec(0.6)}`, 'off 48', `run ${sec(0.5)}`])]);
cases.push(['modal note, derivative, high', script(H(), ['p source 1', 'on 84 127', `run ${sec(0.5)}`, 'off 84', `run ${sec(0.4)}`])]);
for (const k of knobs) cases.push([k + ' extreme', script(H(), [`p ${k} ${k === 'voicing' ? 0 : 1}`, 'p source 1', 'on 45 90', `run ${sec(0.5)}`, 'off 45', `run ${sec(0.3)}`])]);
cases.push(['everything on, flow', script(H(), knobs.filter(k => k !== 'voicing').map(k => `p ${k} 1`).concat(['p voicing 0.6', 'on 40 100', `run ${sec(0.8)}`, 'off 40', `run ${sec(0.4)}`]))]);
cases.push(['knob moves while playing', script(H(), ['on 52 100', `run ${sec(0.2)}`, 'p tense 1', `run ${sec(0.1)}`, 'p breathy 0.7', `run ${sec(0.1)}`,
  'p creaky 0.5', `run ${sec(0.1)}`, 'p growl 0.8', `run ${sec(0.1)}`, 'p voicing 0.2', `run ${sec(0.1)}`, 'p source 1', `run ${sec(0.1)}`,
  'p thin 1', 'p transpose 5', 'p detune -40', 'p volume 3', `run ${sec(0.2)}`, 'off 52', `run ${sec(0.4)}`])]);
cases.push(['mono glide and fallback', script(H(), ['p glide 0.15', 'on 48 100', `run ${sec(0.3)}`, 'on 55 80', `run ${sec(0.3)}`, 'on 60 110', `run ${sec(0.2)}`,
  'off 60', `run ${sec(0.2)}`, 'off 55', `run ${sec(0.2)}`, 'off 48', `run ${sec(0.4)}`, 'on 43 100', `run ${sec(0.3)}`, 'off 43', `run ${sec(0.3)}`])]);
cases.push(['poly chord with stealing', script(H(), ['p mode 1', 'p breathy 0.3', 'p attack 0.005', 'p release 0.6'].concat(
  [36, 43, 48, 52, 55, 59, 62, 64].map(n => `on ${n} 100`), [`run ${sec(0.3)}`, 'on 67 90', 'on 71 70', `run ${sec(0.2)}`, 'off 36', 'off 67', `run ${sec(0.2)}`, 'panic', `run ${sec(0.8)}`]))]);
cases.push(['mode switch while playing', script(H(), ['on 50 100', `run ${sec(0.2)}`, 'p mode 1', 'on 57 100', 'on 62 100', `run ${sec(0.3)}`, 'p mode 0', 'on 45 100', `run ${sec(0.3)}`, 'off 45', `run ${sec(0.3)}`])]);
cases.push(['odd host block size (37)', script(H(48000, 37), ['p growl 0.6', 'p creaky 0.5', 'p jitter 0.5', 'on 45 100', `run ${sec(0.5)}`, 'off 45', `run ${sec(0.3)}`])]);
cases.push(['44.1 kHz', script(H(44100, 512), ['p tense 0.7', 'p breathy 0.4', 'on 60 100', `run 26460`, 'off 60', `run 13230`])]);
cases.push(['96 kHz', script(H(96000, 64), ['p thin 0.8', 'p shimmer 0.5', 'on 69 100', `run 57600`, 'off 69', `run 28800`])]);
cases.push(['envelope extremes', script(H(), ['p attack 0.001', 'p decay 0.001', 'p sustain 0', 'p release 0.001', 'on 48 127', `run ${sec(0.2)}`, 'off 48',
  'p attack 2', 'p sustain 1', 'on 48 1', `run ${sec(0.5)}`, 'off 48', `run ${sec(0.2)}`])]);

let fails = 0;
for (const [name, sc] of cases) {
  const a = runJs(sc), b = runCpp(sc);
  let maxd = 0, num = 0, den = 0;
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) { const d = a[i] - b[i]; maxd = Math.max(maxd, Math.abs(d)); num += d * d; den += a[i] * a[i]; }
  const resid = den > 0 ? 10 * Math.log10(num / den + 1e-30) : -999;
  const audible = den / n > 1e-10;   // guard against comparing silence
  const ok = audible && a.length === b.length && maxd < 1e-4 && resid < -90;
  if (!ok) fails++;
  console.log((ok ? 'PASS ' : 'FAIL ') + name.padEnd(30) + ' max diff ' + maxd.toExponential(1) + '  residual ' + resid.toFixed(0) + ' dB' + (a.length !== b.length ? '  LENGTH ' + a.length + ' vs ' + b.length : ''));
}
console.log(fails ? fails + ' FAILED' : 'C++ matches the reference');
process.exit(fails ? 1 : 0);
