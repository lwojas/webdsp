# Sub Bass instrument

`InstrumentKind.SubBass` (22): a compact harmonic sine / sub-bass voice. It is a pure sine at `shape = 0` and
progressively gains harmonics up to a saw-like wave, then goes through anti-aliased drive, a low-pass filter and two
envelopes, so one voice covers deep sub, rounded synth bass and aggressive bass. It is deliberately not a wavetable
synth: there are no tables and nothing to load; the harmonic content is a function of two controls.
Source: `native/src/instruments/subbass.h`; primitives `native/src/dsp/harmonic_osc.h`, `native/src/dsp/adaa_tanh.h`.

## Signal path

```
HarmonicOsc  (additive sin(n t), band-limited)   <- pitch = Root x octave x tune x note x pitch env
     |  Shape / Profile
2x oversampled AdaaTanh drive                    (linear and clean at Drive 0)
     |
MatrixFilter LP 24 / LP 12 dB  <- cutoff = Cutoff + filter env x amount (octaves), Resonance
     |
x Level x amp env^2 x velocity  ->  soft clip (|y| <= 1)  ->  Panner  ->  bus
```

Shape is the oscillator's own harmonic character. Drive is a nonlinearity applied *after* it, so they are different
controls: Shape 0 with Drive 0 is a sine with no harmonics at all; Drive on that sine adds odd harmonics that depend on
level, while Shape adds a fixed, chosen spectrum (including even harmonics, which a symmetric drive cannot).

## Parameters (23, plus the shared `gain`)

| Param | Range | Default | Unit | Meaning |
|---|---|---|---|---|
| `shape` | 0-1 | 0 | | Harmonic content. 0 is exactly one `sin`; 1 is saw-like. |
| `profile` | 0-4 | 0 | step | Where Shape goes: classic, warm, hollow, bright, saw. |
| `octave` | -2..+1 | 0 | step | Octaves from `root`. |
| `tune` | -100..100 | 0 | cents | Fine detune. (Not the root: see `root`.) |
| `level` | 0-1 | 0.85 | | Oscillator level. A Level 1 sine peaks at 0.6. |
| `phase` | 0-1 | 0 | turns | Start phase of a fresh note. |
| `phasereset` | 0/1 | 1 | | 1: a fresh note starts at `phase`. 0: the oscillator free-runs across notes. |
| `drive` | 0-1 | 0 | | Saturation after the oscillator. 0 adds nothing. |
| `filtertype` | 0-1 | 0 | step | 0 LP 24 dB (ladder), 1 LP 12 dB. Switching mid-note crossfades over 10 ms. |
| `cutoff` | 30-16000 | 12000 | Hz | Low-pass cutoff before the filter envelope. |
| `resonance` | 0-1 | 0 | | Emphasis, bounded short of self-oscillation. |
| `fattack` `fdecay` `fsustain` `frelease` | | 0.002 / 0.3 / 0.3 / 0.2 | s / level | Filter envelope. |
| `famount` | -5..+5 | 0 | oct | Filter envelope depth on the cutoff, signed. |
| `pitchamount` | -24..+24 | 0 | st | Pitch envelope start offset; falls to 0. |
| `pitchdecay` | 0.005-0.5 | 0.08 | s | Pitch envelope: time to fall about 40 dB. |
| `attack` `decay` `sustain` `release` | | 0.003 / 0.3 / 1 / 0.25 | s / level | Amp envelope (gain is the envelope squared, so it falls perceptually smoothly). |
| `root` | 20-880 | 55 | Hz | Base frequency; the note's `pitch` is semitones on top. |

Differences from the other instruments' conventions: `tune` is a cents detune here and the root is `root`, because
the brief asked for a musical fine-tune; `root` plays the role `tune` has elsewhere. Everything except the switches
(`profile`, `octave`, `phase`, `phasereset`, `filtertype`) is an LFO target. The pitch parameter of the brief is the
note's `pitch` (semitones), as for every instrument.

## The harmonic oscillator

`HarmonicOsc` sums `a_n sin(2 pi n phase)` for n = 1..H (H <= 64) with the Chebyshev recurrence in double precision:
one `sin` and one `cos` per sample plus about H multiply-adds. The weights are

