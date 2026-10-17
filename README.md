# Throat

A playable synth that models a human larynx. It is the sound source of the Throat/Mouth pair:
Throat makes the sound, Mouth (later) shapes it like a vocal tract.

Each cycle of vocal-fold vibration is an LF (Liljencrants–Fant) glottal pulse, with breath
noise that pulses with every opening, per-cycle irregularity and subharmonics. The knobs blend
freely between modal, breathy, pressed, creaky, falsetto-like, growled and voiceless sound.

**Status:** DSP core and browser test bench. The JUCE plugin (AU/VST3) is in progress.

## Controls

| Group | Control | What it does |
|---|---|---|
| Phonation | Voicing | Voiceless (breath only) ↔ voiced (full fold vibration). |
| | Breathy | Folds don't fully close: rounder pulses, more breath on each cycle. |
| | Tense | Folds press shut: bright, edgy pulses. Also sharpens breath into a whisper. |
| | Creaky | Vocal fry: short pulses at uneven intervals, alternating loud and soft. Best on low notes. |
| | Thin | Falsetto-like: near-sine pulses with a little breath. |
| | Growl | Period-doubled subharmonics plus a rough false-fold pulse an octave below. |
| Irregularity | Jitter / Shimmer | Random period and amplitude variation per cycle. Both 0 = perfectly steady. |
| Pitch | Transpose / Detune | ±24 semitones / ±100 cents. |
| | Glide | Pitch slide between notes (Mono only). |
| | Voices | Mono (one larynx, last note wins) or Poly (8 voices). |
| Envelope | Attack / Decay / Sustain / Release | Amplitude envelope. |
| Output | Source | Glottal flow, or its derivative (brighter). |
| | Volume | −48 to +6 dB. |

## Test bench

`tools/throat.html` is a self-contained page that runs the reference engine live in the
browser. Play it with the computer keyboard (Ableton layout: `A`–`;` white keys,
`W E T Y U O P` black keys, `Z`/`X` octave, `C`/`V` velocity). It also has two test filters,
vowel A↔I and a low-pass, which are listening aids and not part of Throat.

Rebuild it after changing the engine: `node tools/bench/build.js`.

## Repository layout

- `reference/throat-engine.js` — the reference engine; defines the sound
- `source/ThroatDSP.h` — C++ port, real-time safe, no JUCE dependency
- `tests/` — reference tests, test filter tests, and the C++ vs reference comparison
- `tools/throat.html` — browser test bench (built from `tools/bench/`)

## Tests

```sh
node tests/engine-test.js
node tests/filter-test.js
g++ -O2 -std=c++17 -ffp-contract=off -o dsp_cli tests/dsp_cli.cpp && node tests/compare.js ./dsp_cli
```

GitHub Actions runs all of them on every push.

## License note

The plugin will be built with [JUCE](https://juce.com) 8, which is available under the AGPLv3
or a commercial licence. Keeping this repository public and open source satisfies the AGPL.
