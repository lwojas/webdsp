# Matrix

A semi-modular subtractive synthesizer, in the spirit of a Moog Mother-32. The modules are fixed (one
oscillator, one filter, one amplifier, two LFOs, two envelopes, a noise source) and they are joined only by
**patch points**: eight outs and eleven ins, with a matrix of 88 cells between them. The ordinary VCO into
VCF into VCA voice is simply the default patch, wired the way a Mother-32 is normalled, so every connection,
the audio path included, can be removed or redirected: the oscillator straight into the amp, noise through
the filter, the filter back into its own input, the filter output into its own cutoff.

The engine has no patching UI and no cables to draw. Every cell is an ordinary instrument parameter, so a
client draws the matrix however suits its screen: **outs across the top, ins down the side** (a grid), a flow
diagram, a synth panel or a few chips in a groovebox track. The client-side model and feedback analysis live
in `src/runtime/patchMatrix.ts`; see [matrix-client-guide.md](./matrix-client-guide.md).

Source: `native/src/instruments/matrix.h`, `native/src/dsp/matrix_filter.h`; tests:
`native/test/matrix_test.cpp`, `test/patchMatrix.test.ts`. Generic instrument contract:
[instruments.md](./instruments.md). Identity: `id "matrix"`, name `Matrix`, category `synth`,
`InstrumentKind.Matrix = 18`. An original webdsp voice; it does not model any other synthesizer.

## The patch points

| Outs (columns) | | Ins (rows) | What the in does with its sum |
|---|---|---|---|
| `vco` | oscillator | `pitch` | pitch, 100% = 1 octave |
| `noise` | white noise | `pwm` | pulse width, 100% = ±0.45 of the cycle |
| `lfo1`, `lfo2` | LFOs, -1..1 | `vcfin` | the filter's **audio input** |
| `env1`, `env2` | envelopes, 0..1 | `cutoff` | cutoff, 100% = 5 octaves |
| `vcf` | filter output | `resonance` | ±0.5 |
| `vca` | amp output | `drive` | ±0.6 |
| | | `vcain` | the amp's **audio input** |
| | | `level` | the amp's level (control) input: gain = level + cables, 0..1, applied squared |
| | | `lfo1rate`, `lfo2rate` | LFO rate, 100% = 3 octaves |
| | | `out` | the voice output |