```
a_n  = n^-tilt(shape) * parity(n) * clamp(Heff - (n - 1), 0, 1)        (a_1 = 1 before normalisation)
Heff = min( 1 + (maxH - 1) * shape^curve , band limit )
```

- Harmonics open one at a time as Shape rises (each fades in over one unit of `Heff`) while the roll-off flattens:
  sine, then a rounded 2nd, then strong 2nd/3rd, then a bright stack, then a 1/n saw. No steps, no table crossfades.
- **Band limit.** `Heff` is capped so that no harmonic is above 0.42 x the sample rate (and a little lower with Drive).
  The cap fades the top harmonic out over one harmonic as pitch rises, so a pitch envelope or a note-on pitch never
  aliases and never pops a harmonic in or out. Amplitudes and the phase increment are ramped per sample across each
  16-sample control step, so there is no staircase to splatter into the top octave.
- Every harmonic is a sine at phase 0, so the wave is odd-symmetric and has **no DC** at any shape.
- Output is normalised to the RMS of a unit sine, so the fundamental stays near full level as Shape adds harmonics and
  loudness moves by under 1 dB across every shape and profile.

| Profile | Roll-off n^-tilt (shape 0 -> 1) | Parity | Harmonics at shape 1 | Character |
|---|---|---|---|---|
| classic | 2.0 -> 1.0 | all | 48 | The all-rounder: rounded, strong 2nd/3rd, bright, saw. |
| warm | 2.4 -> 1.4 | even x1.3, odd x0.75 | 12 | Rounded, 2nd-heavy, never harsh. |
| hollow | 1.6 -> 1.0 | odd only | 48 | Square/clarinet-like. |
| bright | 1.3 -> 0.6 | all | 64 | Slow roll-off, fizzy. |
| saw | 1.0 | all | 48 | A 1/n saw, arriving quickly. |

The spectrum is a pure function (`harmonicSpectrum`); a client mirror in `src/runtime/subBass.ts`
(`harmonicSpectrum`, `harmonicWaveform`) lets a UI draw the waveform and harmonic bars from the same maths without
asking the engine. `test/subBass.test.ts` and `native/test/subbass_test.cpp` pin both ends to the same values.

## Drive

`AdaaTanh` is `tanh(g x) / tanh(g)` with first-order antiderivative anti-aliasing, run inside the Character
Filter's 2x half-band oversampler. `g = 12 drive^1.6`. Normalised so that a full-scale input stays at +-1 and a small
`g` is the identity (so a drive that is almost off is almost transparent); `g <= 0.001` is an exact bypass. The
half-band delay (about 0.3 ms) is constant, so Drive does not shift the sound when it moves. It is a symmetric curve,
so it adds odd harmonics and no DC; the loudness is trimmed by up to 2.2 dB at full drive (measured spread: 1.2 dB).

## Filter

`MatrixFilter` (the ZDF ladder and state-variable filter the Matrix and Wobble use; stable under per-sample
retuning), cutoff moved per sample in the octave domain by the filter envelope. Its internal saturators are rational
tanh, so the filter is fed a 0.04-scaled signal and scaled back: that keeps a sine through it at -72 dB harmonics
while a resonant peak still compresses. There is no keytracking and no floor at the fundamental: set `cutoff` below
the note and the note goes away, as on a real low-pass.

## Behaviour

- **Mono per instance.** A fresh note resets the filter, pitch envelope and (with `phasereset`) the phase, so identical
  inputs render identical samples. A retrigger of a sounding note is continuous: phase and filter carry, the amp
  envelope fades over about 2 ms before the new attack, and the new velocity is slewed (2 ms), so there is no step.
- **Note-off** starts the release of both envelopes; the note ends when the amp envelope does. `sustain = 0` makes a
  note that ends itself after `decay`.
- **Velocity** is a gain (`v (0.7 + 0.3 v)`); 0 is silent. Output is bounded for any parameters.
- **Allocation:** none in `render()` (tested).

## Test results (native/test/subbass_test.cpp, 48 kHz)

Measured on a bin-centred held note, 65536-sample FFT, everything not at the fundamental split into "harmonics"
(within 3 bins of 2f, 3f, ...) and "residual" (everywhere else: aliasing and noise), both relative to the fundamental.

