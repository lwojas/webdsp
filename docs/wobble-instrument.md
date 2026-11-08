# Wobble

An opinionated bass for DnB, dubstep, grime and related bass music. Not a second general VA synth: eight
musical macros over one fixed, purpose-built signal path, and a default patch that is already a finished
wobble bass. Source: `native/src/instruments/wobble.h`; tests: `native/test/wobble_test.cpp`. Generic
instrument contract: [instruments.md](./instruments.md).
Identity: `id "wobble"`, name `Wobble`, category `synth`, `InstrumentKind.Wobble = 20`, params `WobbleParam`.

## Signal path

```
saw + detuned pulse (PolyBLEP) <- phase-modulated by a sine at 2.0017 x f0      growl: FM index
   |  src
   +--> ZDF ladder LP 24 dB (MatrixFilter) -> 2x one-pole HP at 1.7 f0 ---+
   +--> presence band (bite) -> fold ------------------------------------+--> mid -> tanh drive (gain-compensated)
   +--> two moving formant band-passes (growl) --------------------------+            -> DC block
sine at f0 (+ a little 3rd) = sub: never filtered, never wobbled ---------------------------------------> +
   -> x amp env^2 x velocity x loudness trim -> soft clip (|x| <= 1) -> Panner -> bus
```

**One wobble, not an LFO patched onto a filter.** A single unipolar signal `u` (1 = open at the start of the
cycle) moves, together and in proportion to `wobble`: the filter cutoff (about +3.2 / -1.8 octaves around
`cutoff` at full depth), the FM index (so the harmonics breathe), the resonance (more when the filter is
closed) and the formant "vowel" (the yoi-yoi). The depth curve is gentle at the bottom of the knob.

**The low end is a separate path.** The sub sine at the fundamental never goes through the filter, the FM or
the wobble. The filtered saw is high-passed above the fundamental so the two never beat or cancel (measured:
without that, the sub band swung 8-16 dB under wobble; now ~2 dB at growl 0). `weight` is the sub level, its
share of upper harmonics, and an equal-power trade against the mids.

