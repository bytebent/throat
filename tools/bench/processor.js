/* AudioWorklet wrapper: Throat engine → test filters → output. Runs after throat-engine.js and test-filters.js. */
class ThroatProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.eng = new ThroatEngine(sampleRate, 1234);
    this.flt = new TestFilters(sampleRate);
    this.vizEvery = Math.round(sampleRate / 30);
    this.vizCount = 0;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.t === 'p') this.eng.setParam(m.id, m.v);
      else if (m.t === 'f') this.flt.set(m.id, m.v);
      else if (m.t === 'on') this.eng.noteOn(m.n, m.v);
      else if (m.t === 'off') this.eng.noteOff(m.n);
      else if (m.t === 'panic') this.eng.allNotesOff();
    };
    this.port.postMessage({ t: 'ready', sr: sampleRate, latency: ThroatEngine.LATENCY });
  }
  process(inputs, outputs) {
    const out = outputs[0], ch0 = out[0], n = ch0.length;
    this.eng.process(ch0, n);
    this.flt.process(ch0, n);
    for (let c = 1; c < out.length; c++) out[c].set(ch0);
    this.vizCount += n;
    if (this.vizCount >= this.vizEvery) {
      this.vizCount = 0;
      const e = this.eng, info = e.vizInfo;
      if (e.vizReady) {
        e.vizReady = false;
        this.port.postMessage({ t: 'viz', g: e.vizG.slice(), n: e.vizN.slice(), f0: info.f0, rd: info.rd, oq: info.oq, voices: info.voices });
      } else {
        this.port.postMessage({ t: 'stat', voices: info.voices });
      }
    }
    return true;
  }
}
registerProcessor('throat-processor', ThroatProcessor);
