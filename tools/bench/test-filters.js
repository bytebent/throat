/*
 * Test filters for the Throat bench. NOT part of Throat: listening aids only, so a bare
 * larynx can be judged. Both switch on and off with a short crossfade.
 *   - Vowel: four cascaded formant resonators, one slider morphing from A to I.
 *   - Low-pass: a 12 dB/oct state-variable filter with cutoff and resonance.
 */
(function (root) {
  'use strict';

  // male formants (Hz) and bandwidths, roughly Peterson & Barney
  const VOWEL_A = { f: [730, 1090, 2440, 3400], bw: [80, 90, 120, 250] };
  const VOWEL_I = { f: [270, 2290, 3010, 3500], bw: [60, 100, 150, 250] };
  const SUB = 32;                     // coefficient update interval (samples)
  const VOWEL_GAIN = [0.66, 0.46];          // level match at A and I (measured in tests)

  class TestFilters {
    constructor(sr) {
      this.sr = sr;
      this.vowelOn = false; this.vowel = 0;
      this.lpOn = false; this.cutoff = 3000; this.res = 0.1;
      this.vWet = 0; this.lWet = 0;
      this.vMix = 0;                  // smoothed vowel position
      this.lnCut = Math.log(3000); this.resS = 0.1;
      this.ry1 = new Float64Array(4); this.ry2 = new Float64Array(4);
      this.ra = new Float64Array(4); this.rb = new Float64Array(4); this.rc = new Float64Array(4);
      this.ic1 = 0; this.ic2 = 0;
      this.fade = 1 - Math.exp(-1 / (0.015 * sr));
      this.smooth = 1 - Math.exp(-SUB / (0.03 * sr));
      this.makeup = 1;
      this.updateVowel(true);
    }

    set(id, v) {
      if (id === 'vowelOn') this.vowelOn = !!v;
      else if (id === 'vowel') this.vowel = Math.min(1, Math.max(0, +v || 0));
      else if (id === 'lpOn') this.lpOn = !!v;
      else if (id === 'cutoff') this.cutoff = Math.min(16000, Math.max(60, +v || 3000));
      else if (id === 'res') this.res = Math.min(1, Math.max(0, +v || 0));
    }

    updateVowel(snap) {
      this.vMix = snap ? this.vowel : this.vMix + (this.vowel - this.vMix) * this.smooth;
      const t = this.vMix, sr = this.sr;
      for (let k = 0; k < 4; k++) {
        const f = Math.exp(Math.log(VOWEL_A.f[k]) * (1 - t) + Math.log(VOWEL_I.f[k]) * t);
        const bw = VOWEL_A.bw[k] * (1 - t) + VOWEL_I.bw[k] * t;
        // Klatt resonator: unity gain at DC
        const C = -Math.exp(-2 * Math.PI * bw / sr);
        const B = 2 * Math.exp(-Math.PI * bw / sr) * Math.cos(2 * Math.PI * f / sr);
        const A = 1 - B - C;
        this.ra[k] = A; this.rb[k] = B; this.rc[k] = C;
      }
      // keep the cascade's loudness roughly level across the slider
      this.makeup = Math.exp(Math.log(VOWEL_GAIN[0]) * (1 - t) + Math.log(VOWEL_GAIN[1]) * t);
    }

    process(buf, n) {
      const vOnT = this.vowelOn ? 1 : 0, lOnT = this.lpOn ? 1 : 0;
      if (this.vWet === 0 && this.lWet === 0 && !vOnT && !lOnT) return;
      const fade = this.fade;
      for (let s = 0; s < n; s += SUB) {
        const m = Math.min(SUB, n - s);
        this.updateVowel(false);
        this.lnCut += (Math.log(this.cutoff) - this.lnCut) * this.smooth;
        this.resS += (this.res - this.resS) * this.smooth;
        let fc = Math.exp(this.lnCut); if (fc > this.sr * 0.45) fc = this.sr * 0.45;
        const g = Math.tan(Math.PI * fc / this.sr);
        const k = 2 - 1.96 * this.resS;          // damping: Q from 0.5 to 25
        const a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const ra = this.ra, rb = this.rb, rc = this.rc, y1 = this.ry1, y2 = this.ry2, mk = this.makeup;
        for (let j = s; j < s + m; j++) {
          let x = buf[j];
          this.vWet += (vOnT - this.vWet) * fade;
          this.lWet += (lOnT - this.lWet) * fade;
          if (this.vWet > 1e-4 || vOnT) {
            let y = x;
            for (let r = 0; r < 4; r++) {
              const o = ra[r] * y + rb[r] * y1[r] + rc[r] * y2[r];
              y2[r] = y1[r]; y1[r] = o; y = o;
            }
            x = x + (y * mk - x) * this.vWet;
          } else if (this.vWet !== 0) { this.vWet = 0; y1.fill(0); y2.fill(0); }
          if (this.lWet > 1e-4 || lOnT) {
            const v3 = x - this.ic2;
            const v1 = a1 * this.ic1 + a2 * v3;
            const v2 = this.ic2 + a2 * this.ic1 + a3 * v3;
            this.ic1 = 2 * v1 - this.ic1; this.ic2 = 2 * v2 - this.ic2;
            x = x + (v2 - x) * this.lWet;
          } else if (this.lWet !== 0) { this.lWet = 0; this.ic1 = 0; this.ic2 = 0; }
          if (x > 1) x = 1; else if (x < -1) x = -1;
          buf[j] = x;
        }
      }
    }
  }

  if (typeof module !== 'undefined' && module.exports) module.exports = { TestFilters };
  else root.TestFilters = TestFilters;
})(typeof globalThis !== 'undefined' ? globalThis : this);