- **Pure sine (Shape 0, Drive 0), 30 / 40 / 55 / 80 / 110 / 220 / 440 Hz:** harmonics -72.5 dB at every pitch,
  residual below -141 dB, mean below 1e-5, peak exactly Level x 0.6.
- **Oscillator alone, full saw at 440 Hz / 1.5 kHz / 2.9 kHz:** residual -149 dB (a naive saw is about -25 dB).
- **Aliasing, saw shape, filter open, residual re fundamental:**

| Pitch | Drive 0 | Drive 0.5 | Drive 1 |
|---|---|---|---|
| 55 Hz | -111 dB | -122 dB | -94 dB |
| 220 Hz | -78 dB | -80 dB | -65 dB |
| 440 Hz | -79 dB | -68 dB | -60 dB |
| 880 Hz | -82 dB | -65 dB | -58 dB |
| 1760 Hz | -87 dB | -63 dB | -55 dB |

  The drive-0 residual is the filter's own saturators acting on a rich wave, not the oscillator. Without the 2x
  oversampler the same rows were -62 / -50 / -47 / -44 / -41 dB at Drive 1.
- **A +24 st pitch sweep at full shape:** content above the band limit is over 80 dB below the rest.
- **DC:** every profile at shape 0 / 0.4 / 1 and Drive 0 / 1 at 30 and 220 Hz stays under 1e-3 (the only DC source
  is the filter's saturators skewing a phase-shifted wave; the oscillator and the drive are odd-symmetric).
- **Clicks:** note-on, note-off and a retrigger at 30 / 55 / 110 / 440 Hz, shape 0 and 1: the largest sample step is
  never above that of the held sound, except the attack slope of a 3 ms ramp on a slow sine, which stays under 4 % of
  full scale. Parameter jumps (level, cutoff, shape, drive, profile, filter type) mid-note stay within 1.3x the
  settled slope; a resonance jump on a high-Q filter rings (about 2.6x) as a resonant filter does.
- **Stability:** 60 random full-range parameter sets plus resonance 1 at both ends of the cutoff: finite, |y| <= 1.
- **Loudness:** Shape and Profile move a held note by -0.8..0 dB, Drive by 0..+1.2 dB.

## Presets

`SUB_BASS_PRESETS` in `src/runtime/types.ts` (mirrored by `native/test/subbass_test.cpp`, which also dumps them as WAVs
with `SUBBASS_WAV_DIR=<dir>`): `sub` (pure sine, filter open, short attack, 0.3 s release), `warm` (shape 0.3 warm
profile, LP at 500 Hz with a small envelope, mild drive), `punchy` (shape 0.5, 0.12 s filter decay, +7 st pitch
snap over 40 ms, drive 0.35), `acid` (shape 0.85 saw, resonance 0.75, +3.6 oct filter envelope, drive 0.4),
`aggressive` (bright profile at full shape, drive 0.75, resonance 0.55, short envelopes). Each stays bounded, DC-free
and click-free when played across the sub range and two octaves up.

## Client

```ts
const bass = runtime.createInstrument(InstrumentKind.SubBass, { bus });
for (const [p, v] of Object.entries(SUB_BASS_PRESETS.punchy)) runtime.setInstrumentParameter(bass, Number(p), v);
runtime.instrumentNoteOn(bass, { velocity: 0.9, duration: 0.5, pitch: 7 });  // +7 st over Root
```

The dev app's `SubBassPanel` (`src/app/components/SubBassPanel.tsx`) is the reference UI: OSC / FILTER / ENV / MOD
sections of knobs with Shape as the large one, presets, the waveform and harmonic bars from `harmonicWaveform`, and a
repeating note to audition.

## Not done / limits

- Monophonic per instance, like every instrument; chords need one instance per note.
- No keytracking, no unison/stereo (output is centred, panned by the note's `pan`), no second filter slope beyond
  LP 12/24, and no per-harmonic editing: by design.
- Switching `filtertype` mid-note crossfades; the ladder and SVF differ in phase, so a very short crossfade is audible
  as a slight tone change rather than a click.
