# Percussion Synth

A small synthesizer that happens to be exceptionally good at percussion: two oscillators, noise, a
resonant filter, a pitch sweep and a restrained drive stage. It is not a fixed drum voice. The same
signal path covers kicks, toms, bass hits, metallic clangs, zaps and noisy percussion, and with
longer envelopes it crosses into bass and drone territory. Source: `native/src/instruments/percsynth.h`;
tests: `native/test/percsynth_test.cpp`. Generic instrument contract: [instruments.md](./instruments.md).
Identity: `id "percsynth"`, name `Percussion Synth`, category `synth`, `InstrumentKind.PercSynth = 16`.

The idea (a few oscillators, a big pitch envelope, FM between them, a filter that is part of the
sound, a little nonlinearity) is in the spirit of analogue percussion synthesizers. This is an
original webdsp voice, not a model of any of them and not a parameter-for-parameter copy.

## Signal path

```
Osc 1 -----------------------------------+--> x osc1level --+
  |  (raw output, before its level)       |                  |
  +--FM, mean pitch kept--> Osc 2 --> x osc2level ---+       +--> mix --> 12 dB LP / HP (cutoff, resonance,
Noise (colour, own decay) ---> x noise ------------+                      cutoff x 2^(filterenv x 6 oct x filter env))
   --> drive: tanh + soft feedback, DC block --> x amp env^2 x velocity --> soft clip (|x| <= 1) --> Panner --> bus

pitch envelope: one exponential sweep. Osc 1 starts `pitchenv` octaves up; osc 2 follows by `osc2env` x that.
```

Mono per instance (a pattern of overlapping hits = several instances). Retrigger is continuous
(oscillator phase, filter and level carry, the sweeps restart); a fresh note resets everything, so
identical inputs render identical samples. Not tempo-synced.

Reused, nothing new in the engine, bus or ABI: `AdsrEnvelope` (amplitude), `PercEnv` (pitch, noise
and filter envelopes), `PercNoise`, `BiquadFilter` (12 dB), `PercCore` (lifecycle, pre-note delay,
pan), the shared sine table (`fm_kernel.h`), `setTableParam`. PolyBLEP saw/square, the tanh drive and
the soft clip are the same inline forms Bass/Pad/PWM Beast use.

## Parameters

| Param | Range | Default | Effect |
|---|---|---|---|
| `tune` | 20-2000 Hz | 55 | Osc 1 resting pitch. A note's `pitch` is semitones on top and moves osc 2 with it. |
| `osc1wave` / `osc2wave` | step 0-2 | 0 / 1 | Sine-triangle blend (round), saw, square. Saw and square are PolyBLEP band-limited. |
| `osc1level` / `osc2level` | 0-1 | 0.9 / 0 | Mixer faders. Osc 1 is silent at 0 but still drives the FM. |
| `osc2pitch` | -24..48 st | 12 | Osc 2 relative to osc 1. Whole intervals are tonal; fractions (e.g. 8.4) are inharmonic and metallic. |
| `osc2fm` | 0-1 | 0 | Osc 1 modulates osc 2's frequency, `f2 x (1 + depth x osc1)`, depth = 3 x amount^2. Linear, so osc 2 stays tuned; the index follows the osc2 : osc1 ratio. |
| `pitchenv` | 0-6 oct | 2.5 | How far above rest osc 1 starts. |
| `pitchdecay` | 5 ms-2 s | 70 ms | Time for the sweep to fall ~40 dB (the percussion-family meaning of "decay"). |
| `osc2env` | 0-2 | 1 | How much of the sweep osc 2 follows. 0 = osc 2 holds its pitch; 2 = it sweeps twice as far. |
| `noise` | 0-1 | 0 | Noise level. |
| `noisetone` | 0-1 | 0.5 | Colour: dark (one-pole low-pass) -> white -> bright (high-pass). Levelled so the ends stay usable. |
| `noisedecay` | 3 ms-2 s | 40 ms | Noise envelope, time to fall ~40 dB. Independent of the amp envelope (which still gates it). |
| `filtertype` | step 0/1 | 0 | Low-pass or high-pass. |
| `cutoff` | 40-16000 Hz | 5000 | Keytracked at half an octave per octave. |
| `resonance` | 0-1 | 0.15 | Q 0.71 -> ~13.7 (quadratic), level-compensated. |
| `filterenv` | -1..1 | 0 | Filter envelope depth, +-6 octaves. Negative sweeps down. Velocity scales it a little. |
| `filterdecay` | 10 ms-2 s | 150 ms | Filter envelope, time to fall ~40 dB. |
| `attack` / `decay` / `sustain` / `release` | 0.5 ms-0.25 s / 20 ms-4 s / 0-1 / 5 ms-4 s | 1 ms / 0.35 s / 0 / 0.12 s | Amp ADSR (linear segments, applied squared). Sustain 0 = a self-ending one-shot. With sustain > 0 the note is held: pass `duration` or call `instrumentNoteOff`. |
| `drive` | 0-1 | 0.2 | Character: clean -> punchy -> saturated -> aggressive. |

### Drive stage

Placed after the filter so a resonant ring is saturated too. Asymmetric tanh (an input offset adds
even harmonics) up to +24 dB, level-compensated; exactly transparent at 0. A low-passed one-sample
feedback of the stage's own output (at most 0.6 of loop gain, quadratic in drive) thickens the lows.
It is bounded by construction: tanh limits the fed-back signal to +-1 and the loop's small-signal gain
stays below 1, so it cannot latch or run away. The output is DC-blocked (5 Hz) and soft-clipped, so
every parameter combination is bounded to |x| <= 1.

### Velocity

Level (linear), plus a little character on hard hits: FM depth x(0.7..1), noise x(0.7..1), filter
envelope x(0.7..1), drive x(0.6..1). All smooth and monotonic.

## Sound-design starting points

Not presets (clients own those); these are the shapes the architecture is built for.

| Sound | Direction |
|---|---|
| Deep kick | `tune` 40-50, `pitchenv` ~3, `pitchdecay` ~50 ms, `decay` ~0.4 s, `cutoff` ~400 |
| Punchy kick | `pitchenv` 4-5, short `pitchdecay`, osc 2 square at 0 st, a tick of `noise` (`noisedecay` ~8 ms), `drive` ~0.5 |
| Tom | `tune` ~130, `pitchenv` ~1, `pitchdecay` ~120 ms, `decay` ~0.6 s |
| Bass percussion | little `pitchenv`, `decay` 1 s+, saw osc 1, `cutoff` ~600, some `drive` |
| Metallic hit | squares, `osc2pitch` 8.4, `osc2level` 1, `osc2fm` ~0.7, high-pass `cutoff` ~2.5 kHz, short `decay` |
| Zap | `pitchenv` 6, `pitchdecay` ~300 ms, fast `attack` |
| Noisy percussion | `osc1level` 0, `noise` 1, `filterenv` ~0.8, short `decay`, some `resonance` |
| Experimental | saw + square, `osc2pitch` ~20, high `osc2fm`, high `resonance`, `drive` 1 |

## Limitations

- Parameters are read per render block (as every instrument here); there is no extra smoothing layer.
  Level-type controls step at block boundaries, the filter cutoff updates every 16 samples.
- Osc 2 shares the pitch-envelope decay with osc 1 (it has its own depth via `osc2env`).
- Ear-tuning of defaults, `kLevel` and the ranges is still to do (set `PERCSYNTH_WAV_DIR=<dir>` when running
  `percsynth_test` to dump the sound-design cases as WAVs).
