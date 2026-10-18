# PWM Beast instrument

A pulse-based character synth: animated, thick, a little unstable, able to go from a hollow sub to a
nasal lead to a distorted stab on one signal path. Source: `native/src/instruments/pwmbeast.h`; tests:
`native/test/pwmbeast_test.cpp`. Generic instrument contract: [instruments.md](./instruments.md).
Identity: `id "pwmbeast"`, name `PWM Beast`, category `synth`, `InstrumentKind.PwmBeast = 10`.

## Signal path

```
PolyBLEP pulse (zero-mean, width w) + sub sine (-1 oct)                       } width, sub
   ^ w = width + pwm * 0.42 * source                                          } pwm, rate, shape
   -> character: 1-pole edge rounding (2 x f0) -> sine fold (gain, asymmetry) -> blend over the pulse   } harmonics
   -> drive: asymmetric tanh, gain-compensated, rides the timbre envelope      } drive
   -> DC block (5 Hz)
   -> 24 dB low-pass (BiquadFilter), cutoff = base x keytrack x 2^(envamt x 5 oct x timbre env)   } cutoff, resonance, envamt
   -> x amp env^2 x velocity -> soft clip (|x| <= 1) -> Panner -> bus
```

Mono per instance (a chord = several instances). Reused: `AdsrEnvelope` (x2: amplitude and an
internal timbre envelope), `BiquadFilter`, `PercCore` (lifecycle, pre-note delay, pan),
`setTableParam`. PolyBLEP, tanh drive and soft clip are the same inline forms Bass/Pad use. Nothing new
in the engine, bus or ABI.

## Parameters

| Param | Range | Default | Effect |
|---|---|---|---|
| `tune` | 27.5-440 Hz | 55 | Root; note `pitch` is semitones on top. |
| `width` | 0.05-0.95 | 0.3 | Static pulse width. 0.5 = square. Below/above 0.5 are inverted images: same clean spectrum, different once shaped. |
| `pwm` | 0-1 | 0.5 | PWM depth: the width swings +-0.42 x pwm around `width` (clamped away from the edges). |
| `rate` | 0.1-200 Hz | 3.5 | PWM LFO rate: slow sweep up to an audio-rate-ish growl. |
| `shape` | 0-1 | 0.35 | PWM source. 0 = sine, 0.5 = skewed ramp, 1 = the note's timbre envelope (rate ignored). In between crossfades. |
| `harmonics` | 0-1 | 0.5 | Character stage: 0 = clean hollow pulse, up through thick and biting to folded and nasal. |
| `drive` | 0-1 | 0.4 | Asymmetric drive after the character stage, up to +34 dB, level-compensated. |
| `cutoff` | 60-8000 Hz | 500 | Low-pass, keytracked at half an octave per octave. |
| `resonance` | 0-1 | 0.45 | Low-pass Q 0.71 -> ~11.7 (quadratic). |
| `envamt` | 0-1 | 0.7 | How far the timbre envelope opens the filter (up to 5 octaves) and pushes the drive. |
| `sub` | 0-1 | 0.25 | Sine one octave down, a support not the sound (added before the shaper, so it is shaped too). |
| `attack` / `decay` / `release` | 1 ms-0.25 s / 30 ms-2 s / 10 ms-1.5 s | 3 ms / 0.3 s / 0.12 s | Amp ADSR (linear, shared implementation, applied squared). |
| `sustain` | 0-1 | 0.6 | Level the note holds at. |

All are LFO-layer targets by the default rules ([lfo-modulation.md](./lfo-modulation.md)), including
`rate`; Hz and seconds controls modulate in octaves. Note-time params (attack, release, decay) are read when a note
starts or ends. Sustained: pass `duration` on notes, or call `instrumentNoteOff`.

## How each stage earns its place

- **Pulse, built from two saws.** `pulse = saw(p - w) - saw(p)`, both PolyBLEP'd: both edges are band-limited
  and the mean is zero at any width (levels `1-w` for a fraction `w`, `-w` for the rest), so PWM never
  pumps DC through the filter. A pulse and its mirror (`w` vs `1-w`) are the same wave inverted: identical
  clean spectra, but the asymmetric stages after treat them differently, which is what the "inverted"
  half of the width range is for. Narrow pulses are levelled toward equal rms (gain capped at 1.6x).
  The effective width never gets closer than 0.03 (or 2.5 x phase increment at high pitch) to an edge.
