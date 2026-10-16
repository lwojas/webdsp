# Bass synth: design proposal

Status: approved 2026-10-15 for implementation. This document is the pre-implementation design,
kept so the decision trail survives if the implementation is reworked. The shipped instrument is
documented in [`bass-instrument.md`](./bass-instrument.md) once it exists.

Inputs: [`synth-dsp/`](./synth-dsp/README.md) research, [`instruments.md`](./instruments.md),
`native/src/instrument.h`, `instruments/kick.h`, `dsp/adsr_envelope.h`, `dsp/biquad_filter.h`.

## A. Existing architecture (relevant findings)

- An `Instrument` is a monophonic machine: `configure / noteOn(velocity, delayFrames, pan, pitch) /
  noteOff / setParam / reset / render / isActive`. Its static `InstrumentParamInfo[]` table is the
  whole public surface. `pitch` is a semitone offset on `tune`.
- Reusable DSP: `AdsrEnvelope` (linear, frame-exact, continuous retrigger), `BiquadFilter`
  (RBJ LP/HP, 12/24 dB, double precision, gain at cutoff = Q), `Saturation` (tanh + asymmetry + DC
  blocker), `PercCore` lifecycle, `Panner`.
- Missing: oscillators beyond Kick's sine, PolyBLEP, ADAA/oversampling, nonlinear (in-loop) filter.
- **Gap:** instrument notes had no scheduled note-off (samples have `durationFrames`;
  `InstrumentNote` did not). A sustained bass needs sample-accurate note length.
- `BiquadFilter` is a block-processing `DSPNode`; every cutoff/Q `setParam` recomputes all designs.
  It can be modulated at sub-block rate, not per sample.

## B. Research that matters

| # | Technique | Verdict |
|---|---|---|
| 1 | PolyBLEP saw | Use |
| 3/4/5 | Phase distortion, FM/PM, feedback FM | Candidate 2 |
| 6 | Dynamic-drive waveshaping | Use |
| 9 | ADAA | Skip: bass fundamentals alias far below audibility, LP follows the shaper |
| 19 | Drift | Skip: sequenced bass needs repeatable notes |
| 21 | Supersaw | Skip: detuned stacks cancel/smear below ~150 Hz |
| 27 | Moog-style nonlinear ladder | Candidate 3 (needs a new filter) |

## C. Candidate 1: "Shaped Core" (chosen)

```
saw (PolyBLEP) -+
sine -----------+- body crossfade -+
sub sine (-1 oct)+                  |
                                    v
        asymmetric tanh <- drive = f(bite, timbreEnv, velocity)
                                    | DC block
                                    v
   BiquadFilter LP 24 dB <- cutoff = f(cutoff, keytrack, timbreEnv, velocity), Q = f(resonance)
                                    | level comp
                                    v
        x ampEnv^2 x velocity -> soft clip -> Panner -> bus
```

Why it works for bass:
- Sine + sub keep the fundamental present when the cutoff closes.
- The shaper sits *before* the filter, so the moving low-pass windows generated harmonics.
- Drive follows an internal timbre envelope, so the note starts rich and settles round (timbral
  pluck, not only a volume pluck).
- Velocity changes drive and cutoff (scaled by `bite`), so dynamics are timbral.
- ~50% cutoff keytracking keeps relative brightness across the range.
- Level compensation keeps drive/resonance from changing loudness.

Cost ~1.5-2x Kick (estimate). Complexity S-M. Public params: 9.

## D. Candidate 2: "Warp Bass" (phase distortion / feedback FM)

Single phase-warped sine, DCW amount under envelope, sub, LP as tone control. Cheapest (~1x Kick),
fundamental constant by construction, digital pluck character. Rejected as first choice: the filter
becomes a minor tone control, and it is weaker for analogue-style lines.

## E. Candidate 3: "Ladder Bass" (nonlinear resonant filter voice)

Pulse/saw + sub into a zero-delay-feedback 4-pole ladder with tanh in the loop. Resonance squash,
level-dependent response, keytracked self-oscillation. ~2-3x Kick, complexity M-L, needs a new
instrument-private filter. **Escalation path**: if Candidate 1 sounds generic by ear, build this
filter privately; do not extend `BiquadFilter`.

## F. Decisions

- Filter: reuse `BiquadFilter` unchanged, 24 dB, cutoff modulated every 16 samples in the octave
  domain, post-filter soft clip bounds the resonant peak.
- Envelopes: one public ADSR (amplitude, squared, `sqrt(sustain)` so public sustain is the real
  level) plus an internal derived timbre `AdsrEnvelope` (attack = amp attack, decay x0.5,
  sustain x0.4, release = amp release; tune by ear).
- Mono per instance. Phase reset to the sine peak (0.25) on a fresh note. Retrigger is continuous.
  Note-off starts `release`. No glide in v1; added in v1.1 as `legato` + `slide` (see bass-instrument.md).
- Prerequisite core change: optional `duration` on instrument notes, implemented engine-side by
  splitting the instrument's render block at the release frame (no `Instrument` interface change).
  Additive; sample playback untouched.

## G. Public API

`InstrumentKind::Bass = 5`; `{ id: "bass", name: "Bass", category: "bass" }`.

| Param | Range | Default | Unit |
|---|---|---|---|
| `tune` | 27.5-110 | 55 | Hz |
| `body` | 0-1 | 0.5 | |
| `bite` | 0-1 | 0.4 | |
| `cutoff` | 40-4000 | 400 | Hz |
| `resonance` | 0-1 | 0.25 | |
| `attack` | 0.001-0.25 | 0.003 | s |
| `decay` | 0.03-2 | 0.35 | s |
| `sustain` | 0-1 | 0.6 | |
| `release` | 0.01-1.5 | 0.1 | s |

Internal (never exposed): sub level, shaper pre-gain range and asymmetry, keytrack amount,
timbre-envelope derivation, envelope->cutoff depth, velocity->timbre depth, Q map and
compensation, filter slope, onset pitch transient, sub-block size, start phase, output level,
fundamental clamp.

## H. Test strategy

Pitch accuracy; envelope timing; filter slope/keytrack; resonance/stability fuzz; parameter
validation; velocity (level and timbre); retrigger continuity; determinism; release and cleanup;
no allocation; peak safety; scheduled duration accuracy. Anti-triviality guards: timbre moves
independently of level, even harmonics present and rising with drive, fundamental retained under a
closed filter, output nonlinear in velocity, spectral distance from a naive saw->LP->ADSR render.

## I. Implementation plan

0. Scheduled instrument note-off (`duration`), separate and additive.
1. Core source prototype + WAV dump (listening gate).
2. Filter integration (sub-block modulation, keytrack, Q map).
3. Envelopes (amp + derived timbre), velocity -> timbre.
4. Voice lifecycle.
5. Public API plumbing (native enums, registry, TS, worklet metadata, paramIds test).
6. Tests + docs.
7. Regenerate WASM and `lib/`, run all tests.

## J. Follow-up for clients

After release, update consuming clients (webtracker etc.) to pass `duration` for bass notes (not
drums) and bump the webdsp version.
