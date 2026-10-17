// Offline tests for the bench's test filters. Run: node tests/filter-test.js
'use strict';
const path = require('path');
const { ThroatEngine } = require(path.join(__dirname, '..', 'reference', 'throat-engine.js'));
const { TestFilters } = require(path.join(__dirname, '..', 'tools', 'bench', 'test-filters.js'));
let fails = 0;
const check = (n, ok, i) => { console.log((ok ? 'PASS ' : 'FAIL ') + n + (i ? '  ' + i : '')); if (!ok) fails++; };
const SR = 48000;
function run(setup, events, sec) {
  const e = new ThroatEngine(SR, 5), f = new TestFilters(SR);
  e.setParam('volume', 0); e.setParam('sustain', 1); setup(e, f);
  e.noteOn(48, 100);
  const out = new Float32Array(SR * sec);
  for (let i = 0; i < out.length; i += 128) {
    for (const ev of events) if (Math.round(ev.t * SR / 128) * 128 === i) f.set(ev.id, ev.v);
    const s = out.subarray(i, i + 128); e.process(s, s.length); f.process(s, s.length);
  }
  return out;
}
const finite = a => a.every(Number.isFinite);
const peak = a => a.reduce((p, v) => Math.max(p, Math.abs(v)), 0);
const step = (a, s, t) => { let m = 0; for (let i = s + 1; i < t; i++) m = Math.max(m, Math.abs(a[i] - a[i - 1])); return m; };
// toggles and sweeps while playing: no blow-ups, no clicks beyond the dry signal's own steps
for (const src of [0, 1]) {
  const ev = [{ t: 0.2, id: 'vowelOn', v: 1 }, { t: 0.4, id: 'vowel', v: 1 }, { t: 0.6, id: 'lpOn', v: 1 }, { t: 0.7, id: 'cutoff', v: 300 },
    { t: 0.8, id: 'res', v: 1 }, { t: 0.9, id: 'cutoff', v: 12000 }, { t: 1.0, id: 'vowelOn', v: 0 }, { t: 1.1, id: 'lpOn', v: 0 }];
  const o = run(e => e.setParam('source', src), ev, 1.3);
  const dry = run(e => e.setParam('source', src), [], 1.3);
  check((src ? 'deriv' : 'flow') + ': toggles, slider and cutoff sweeps stay finite and in range', finite(o) && peak(o) < 1, 'peak ' + peak(o).toFixed(2));
  check((src ? 'deriv' : 'flow') + ': switching filters on/off does not click', step(o, 0, o.length) < Math.max(0.05, 2 * step(dry, 0, dry.length)),
    'max step ' + step(o, 0, o.length).toFixed(3) + ' (dry ' + step(dry, 0, dry.length).toFixed(3) + ')');
}
// everything off: bit-identical to the dry engine
{
  const a = run(() => {}, [], 0.5), b = run(() => {}, [{ t: 0.1, id: 'cutoff', v: 200 }], 0.5);
  check('filters off: output is the untouched engine signal', a.every((v, i) => v === b[i]));
}
console.log(fails ? fails + ' FAILED' : 'all passed'); process.exit(fails ? 1 : 0);
