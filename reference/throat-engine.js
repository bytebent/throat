/*
 * Throat engine v0.1 · bytebent
 *
 * A parametric larynx. It makes every sound in the Throat/Mouth pair:
 *   - glottal pulses from the LF (Liljencrants–Fant) model, shaped by Rd
 *     (pressed ← 0.3 … 1.0 modal … 2.7 → breathy), output as flow or flow derivative,
 *   - breath / turbulence noise, steady and pulsed by each glottal opening,
 *   - per-cycle irregularity (jitter, shimmer, creak) and subharmonics (growl).
 *
 * Pulses and noise run at 4x the host rate and are decimated with a windowed-sinc
 * FIR, so sharp (tense, high) pulses don't alias. process() never allocates.
 *
 * Works in an AudioWorklet and in Node (module.exports) for offline tests.
 */
(function (root) {
  'use strict';

  const OS = 4;                       // internal oversampling
  const TN = 2048;                    // table points per glottal period
  const TW = TN + 1;                  // stored row width (wrap point)
  const MIP_LEVELS = 9;               // harmonic limits 1024, 512, … 4 (band-limited copies)
  const RD_LO = 0.3, RD_HI = 2.7, RD_STEP = 0.05;
  const RD_COUNT = Math.round((RD_HI - RD_LO) / RD_STEP) + 1;   // 49
  const RD_MODAL = 1.0;
  const RD_SUB = 0.45;                // false-fold (growl) pulse shape
  const TABLE_RMS = 0.1;              // pulse level at modal voice
  const NORM_POW = 0.7;               // how much loudness is evened out across Rd
  const FIR_LEN = 128;                // decimation filter taps (at 4x rate)
  const BLOCK = 32;                   // fixed internal control block (host samples) = latency
  const MAX_VOICES = 8;

  /* ---------- LF model ---------- */

  // Solve the LF model for one Rd value. Period T0 = 1, excitation Ee = 1.
  function lfSolve(Rd) {
    const Ra = (-1 + 4.8 * Rd) / 100;
    const Rk = (22.4 + 11.8 * Rd) / 100;
    const Rg = 0.25 * Rk / (0.11 * Rd / (0.5 + 1.2 * Rk) - Ra);
    const Tp = 1 / (2 * Rg), Te = Tp * (1 + Rk), Ta = Ra;
    const d = 1 - Te;
    let eps = 1 / Ta;
    for (let i = 0; i < 300; i++) eps = (1 - Math.exp(-eps * d)) / Ta;
    const wg = Math.PI / Tp, sinTe = Math.sin(wg * Te), cosTe = Math.cos(wg * Te);
    const ed = Math.exp(-eps * d);
    const retArea = -((1 - ed) / eps - d * ed) / (eps * Ta);
    const openArea = a => (a * sinTe - wg * cosTe + wg * Math.exp(-a * Te)) * (-1 / sinTe) / (a * a + wg * wg);
    // total area must be zero (flow returns to its starting value); f(a) decreases with a
    let lo = -200, hi = 200;
    for (let i = 0; i < 200; i++) {
      const mid = 0.5 * (lo + hi);
      if (openArea(mid) + retArea > 0) lo = mid; else hi = mid;
    }
    const a = 0.5 * (lo + hi);
    return { Rd, Tp, Te, Ta, eps, a, wg, sinTe, ed, flowTe: openArea(a), area: openArea(a) + retArea };
  }

  // Flow and flow derivative at normalised time t in [0, 1].
  function lfEval(p, t, out) {
    if (t <= p.Te) {
      const e = Math.exp(p.a * (t - p.Te));
      const s = Math.sin(p.wg * t), c = Math.cos(p.wg * t);
      out[1] = -e * s / p.sinTe;
      out[0] = (e * (p.a * s - p.wg * c) + p.wg * Math.exp(-p.a * p.Te)) * (-1 / p.sinTe) / (p.a * p.a + p.wg * p.wg);
    } else {
      const u = t - p.Te, eu = Math.exp(-p.eps * u);
      out[1] = -(eu - p.ed) / (p.eps * p.Ta);
      out[0] = p.flowTe - ((1 - eu) / p.eps - u * p.ed) / (p.eps * p.Ta);
    }
  }

  /* ---------- tables (built once) ---------- */

  let TABLES = null;

  function rmsOf(arr, off, n, removeMean) {
    let m = 0;
    if (removeMean) { for (let i = 0; i < n; i++) m += arr[off + i]; m /= n; }
    let s = 0;
    for (let i = 0; i < n; i++) { const v = arr[off + i] - m; s += v * v; }
    return Math.sqrt(s / n);
  }

  function buildTables() {
    if (TABLES) return TABLES;
    const flowRaw = new Float64Array(RD_COUNT * TW);
    const derivRaw = new Float64Array(RD_COUNT * TW);
    const flow = new Float32Array(RD_COUNT * TW);
    const deriv = new Float32Array(RD_COUNT * TW);
    const shape = new Float32Array(RD_COUNT * TW);     // flow scaled to peak 1 (drives pulsed breath)
    const te = new Float32Array(RD_COUNT);              // open quotient per row
    const tmp = [0, 0];
    for (let r = 0; r < RD_COUNT; r++) {
      const p = lfSolve(RD_LO + r * RD_STEP);
      te[r] = p.Te;
      const o = r * TW;
      for (let i = 0; i < TN; i++) {
        lfEval(p, i / TN, tmp);
        flowRaw[o + i] = tmp[0];
        derivRaw[o + i] = tmp[1];
      }
      // tiny numerical residue: pin the period end to the start
      const endFlow = (lfEval(p, 1, tmp), tmp[0]);
      for (let i = 0; i < TN; i++) flowRaw[o + i] -= endFlow * (i / TN);
      flowRaw[o + TN] = flowRaw[o];
      derivRaw[o + TN] = derivRaw[o];
    }
    const ref = Math.round((RD_MODAL - RD_LO) / RD_STEP);
    const refF = rmsOf(flowRaw, ref * TW, TN, true), refD = rmsOf(derivRaw, ref * TW, TN, false);
    const flowMip = [], derivMip = [];
    for (let l = 0; l < MIP_LEVELS; l++) { flowMip.push(new Float32Array(RD_COUNT * TW)); derivMip.push(new Float32Array(RD_COUNT * TW)); }
    const re = new Float64Array(TN), im = new Float64Array(TN), sre = new Float64Array(TN), sim = new Float64Array(TN);
    for (let r = 0; r < RD_COUNT; r++) {
      const o = r * TW;
      const rf = rmsOf(flowRaw, o, TN, true), rd = rmsOf(derivRaw, o, TN, false);
      const gf = (TABLE_RMS / refF) * Math.pow(refF / rf, NORM_POW);
      const gd = (TABLE_RMS / refD) * Math.pow(refD / rd, NORM_POW);
      let peak = 0;
      for (let i = 0; i < TW; i++) peak = Math.max(peak, flowRaw[o + i]);
      for (let i = 0; i < TW; i++) {
        flow[o + i] = flowRaw[o + i] * gf;
        deriv[o + i] = derivRaw[o + i] * gd;
        shape[o + i] = Math.max(0, flowRaw[o + i] / peak);
      }
      // band-limited copies: keep harmonics 1..H, H = 1024 >> level
      for (let kind = 0; kind < 2; kind++) {
        const src = kind ? deriv : flow, dst = kind ? derivMip : flowMip;
        for (let i = 0; i < TN; i++) { sre[i] = src[o + i]; sim[i] = 0; }
        fft(sre, sim, false);
        for (let l = 0; l < MIP_LEVELS; l++) {
          const H = (TN / 2) >> l;
          for (let i = 0; i < TN; i++) { re[i] = 0; im[i] = 0; }
          re[0] = sre[0];
          for (let k = 1; k <= H && k < TN / 2; k++) {
            re[k] = sre[k]; im[k] = sim[k]; re[TN - k] = sre[TN - k]; im[TN - k] = sim[TN - k];
          }
          if (H >= TN / 2) { re[TN / 2] = sre[TN / 2]; }
          fft(re, im, true);
          const d = dst[l];
          for (let i = 0; i < TN; i++) d[o + i] = re[i] / TN;
          d[o + TN] = d[o];
        }
      }
    }
    // Thin: an almost-sine pulse (raised cosine flow over the whole period)
    const sFlow = new Float32Array(TW), sDeriv = new Float32Array(TW), sShape = new Float32Array(TW);
    for (let i = 0; i < TW; i++) {
      const t = i / TN;
      sShape[i] = 0.5 * (1 - Math.cos(2 * Math.PI * t));
      sFlow[i] = sShape[i];
      sDeriv[i] = Math.PI * Math.sin(2 * Math.PI * t);
    }
    const thinLevel = 0.8 * TABLE_RMS;
    const g1 = thinLevel / rmsOf(sFlow, 0, TN, true), g2 = thinLevel / rmsOf(sDeriv, 0, TN, false);
    for (let i = 0; i < TW; i++) { sFlow[i] *= g1; sDeriv[i] *= g2; }
    TABLES = { flow, deriv, shape, te, sFlow, sDeriv, sShape, flowMip, derivMip };
    return TABLES;
  }

  // in-place radix-2 FFT (table building only)
  function fft(re, im, inverse) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const ang = (inverse ? 2 : -2) * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang), h = len >> 1;
      for (let i = 0; i < n; i += len) {
        let cr = 1, ci = 0;
        for (let j = 0; j < h; j++) {
          const a = i + j, b = a + h;
          const vr = re[b] * cr - im[b] * ci, vi = re[b] * ci + im[b] * cr;
          re[b] = re[a] - vr; im[b] = im[a] - vi; re[a] += vr; im[a] += vi;
          const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
        }
      }
    }
  }

  // pick the band-limited table for a pulse lasting `len` samples at the 4x rate:
  // harmonics may reach (4x rate − host Nyquist); above that they would fold into the audible band
  function mipLevel(len) {
    const maxH = len * (1 - 0.5 / OS);
    let l = 0, H = TN / 2;
    while (H > maxH && l < MIP_LEVELS - 1) { H >>= 1; l++; }
    return l;
  }

  /* ---------- decimation filter ---------- */

  function designFir() {
    const h = new Float32Array(FIR_LEN);
    const fc = 0.5 / OS;                // cutoff at the host Nyquist, in cycles per 4x sample
    const M = FIR_LEN - 1;
    let sum = 0;
    for (let i = 0; i < FIR_LEN; i++) {
      const x = i - M / 2;
      const sinc = x === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * x) / (Math.PI * x);
      const w = 0.42 - 0.5 * Math.cos(2 * Math.PI * i / M) + 0.08 * Math.cos(4 * Math.PI * i / M);
      h[i] = sinc * w;
      sum += h[i];
    }
    for (let i = 0; i < FIR_LEN; i++) h[i] /= sum;
    return h;
  }

  /* ---------- parameters ---------- */

  const PARAMS = {
    voicing:   { def: 1,     min: 0,    max: 1 },
    breathy:   { def: 0,     min: 0,    max: 1 },
    tense:     { def: 0,     min: 0,    max: 1 },
    creaky:    { def: 0,     min: 0,    max: 1 },
    thin:      { def: 0,     min: 0,    max: 1 },
    growl:     { def: 0,     min: 0,    max: 1 },
    jitter:    { def: 0,     min: 0,    max: 1 },
    shimmer:   { def: 0,     min: 0,    max: 1 },
    transpose: { def: 0,     min: -24,  max: 24 },     // semitones
    detune:    { def: 0,     min: -100, max: 100 },    // cents
    mode:      { def: 0,     min: 0,    max: 1 },      // 0 mono, 1 poly
    glide:     { def: 0.06,  min: 0,    max: 2 },      // seconds (mono only)
    attack:    { def: 0.02,  min: 0.001, max: 5 },
    decay:     { def: 0.3,   min: 0.001, max: 5 },
    sustain:   { def: 0.8,   min: 0,    max: 1 },
    release:   { def: 0.25,  min: 0.001, max: 8 },
    volume:    { def: -6,    min: -48,  max: 6 },      // dB
    source:    { def: 0,     min: 0,    max: 1 },      // 0 flow, 1 derivative
  };

  // Breath noise colour: soft /h/ = white low-passed at 2.5 kHz, bright whisper = white high-passed
  // at 1.8 kHz; both through a gentle 7 kHz roll-off. Gains level-match them (measured offline).
  const NOISE_SOFT_HZ = 2500, NOISE_BRIGHT_HZ = 1800, NOISE_AIR_HZ = 7000;
  const NOISE_SOFT_GAIN = 0.89, NOISE_BRIGHT_GAIN = 0.6;
  const ROUGH_HZ = 45;

  /* ---------- voice ---------- */

  class Voice {
    constructor(eng, seed) {
      this.e = eng;
      this.seed = seed >>> 0 || 1;
      this.rng = this.seed;
      this.reset();
    }
    reset() {
      this.stage = 0;            // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
      this.env = 0;
      this.note = -1;
      this.held = false;
      this.vel = 1;
      this.age = 0;
      this.logF = Math.log(110);
      this.glideLeft = 0;
      this.glideStep = 0;
      this.hasPitch = false;
      this.cyc = 0;              // samples (4x) elapsed in the current cycle
      this.per = 0;              // cycle length (4x samples, fractional)
      this.pulse = 1;            // open+return length inside the cycle
      this.inc = 1;
      this.cycleN = 0;
      this.amp = 1;
      this.ri = 0; this.rf = 0; this.sm = 0;
      this.tab = null; this.sTab = null; this.subTab = null;
      this.subPos = 0;
      this.n1 = 0; this.n2 = 0; this.n3 = 0; this.rough = 0;
      this.lastF0 = 0; this.lastRd = RD_MODAL; this.lastOq = 0;
    }
    rand() {
      let x = this.rng;
      x ^= x << 13; x ^= x >>> 17; x ^= x << 5;
      this.rng = x >>> 0;
      return this.rng / 4294967296;
    }
    gauss() { return (this.rand() + this.rand() + this.rand() + this.rand() - 2) * 1.7320508; }

    get active() { return this.stage !== 0; }

    setTarget(note, glide) {
      const target = Math.log(440) + (note - 69) * Math.LN2 / 12;
      this.note = note;
      const gs = this.e.p.glide;
      if (glide && gs > 0 && this.hasPitch) {
        this.glideLeft = Math.max(1, Math.round(gs * this.e.sr));
        this.glideStep = (target - this.logF) / this.glideLeft;
      } else {
        this.logF = target;
        this.glideLeft = 0;
      }
      this.hasPitch = true;
    }

    trigger(vel) {
      this.vel = vel;
      if (this.stage === 0) {     // starting from silence: begin a fresh cycle
        this.cyc = 0; this.per = 0; this.cycleN = 0; this.env = 0;
      }
      this.stage = 1;
      this.age = ++this.e.ageCounter;
    }

    release() { if (this.stage !== 0) this.stage = 4; }

    startCycle() {
      const e = this.e, P = e.p, T = e.tables;
      let f0 = Math.exp(this.logF + e.pitchLog);
      if (f0 < 20) f0 = 20; else if (f0 > 2500) f0 = 2500;
      const k = this.cycleN++;
      let per = e.srOS / f0;
      let amp = 1;
      if (P.jitter > 0) per *= 1 + P.jitter * 0.05 * this.gauss();
      const growl = e.s.growl, creak = e.s.creaky;
      if (growl > 0) {
        per *= 1 + ((k & 1) ? -1 : 1) * growl * 0.12;
        if (k & 1) amp *= 1 - 0.55 * growl;
      }
      let pulse = per;
      if (creak > 0) {
        pulse = per * (1 - 0.45 * creak);
        per = per * (1 + creak * 0.35 * Math.abs(this.gauss()));
        if (this.rand() < creak * 0.3) per *= 1.4 + this.rand() * 1.2 * creak;
        if (k & 1) amp *= 1 - 0.5 * creak * (0.6 + 0.4 * this.rand());
        amp *= 1 + 0.9 * creak;          // fewer pulses per second: keep fry roughly as loud
      }
      if (P.shimmer > 0) amp *= Math.exp(P.shimmer * 0.25 * this.gauss());
      if (pulse < 8) pulse = 8;
      if (per < pulse) per = pulse;

      // pulse shape for this cycle
      const rdx = (Math.exp(e.lnRd) - RD_LO) / RD_STEP;
      let ri = Math.floor(rdx);
      if (ri < 0) ri = 0; else if (ri > RD_COUNT - 2) ri = RD_COUNT - 2;
      let rf = rdx - ri; if (rf < 0) rf = 0; else if (rf > 1) rf = 1;
      this.ri = ri; this.rf = rf; this.sm = e.sineMix;
      const deriv = e.sourceDeriv;
      const lvl = mipLevel(pulse);
      this.tab = deriv ? T.derivMip[lvl] : T.flowMip[lvl];
      this.subTab = deriv ? T.derivMip[mipLevel(per * 2)] : T.flowMip[mipLevel(per * 2)];
      this.sTab = deriv ? T.sDeriv : T.sFlow;
      this.amp = amp;
      this.inc = 1 / pulse;
      this.pulse = pulse;
      this.per = per;
      this.lastF0 = e.srOS / per;
      this.lastRd = Math.exp(e.lnRd);
      const oq = (T.te[ri] + (T.te[ri + 1] - T.te[ri]) * rf) * (1 - this.sm) + this.sm;
      this.lastOq = oq * pulse / per;
      if (this === e.vizVoice) e.vizCycle();
    }

    render(n) {
      const e = this.e, P = e.p, T = e.tables, buf = e.osBuf;
      const shapeT = T.shape, sShape = T.sShape;
      const subRow = e.subRow * TW;
      const isViz = this === e.vizVoice;
      const sr = e.sr;
      const aInc = 1 / (Math.max(P.attack, 0.001) * sr);
      const dCoef = Math.exp(-4.6 / (Math.max(P.decay, 0.001) * sr));
      const rCoef = Math.exp(-4.6 / (Math.max(P.release, 0.001) * sr));
      const S = P.sustain;
      const rVoic = e.rVoicing, rSt = e.rSteady, rPu = e.rPulse, rBr = e.rBright, rGr = e.rGrowl;
      const a1 = e.nA1, a2 = e.nA2, a3 = e.nA3, ar = e.rA, roughG = e.roughGain;
      const g1 = e.nG1, g2 = e.nG2;
      let o = 0;
      for (let j = 0; j < n; j++) {
        // envelope (host rate)
        switch (this.stage) {
          case 1: this.env += aInc; if (this.env >= 1) { this.env = 1; this.stage = 2; } break;
          case 2: this.env = S + (this.env - S) * dCoef; if (Math.abs(this.env - S) < 1e-4) { this.env = S; this.stage = 3; } break;
          case 3: this.env = S; break;
          case 4: this.env *= rCoef; if (this.env < 1e-5) { this.env = 0; this.stage = 0; } break;
        }
        if (this.glideLeft > 0) { this.logF += this.glideStep; this.glideLeft--; }
        const gain = this.env * this.vel;
        const voicing = rVoic[j], steady = rSt[j], pulsed = rPu[j], bright = rBr[j], growl = rGr[j];
        const subInc = Math.exp(this.logF + e.pitchLog) * 0.5 / e.srOS;
        for (let q = 0; q < OS; q++, o++) {
          if (this.cyc >= this.per) {
            this.cyc -= this.per;
            this.startCycle();
            if (this.cyc >= this.per) this.cyc = 0;
          }
          let g = 0, sh = 0;
          if (this.cyc < this.pulse) {
            const x = this.cyc * this.inc * TN, i = x | 0, fr = x - i;
            const b0 = this.ri * TW + i, b1 = b0 + TW;
            const tab = this.tab;
            const v0 = tab[b0] + (tab[b0 + 1] - tab[b0]) * fr;
            const v1 = tab[b1] + (tab[b1 + 1] - tab[b1]) * fr;
            let v = v0 + (v1 - v0) * this.rf;
            sh = shapeT[b0] + (shapeT[b0 + 1] - shapeT[b0]) * fr;
            if (this.sm > 0) {
              const st = this.sTab;
              const s = st[i] + (st[i + 1] - st[i]) * fr;
              v += (s - v) * this.sm;
              const ss = sShape[i] + (sShape[i + 1] - sShape[i]) * fr;
              sh += (ss - sh) * this.sm;
            }
            g = v * this.amp;
          }
          this.cyc += 1;
          const w = this.rand() * 2 - 1;
          let rough = 0;
          if (growl > 0) {
            this.rough += (w - this.rough) * ar;
            rough = this.rough * roughG;
            if (rough > 2.5) rough = 2.5; else if (rough < -2.5) rough = -2.5;
            let gm = 1 + growl * 0.35 * rough;
            if (gm < 0) gm = 0;
            g *= gm;
            // false-fold pulse train an octave below
            this.subPos += subInc;
            if (this.subPos >= 1) this.subPos -= 1;
            const sx = this.subPos * TN, si = sx | 0, sf = sx - si;
            const st = this.subTab || T.flow;
            g += (st[subRow + si] + (st[subRow + si + 1] - st[subRow + si]) * sf) * growl * 0.4 * (1 + 0.25 * rough);
          }
          // breath noise: soft /h/ ↔ bright whisper, steady + pulsed by the glottal opening
          this.n1 += (w - this.n1) * a1;
          this.n2 += (w - this.n2) * a2;
          const soft = this.n1 * g1, hi = (w - this.n2) * g2;
          this.n3 += (soft + (hi - soft) * bright - this.n3) * a3;   // gentle air roll-off
          const noise = this.n3;
          const nAmp = steady + pulsed * sh;
          const voiced = g * voicing;
          buf[o] += (voiced + noise * nAmp) * gain;
          if (isViz) e.vizPush(voiced, nAmp);
        }
      }
    }
  }

  /* ---------- engine ---------- */

  function ramp(arr, a, b, n) {
    const st = (b - a) / n;
    for (let j = 0; j < n; j++) arr[j] = a + st * (j + 1);
    return b;
  }

  class ThroatEngine {
    constructor(sampleRate, seed) {
      this.sr = sampleRate;
      this.srOS = sampleRate * OS;
      this.tables = buildTables();
      this.subRow = Math.round((RD_SUB - RD_LO) / RD_STEP);
      this.p = {};
      for (const k in PARAMS) this.p[k] = PARAMS[k].def;
      this.s = { voicing: 1, breathy: 0, tense: 0, creaky: 0, thin: 0, growl: 0 };   // smoothed knobs
      this.smoothCoef = 1 - Math.exp(-BLOCK / (0.02 * sampleRate));
      this.lnRd = Math.log(RD_MODAL); this.sineMix = 0; this.sourceDeriv = false;
      this.pitchLog = 0;
      this.voices = [];
      const base = (seed >>> 0) || 0x9e3779b9;
      for (let i = 0; i < MAX_VOICES; i++) this.voices.push(new Voice(this, (base + i * 0x6d2b79f5) >>> 0));
      this.ageCounter = 0;
      this.stack = [];           // mono note stack (preallocated use only via push/splice of small ints)
      this.osBuf = new Float32Array(BLOCK * OS);
      this.blk = new Float32Array(BLOCK); this.blkPos = BLOCK;
      this.rVoicing = new Float32Array(BLOCK); this.rSteady = new Float32Array(BLOCK);
      this.rPulse = new Float32Array(BLOCK); this.rBright = new Float32Array(BLOCK);
      this.rGrowl = new Float32Array(BLOCK); this.rVol = new Float32Array(BLOCK);
      this.cur = { voicing: 1, steady: 0, pulse: 0, bright: 0.35, growl: 0, vol: Math.pow(10, PARAMS.volume.def / 20) };
      // noise filters (at the 4x rate)
      const op = hz => 1 - Math.exp(-2 * Math.PI * hz / this.srOS);
      this.nA1 = op(NOISE_SOFT_HZ); this.nA2 = op(NOISE_BRIGHT_HZ); this.nA3 = op(NOISE_AIR_HZ); this.rA = op(ROUGH_HZ);
      const sdU = 1 / Math.sqrt(3);
      this.roughGain = 1 / (sdU * Math.sqrt(this.rA / (2 - this.rA)));
      this.nG1 = NOISE_SOFT_GAIN;
      this.nG2 = NOISE_BRIGHT_GAIN;
      // decimator
      this.fir = designFir();
      this.hist = new Float32Array(FIR_LEN * 2);
      this.hw = 0;
      // DC blocker
      this.dcR = 1 - 2 * Math.PI * 20 / sampleRate;
      this.dcX = 0; this.dcY = 0;
      // live view capture
      this.vizVoice = null;
      this.capG = new Float32Array(65536); this.capN = new Float32Array(65536);
      this.capLen = 0; this.capCycles = 0;
      this.vizG = new Float32Array(256); this.vizN = new Float32Array(256);
      this.vizReady = false; this.vizInfo = { f0: 0, rd: 1, oq: 0, voices: 0 };
    }

    setParam(id, v) {
      const d = PARAMS[id];
      if (!d) return;
      v = +v;
      if (!(v === v)) return;
      if (v < d.min) v = d.min; else if (v > d.max) v = d.max;
      if (id === 'mode') {
        v = v >= 0.5 ? 1 : 0;
        if (v !== this.p.mode) { this.allNotesOff(); this.stack.length = 0; }
      }
      if (id === 'source') v = v >= 0.5 ? 1 : 0;
      this.p[id] = v;
    }

    velAmp(vel) { const v = Math.max(1, Math.min(127, vel)) / 127; return v; }

    noteOn(note, vel) {
      const va = this.velAmp(vel == null ? 100 : vel);
      if (this.p.mode === 0) {
        const v = this.voices[0];
        const legato = this.stack.length > 0;
        const i = this.stack.indexOf(note);
        if (i >= 0) this.stack.splice(i, 1);
        this.stack.push(note);
        v.setTarget(note, true);
        if (!legato || v.stage === 0 || v.stage === 4) v.trigger(va); else v.vel = va;
        this.vizVoice = v;
      } else {
        let v = null;
        for (const x of this.voices) if (x.active && x.note === note) { v = x; break; }
        if (!v) for (const x of this.voices) if (!x.active) { v = x; break; }
        if (!v) {   // steal: quietest releasing voice, else the oldest
          let best = null;
          for (const x of this.voices) if (x.stage === 4 && (!best || x.env < best.env)) best = x;
          if (!best) for (const x of this.voices) if (!best || x.age < best.age) best = x;
          v = best;
        }
        v.setTarget(note, false);
        v.held = true;
        v.trigger(va);
        this.vizVoice = v;
      }
    }

    noteOff(note) {
      if (this.p.mode === 0) {
        const v = this.voices[0];
        const i = this.stack.indexOf(note);
        if (i < 0) return;
        this.stack.splice(i, 1);
        if (this.stack.length === 0) v.release();
        else if (v.note === note) v.setTarget(this.stack[this.stack.length - 1], true);
      } else {
        for (const x of this.voices) if (x.held && x.note === note) { x.held = false; x.release(); }
      }
    }

    allNotesOff() {
      this.stack.length = 0;
      for (const x of this.voices) { x.held = false; x.release(); }
    }

    // per-block parameter smoothing and derived values
    updateBlock(n) {
      const P = this.p, s = this.s, c = this.smoothCoef;
      for (const k in s) s[k] += (P[k] - s[k]) * c;
      // Rd: tense presses toward 0.3, breathy opens toward 2.7, thin and creak bend it too
      let ln = Math.log(RD_MODAL) + s.breathy * Math.log(2.7) + s.tense * Math.log(0.3)
             + s.creaky * Math.log(0.5) + s.thin * Math.log(2.0);
      const lo = Math.log(RD_LO), hi = Math.log(RD_HI);
      this.lnRd = ln < lo ? lo : ln > hi ? hi : ln;
      this.sineMix = s.thin * 0.85;
      this.sourceDeriv = P.source === 1;
      this.pitchLog = (P.transpose + P.detune / 100) * Math.LN2 / 12;
      // breath
      const vo = s.voicing;
      let bright = 0.35 + 0.55 * s.tense - 0.3 * s.breathy + 0.25 * s.thin;
      bright = bright < 0 ? 0 : bright > 1 ? 1 : bright;
      const steady = (1 - vo) * 1.0 + vo * (s.breathy * 0.05 + s.thin * 0.03);
      const pulse = vo * (s.breathy * 0.3 + s.thin * 0.12);
      const cur = this.cur;
      cur.voicing = ramp(this.rVoicing, cur.voicing, vo, n);
      cur.steady = ramp(this.rSteady, cur.steady, steady, n);
      cur.pulse = ramp(this.rPulse, cur.pulse, pulse, n);
      cur.bright = ramp(this.rBright, cur.bright, bright, n);
      cur.growl = ramp(this.rGrowl, cur.growl, s.growl, n);
      cur.vol = ramp(this.rVol, cur.vol, Math.pow(10, P.volume / 20), n);
    }

    // Host calls with any n; the engine always works in fixed BLOCK chunks (BLOCK samples latency).
    process(out, n) {
      const blk = this.blk;
      let p = this.blkPos;
      for (let j = 0; j < n; j++) {
        if (p === BLOCK) { this.processBlock(blk, 0, BLOCK); p = 0; }
        out[j] = blk[p++];
      }
      this.blkPos = p;
    }

    processBlock(out, off, n) {
      this.updateBlock(n);
      const buf = this.osBuf;
      buf.fill(0, 0, n * OS);
      let count = 0;
      for (let i = 0; i < this.voices.length; i++) {
        const v = this.voices[i];
        if (v.stage !== 0) { v.render(n); count++; }
      }
      if (this.vizVoice && this.vizVoice.stage === 0) this.vizVoice = null;
      this.vizInfo.voices = count;
      const h = this.fir, hist = this.hist, L = FIR_LEN, vol = this.rVol;
      let hw = this.hw, x1 = this.dcX, y1 = this.dcY;
      const R = this.dcR;
      for (let j = 0, o = 0; j < n; j++) {
        for (let q = 0; q < OS; q++, o++) {
          const x = buf[o];
          hist[hw] = x; hist[hw + L] = x;
          hw = hw + 1 === L ? 0 : hw + 1;
        }
        let acc = 0;
        for (let t = 0; t < L; t++) acc += h[t] * hist[hw + t];
        const y = acc - x1 + R * y1;
        x1 = acc; y1 = y;
        let s = y * vol[j];
        if (s > 1) s = 1; else if (s < -1) s = -1;
        out[off + j] = s;
      }
      this.hw = hw; this.dcX = x1; this.dcY = y1;
      if (Math.abs(this.dcY) < 1e-20) this.dcY = 0;
    }

    /* live view: record the latest voice's glottal signal, hand over 2 cycles at a time */
    vizPush(g, nAmp) {
      if (this.capLen < this.capG.length) {
        this.capG[this.capLen] = g; this.capN[this.capLen] = nAmp; this.capLen++;
      }
    }
    vizCycle() {
      this.capCycles++;
      if (this.capCycles >= 3) {     // first boundary starts capture; two full cycles after that
        const len = this.capLen;
        if (len > 16) {
          for (let b = 0; b < 256; b++) {
            const i0 = Math.floor(b * len / 256), i1 = Math.max(i0 + 1, Math.floor((b + 1) * len / 256));
            let mx = 0;
            for (let i = i0; i < i1; i++) if (this.capN[i] > mx) mx = this.capN[i];
            this.vizG[b] = this.capG[i0];
            this.vizN[b] = mx;
          }
          const v = this.vizVoice;
          this.vizInfo.f0 = v.lastF0; this.vizInfo.rd = v.lastRd; this.vizInfo.oq = v.lastOq;
          this.vizReady = true;
        }
        this.capLen = 0; this.capCycles = 1;
      }
    }
  }

  ThroatEngine.PARAMS = PARAMS;
  ThroatEngine.OS = OS;
  ThroatEngine.LATENCY = BLOCK;
  ThroatEngine.lfSolve = lfSolve;
  ThroatEngine.buildTables = buildTables;

  if (typeof module !== 'undefined' && module.exports) module.exports = { ThroatEngine, lfSolve, lfEval, buildTables, PARAMS };
  else root.ThroatEngine = ThroatEngine;
})(typeof globalThis !== 'undefined' ? globalThis : this);