- **PWM source.** One local LFO (phase reset per fresh note, so identical inputs render identical
  samples; a retrigger carries it) and the internal timbre envelope; `shape` crosses between them.
  With the envelope as source the width sweeps once per note (bright -> settled) instead of cycling:
  the "envelope-driven movement" case. The LFO runs per sample, so fast rates are smooth rather than
  stepped. No modulation matrix: a new source would be one more term in `src`.
- **Character stage (`harmonics`).** The key design fact: a memoryless shaper cannot change an ideal
  pulse, because a pulse takes two values and any curve maps two values to two values. So the pulse
  is first rounded (1-pole low-pass at 2 x f0, keytracked), giving the wave curvature between its
  plateaus, and *then* folded: `sin(pi/2 (gain x r + bias))`. The fold is bounded by 1, adds asymmetric
  (even + odd) harmonics, and because the rounded plateaus sit at different heights for different
  widths, **PWM moves the harmonics through a nonlinear map, not just a comb**. That is the audible
  difference from a plain pulse with a PWM LFO. The result is blended *over* the unrounded pulse
  (`mix = 1.6 x harmonics`, capped at 1), because folding alone can cancel the fundamental. Fold gain
  1 -> 5 and bias 0 -> 0.3 with the dial; eased above ~1.5 kHz (it would alias).
- **Drive.** Asymmetric tanh (bias grows with drive) for even-harmonic density, with compensation
  `P / (tanh(g P + bias) - tanh(bias))` at a reference peak so more drive adds density, not level
  (measured: rms stays within ~0.6-1.7x across the dial). The drive itself follows the timbre envelope
  (0.6 -> 1.0 of its setting), so a note bites at the onset and settles. Inline rather than
  `Saturation`: that node is a DSPNode with a static drive and a ~38 Hz DC corner, which this does not want.
- **Filter and envelope.** 24 dB low-pass, Q from `resonance`, makeup so resonance does not just add
  level. The timbre envelope (same attack, decay x 0.6, sustain x 0.3, same release) opens the cutoff
  by up to 5 octaves, scaled slightly by velocity, and closes faster than the level does: the note
  starts with the harmonics exposed, the filter closes, and the sustained PWM tone remains. The cutoff
  never drops below 0.9 x f0, so the filter can darken the note but never delete it.

## How it differs from Pulse

The repo's `Pulse` is a *rhythmic* synth (tempo-locked pump/repeat on three detuned saws): its
character is movement on the beat grid, with a conventional oscillator -> drive -> filter path
underneath. PWM Beast has no tempo behaviour; its character is in the *timbre path*: pulse width as the
primary modulation target, a fold stage that turns width into harmonic motion, drive that follows the
note's own envelope, and a filter driven hard by that same envelope. Switching PWM off (`pwm 0`) leaves
a rounded/folded/driven pulse whose waveform differs from a clean one (tested), so the instrument does
not collapse into "a pulse with an LFO".

## Decisions worth knowing

- **Voicing constants** (`kRoundRatio`, fold gain/bias/mix, `kMaxDriveDb`, `kLevel`) were set by
  measurement (spectra and level across width x harmonics) and level-matched to Bass (held rms ~0.18).
  They have not been auditioned by ear; the likely tuning candidates are the default `harmonics`, `drive`
  and `resonance`, and `kFoldBias`.
- **Fold level.** The folded wave has less rms than the pulse at the top of the dial (about -4 dB,
  fundamental thinned); the blend and a small `foldComp` keep it within ~5 dB. Drive compensation then
  works from there.
- **Retrigger.** A note on a sounding instrument carries phase, LFO, filter and envelope levels (no
  reset, no discontinuity). No legato/slide: out on purpose, as in Hypersaw.

## Cost and limits

Per sample: two PolyBLEP saws, a sine (LFO) and a sine (sub), a sine fold, a tanh, and the 24 dB
filter; per sub-block (16 samples): a few `pow`/`exp2`. No allocation in `render()` (tested).
Not oversampled: the fold and drive alias at high pitch with extreme settings, mitigated by easing the
fold above 1.5 kHz and by the low-pass. Oversampling the character+drive stages is the upgrade path.