**Tempo-locked.** The wobble phase is a pure function of the engine beat grid (`Instrument::syncTransport`,
like Pulse's pump), so notes on the beat always start the same point of the cycle, tempo changes never jump,
and a sequencer gets consistent wubs. `freerate` > 0 switches to a free-running wobble that restarts at each note.

Reused, nothing new in the engine, bus or ABI: `AdsrEnvelope`, `MatrixFilter` (zero-delay-feedback ladder, stable
under per-sample retuning), `PercBandpass`, `PercCore` (lifecycle, pre-note delay, pan, routing), `setTableParam`,
the LFO sample-and-hold hash (`lfoSampleHold`). New and local: the oscillator stage, the wobble shapes, the
trim curves. A new kind is the one registry entry plus a `WobbleParam` enum.

## Controls

Front panel: `wobble`, `rate`, `growl`, `bite`, `weight`, `drive`, `cutoff`, `resonance` (+ `shape` as a small
selector). `tune`, `release`, `freerate`, `glide` are the long-form extras. All macros are 0..1. A `character`
macro was deliberately left out: growl, bite, drive and wobble already span the territory.

| Param | Range | Default | Effect |
|---|---|---|---|
| `wobble` | 0-1 | 0.6 | Depth of the coordinated movement. 0 = steady bass; 1 = violent. Mid band swings ~2-3 dB at 0, ~13 dB at 1 (~19 dB with full growl). |
| `rate` | step 0-11 | 6 (1/8) | Synced cycle: 1/1, 1/2, 1/4., 1/4, 1/8., 1/4T, 1/8, 1/16., 1/8T, 1/16, 1/16T, 1/32. Names: `kWobbleRateNames`. |
| `shape` | step 0-3 | 0 | smooth (cosine), square-ish (slewed 4 ms), ramp (snaps open, falls), random (stepped, two steps per cycle, first always open). |
| `growl` | 0-1 | 0.5 | Clean heavy bass to FM sidebands + moving formants + detuned second oscillator. Modulated by the wobble. |
| `bite` | 0-1 | 0.45 | Onset zap (cutoff opens for ~90 ms), a folded presence band around 1.5-4 kHz, a touch more resonance. Cuts through drums without an EQ boost: 2-8 kHz rises ~12 dB, the low end does not move. |
| `weight` | 0-1 | 0.6 | Lean to huge: sub level 0.12-0.52, more sub harmonics, mids trade off equal-power. |
| `drive` | 0-1 | 0.4 | tanh with slight asymmetry on the mid path; gain-compensated, so it adds density (1-8 kHz +2 dB and more), not level. |
| `cutoff` | 80-8000 Hz | 650 | Centre of the wobble sweep. The bite and growl paths follow how open the filter is, so it stays the obvious performance control (+13 dB of 0.6-6 kHz from 150 to 4000 Hz). |
| `resonance` | 0-1 | 0.4 | Smooth to screaming; capped short of self-oscillation (ladder loop gain at most 0.93 of 4). |
| `tune` | 27.5-110 Hz | 55 | Root; a note's `pitch` is semitones on top. |
| `release` | 10 ms-1.5 s | 0.12 | Attack is fixed at 2 ms, sustain full: hold with `duration` or `instrumentNoteOff`. |
| `freerate` | 0-20 Hz | 0 | 0 = synced via `rate`; > 0 = free-running. |
| `glide` | 0-0.5 s | 0.06 | Slide time of a note tied to the held one (`tie`): no re-attack, pitch glides. |

Modulation targets: all macros and `cutoff`/`tune` (log); `rate`, `shape` (steps) and `freerate` are not.
Velocity scales level (soft notes quieter, 0 silent) and the onset zap.

## Gain staging

Loudness, not peak, is what a macro must not change. A held default note is -9.5 dB rms (peak 0.8). Weight,
Drive and Growl each add energy, so each has a trim measured by sweeping it alone and held at zero at the
default (`weight`: -4.5 dB per unit, `drive`: -4 dB x (drive^1.3 - 0.4^1.3), `growl`: a five-point curve).
Result: every macro swept 0 to 1 stays within ~1 dB of the default (resonance 1: +1 dB; all macros at 1:
about -3 dB, which is what stacked compensation gives); the test asserts +-3 dB. The output is bounded by the
same soft clip as Bass (|x| <= 1 by construction), and the ladder, drive and filters are bounded internally.
Macro changes are smoothed at control rate (12 ms); the cutoff and FM index are interpolated per sample.

## Using it

```ts
const id = rt.createInstrument(InstrumentKind.Wobble, { bus });
rt.setInstrumentParameter(id, WobbleParam.Rate, 6);      // 1/8
rt.setInstrumentParameter(id, WobbleParam.Growl, 0.8);
rt.instrumentNoteOn(id, { velocity: 1, pitch: 3, duration: 0.45 });
```

The tempo comes from the engine transport; without one it is 120 BPM. There is no per-instrument panel in the
demo app (as for every other kind): the catalogue (`getInstrumentParams`) is the UI contract.

## Validation

Native tests (`native/test/wobble_test.cpp`, 346 checks): registration and stable names; every parameter
changes the sound; clamping and non-finite input; lifecycle, release, velocity; determinism and pitch; **wobble
period** measured from the output for 1/16, 1/8, 1/4, dotted and triplet steps and at 60/120/240 BPM, free-run
independent of tempo, and phase identical for notes on different bars; wobble depth and shape movement; growl,
bite, weight, drive, cutoff and resonance each move the spectrum the way they are described; loudness within
+-3 dB for every macro; sub steady under wobble; extremes (all-min, all-max, mixed; pitches -48..+48; velocity
0..4; 22.05-96 kHz) finite and |x| <= 1; 8 s all-max run does not grow; mid-note parameter jumps do not click;
tied-note glide; no allocation in the audio path.

`WOBBLE_WAV_DIR=<dir> native/build/wobble_test` writes nine 140 BPM, 8-note listening renders (default, clean,
1/4, 1/16T growl, ramp, square, random, everything up, lean and dark).

**Not done: critical listening.** The tuning above is from spectral and level measurements. Whether the default
reads as "that is a wobble bass" is for ears; the knobs most likely to want a nudge are `kFmMax`, `kFormantGain`
and `kSweepOct`. Aliasing from the folded presence band at high bite is only bounded by the 8 kHz band
measurement (about -26 dB below the sub at bite 1), not by ear. The shared `modulation_test` has one failing
check (`plain` vs `zero` LFO render) that exists without this instrument.
