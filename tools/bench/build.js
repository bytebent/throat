// Builds tools/throat.html: the reference engine, test filters and worklet wrapper inlined into one page.
// Run: node tools/bench/build.js
const fs = require('fs'), path = require('path');
const d = __dirname;
const parts = [path.join(d, '..', '..', 'reference', 'throat-engine.js'), path.join(d, 'test-filters.js'), path.join(d, 'processor.js')].map(f => fs.readFileSync(f, 'utf8'));
const dsp = parts.join('\n');
if (/<\/script/i.test(dsp)) throw new Error('DSP source contains </script');
const tpl = fs.readFileSync(path.join(d, 'template.html'), 'utf8');
if (tpl.split('/*__DSP__*/').length !== 2) throw new Error('placeholder missing');
const out = path.join(d, '..', 'throat.html');
fs.writeFileSync(out, tpl.replace('/*__DSP__*/', () => dsp));
console.log('tools/throat.html', Math.round(fs.statSync(out).size / 1024), 'KB');
