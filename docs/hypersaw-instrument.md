# Hypersaw instrument

Seven detuned saws played as one voice: wide, dense, bright, animated. Basses, pads and leads from the
same patch. Source: `native/src/instruments/hypersaw.h`; tests: `native/test/hypersaw_test.cpp`.
Generic instrument contract: [instruments.md](./instruments.md). Technique: #21 in
[synth-dsp/techniques.md](./synth-dsp/techniques.md). Identity: `id "hypersaw"`, name `Hypersaw`,
category `synth`, `InstrumentKind.Hypersaw = 9`.

## Signal path

```
7x PolyBLEP saw, f0 * 2^(detune * offset_i / 1200)          offsets {-1,-.57,-.18,0,+.18,+.57,+1}
   -> gain: centre 1-.55d, sides .04+1.28d-.74d^2 (d = density), power/amplitude-normalised
   -> constant-power pan, positions {-1,-.6,-.22,0,+.22,+.6,+1} x width
   -> 12 dB low-pass (stereo, Q .707, cutoff keytracked at half-octave per octave)
   -> x ADSR x velocity -> soft clip (|x| <= 1) -> balance Panner (note pan) -> bus
```

One voice, not seven: one envelope, one filter, one note lifecycle. Mono per instance like every
instrument (a chord = several instances). Reused: `AdsrEnvelope`, `BiquadFilter`, `PercCore`
(lifecycle, pre-note delay, pan), `setTableParam`. Nothing new in the engine, bus or ABI.

## Parameters

| Param | Range | Default | Effect |
|---|---|---|---|
| `tune` | 27.5-440 Hz | 110 | Root; note `pitch` is semitones on top. |
| `detune` | 0-100 ct | 25 | Offset of the outermost pair; the others scale from it. 0 = unison, ~5 subtle, ~25 supersaw, 100 extreme. |
| `spread` | 0-1 | 0.8 | Stereo width. 0 = all centred (L == R exactly), 1 = full distribution. |
| `density` | 0-1 | 0.7 | Macro: centre vs sides. 0 = lone centre saw, 1 = sides above the centre. Loudness-neutral. |
| `cutoff` | 200-16000 Hz | 5000 | Low-pass, keytracked. |
| `attack` / `decay` / `release` | 1 ms-4 s / 20 ms-4 s / 10 ms-6 s | 0.01 / 0.5 / 0.4 | Amp ADSR (linear, shared implementation). |
| `sustain` | 0-1 | 0.8 | Held level. |

All are LFO targets by the default rules ([lfo-modulation.md](./lfo-modulation.md)): `detune` and
`spread` linear, Hz/seconds controls log.

## Decisions worth knowing

- **Detune table.** Offsets are data (`kOffset`, sized by `kOsc`), symmetric about the centre saw.
  Inner pair at 0.18 and middle pair at 0.57 of the outer pair (the proportions of the classic
  hardware stack): small `detune` is calm because the inner saws barely move, large detune gets the
  outer pair doing the work. The unit is cents, so the character is the same at any pitch.
- **Width follows detune.** Pan positions are in units of a width = `spread` x (0.25 at 0 ct up to 1
  at 40 ct). A unison has nothing to widen; more detune earns more image. The centre saw is never
  panned, the outer pair goes to the edges, pitch order matches pan order (low saws left).
- **Mono.** The panning law is the repo's constant-power one, so folding to mono costs the outer
  pair ~3 dB and nothing is out of phase: the same seven saws add in L and R (tested: mono fold
  within -3/+1.6 dB of the centred version).
- **Phase.** A fresh note starts each saw at a fixed phase, all within 0.11 cycle of each other. Not
  spread over the cycle: at small detune the relationship stays frozen and evenly spread phases
  cancel the fundamental (measured: -6 dB, hollow unison). Not random: identical inputs render
  identical samples. A retrigger on a sounding instrument carries phases, filter and envelope level
  (no reset, no discontinuity); the click-free start comes from the envelope (>= 1 ms attack from 0).
- **Level.** Saws at distinct frequencies add in power, near-identical ones in amplitude (up to
  ~8 dB louder). The normaliser slides between the two over the first 10 ct, so loudness is
  constant from unison to extreme detune, and matches Bass/Pad at defaults (RMS ~0.18).
  Between 1 and 10 ct, slow beating legitimately swings the level over seconds.
- **No internal movement.** The animation is the detune beating itself; no drift or LFO is built in.
  Use the LFO layer on `detune`, `spread` or `cutoff`. Not tempo-synced.
- **Aliasing.** PolyBLEP saws; fundamental clamped to 2 kHz / 0.2 x sample rate like Pad.

## Cost and limits

Per sounding note: 7 PolyBLEP saws and one stereo biquad per sample; 7 `exp2`, 14 trig and one
filter update per render call. No allocation in `render()` (tested). The oscillator count is a
constant, not a parameter. No legato/slide, no HP filter, no filter resonance or envelope: kept
out on purpose; sensible follow-ups if needed.