Each cell is a signed **amount** from -1 to 1 (0 = no cable). Everything patched into an in is **summed**, each
out scaled by its amount, so an in with several cables is a small mixer, and one out can feed several ins at
different amounts. For an audio in (`vcfin`, `vcain`, `out`) the amount is a level; for a control in it is a
depth (the table's 100% is a cable of amount 1 with the out at its peak). Amounts are signed: a negative amount
inverts. The knobs set where each in rests before the cables arrive.

### The default patch

The cells' default values are the ordinary voice:

| Cable | Amount | Does |
|---|---|---|
| `vco_vcfin` | 1 | oscillator into the filter |
| `vcf_vcain` | 1 | filter into the amp |
| `vca_out` | 1 | amp to the output |
| `env1_level` | 1 | ENV 1 opens the amp |
| `env2_cutoff` | 0.4 | ENV 2 sweeps the cutoff |

Take any of them away and that part of the voice stops: with no `vca_out` nothing reaches the output.
`native/test/matrix_test.cpp` checks that zeroing every other cell is exactly the default voice, that removing
each of these five silences it, and that the audio path can be re-routed.

### Feedback and the order the voice runs in

Each sample the voice computes noise, LFO 1, LFO 2, ENV 1, ENV 2, then the VCO, the VCF, the VCA and the output,
in that order. An in hears an out of a module **earlier** in that order in the same sample, and an out of a
**later** module (or of its own module) **one sample late**. So VCF out into the VCF cutoff, VCF out into the
pitch, VCO out into its own pitch, VCA out into the filter input and the like are all well defined loops with
one sample of delay, while VCO into the cutoff, or VCF out into the amp level, use the current sample.

Nothing can run away: the filter limits its signal to ±1 internally, it is a zero-delay-feedback design that
tolerates its cutoff moving every sample, and the output is soft-clipped to exactly ±1. The tests drive random
patches, every cable at +1 and at -1, full resonance and drive, and check the output stays finite and bounded.
`patchMatrix.ts` does the graph analysis for a client (which cables are part of a loop, and where a signal
ends up); a cell can be feedback in one patch and not in another, because a loop closes through other cables.
A cable that runs against the order but closes no loop (VCA out into the pitch with the VCO not patched into
anything) is simply delayed by one sample.

## Signal path

There is none beyond the cables. In the default patch:

```
VCO (saw|pulse|triangle) -> VCF in -> VCF (LP24 ladder|LP12|BP|HP) -> VCA in -> VCA -> OUT -> soft clip -> Panner -> bus
ENV 1 -> VCA level        ENV 2 -> cutoff       LFOs, noise, VCO, VCF, VCA available to every in
```

The VCO's pitch is `tune x note x 2^(cables)`; the cutoff is `cutoff x keytrack x 2^(5 x cables)`; the amp's
gain is `clamp(level + cables, 0, 1)` squared. Velocity scales the output level.

## Filter (`dsp/matrix_filter.h`)

| `mode` | Filter |
|---|---|
| 0 | LP 24 dB: four one-pole stages in a feedback loop (Moog-style ladder). Resonance 0..1 is loop gain 0..4; at 1 the ladder rings on its own. |
| 1 | LP 12 dB: state-variable filter, Q up to about 20. |
| 2 | Band-pass, unity gain at the centre. |
| 3 | HP 12 dB. |

All modes are zero-delay-feedback (trapezoidal) designs, recomputed every sample, so audio-rate modulation of the
cutoff is stable. The cutoff is clamped to 20 Hz .. 0.45 x sample rate. `drive` saturates the signal ahead of the
filter (transparent at 0; gain 1..8, quadratic). The ladder gives most of the level it loses to resonance back.

## Parameters

Ids 0..20 are the module knobs; ids 21..108 are the matrix (`21 + out * 11 + in`). The catalogue
(`getInstrumentParams`) lists all 109, then the shared `gain` row (id 1000).

| Param | Range | Default | Effect |
|---|---|---|---|
| `tune` | 27.5-1760 Hz | 110 | Root; a note's `pitch` is semitones on top. |
| `wave` | step 0-2 | 0 | Saw, pulse, triangle. |
| `pwm` | 0.05-0.95 | 0.5 | Pulse width before cables (pulse wave only). |
| `cutoff` | 20-18000 Hz | 1200 | Before keytrack and cables. |
| `resonance` | 0-1 | 0.25 | |
| `drive` | 0-1 | 0 | Saturation ahead of the filter. |
| `mode` | step 0-3 | 0 | LP24, LP12, BP, HP. |
| `keytrack` | 0-1 | 0.5 | Cutoff follows note pitch ^ keytrack. |
| `level` | 0-1 | 0 | VCA level before cables. 0 = the amp opens only by cable (ENV 1 by default). |
| `lfo1rate` / `lfo2rate` | 0.05-40 Hz | 2 / 0.5 | Before the rate cables. |
| `lfo1wave` / `lfo2wave` | step 0-4 | 0 / 1 | Sine, triangle, saw, square, sample & hold. |
| `env1attack` / `decay` / `sustain` / `release` | 0.5 ms-4 s / 10 ms-4 s / 0-1 / 5 ms-4 s | 3 ms / 0.4 s / 0 / 120 ms | ENV 1. Also sets how long a note lives (below). |
| `env2attack` / `decay` / `sustain` / `release` | same | 2 ms / 0.25 s / 0 / 120 ms | ENV 2. |
| `<out>_<in>` (88 cells) | -1..1 | 0 (five defaults above) | The matrix. Out `vco noise lfo1 lfo2 env1 env2 vcf vca`, in `pitch pwm vcfin cutoff resonance drive vcain level lfo1rate lfo2rate out`. |

There is no noise knob: noise is a cable (`noise_vcfin` is how much noise enters the filter, `noise_out` puts it
in the output).

Steps (`wave`, `mode`, the LFO waves) and the 88 cells are not LFO modulation targets; every other control is
(generic modulation, [lfo-modulation.md](./lfo-modulation.md)). The cells can be automated like any parameter
(scheduled params), so a patch can change from step to step.

### Notes and lifetime

ENV 1 is the note's lifetime: a note ends when ENV 1 has finished its release, or when ENV 1 has fallen to a
sustain of 0. With `env1sustain` 0 (the default) a note is a self-ending one-shot. With sustain above 0 the note
is held: pass `duration` on the note or call `instrumentNoteOff`. This holds whatever ENV 1 is cabled to: a patch
that takes ENV 1 off the amp (and a base `level` above 0) leaves a gate of that length.

A retrigger on a sounding note is continuous (oscillator and LFO phases, filter state and envelope levels
carry); a fresh note resets everything so identical inputs render identical samples. Mono per instance, like
every instrument here; not tempo-synced.

### Smoothing and cost

Cable amounts move toward their target every 32 frames (they settle within a few milliseconds), so adding or
removing a cable during a note, the audio path included, does not click. An in with no cable costs nothing per
sample; with every cell active the voice uses about twice the CPU of the default patch (measured natively:
under 1% of a core at 48 kHz).

## Sound-design starting points

Not presets (clients own those). Cables are written `out > in amount`.

| Sound | Direction |
|---|---|
| Plain subtractive | The default patch. |
| Wobble | `cutoff` 600, `resonance` 0.5, `lfo1 > cutoff` 0.55, `lfo1rate` 2 |
| Pluck | `cutoff` 500, `env2 > cutoff` 0.7, `env2 > resonance` 0.2, `env1decay` 0.25 |
| Vibrato lead | `wave` 1, `env1sustain` 1, `lfo1 > pitch` 0.03, `lfo1rate` 5.5 (pass `duration`) |
| Moving pulse | `wave` 1, `lfo2 > pwm` 0.6, `lfo2rate` 0.4 |
| Filter feedback | `resonance` 0.6, `vcf > cutoff` 0.3-0.6: the filter output opens and closes its own cutoff, which raises the level and changes the waveshape; negative amounts do the opposite. |
| Pitch feedback | `vcf > pitch` 0.1-0.3 with a little `resonance`: the filter bends the oscillator. Small amounts detune, large ones become FM noise. |
| Filter into itself | `vcf > vcfin` 0.3-0.6: the output returns to the input, thickening the resonance. |
| Noise voice | `vco > vcfin` 0, `noise > vcfin` 1: a filtered noise hit; play it with `env2 > cutoff`. |
| Skip the filter | `vcf > vcain` 0, `vco > vcain` 1: the raw oscillator through the amp. |
| Audio-rate FM on the filter | `vco > cutoff` 0.3-0.6, `cutoff` 900, plus `noise > cutoff` 0.2 for roughness |
| Ring-like tone | `level` 0.3, `vco > level` 0.8 |
| LFO as a tone | `vca > out` 0, `lfo1 > out` 0.8, `lfo1rate` 40, `env1 > lfo1rate` 0.5: an oscillator whose pitch the envelope bends |

## Limitations

- A loop is delayed by one sample, so its colour depends on the sample rate (the engine runs at the
  AudioContext's rate).
- The triangle is naive (not band-limited); its harmonics fall as 1/n^2 so the alias floor is low, but a high
  triangle with an open filter is not clean. Saw and pulse use PolyBLEP.
- The filter's resonance is bounded by a limiter, not by a model of an analogue circuit; extreme settings are
  loud and rough rather than faithful.
- One voice per instance. Overlapping notes need more than one instance.
- The eight outs and eleven ins are a fixed set, not a general modular system: there are no mults, attenuators
  or extra modules beyond the amounts per cable.
- No tempo sync on the LFOs. The generic modulation layer's LFOs can sync (they act on the module knobs, not on
  the cells).
- Ear-tuning of defaults, `kLevel` and the depths is still to do.
