# Matrix

A subtractive synthesizer with a patch matrix. The voice is the usual one, VCO into VCF into VCA,
with two LFOs and two envelopes. On top of it sits a 7 x 6 grid of signed amounts: any source can
drive any destination, including the audio-rate sources (noise, the oscillator, the filter's own
output). Patching the filter output back into the filter cutoff or the oscillator pitch is feedback,
and it is one cell of the grid like any other.

The engine has no patching UI and no cables. Every cell is an ordinary instrument parameter, so a
client draws the matrix however suits its screen: a grid, a flow diagram, a synth panel or a few
chips in a groovebox track. The client-side model and feedback analysis live in
`src/runtime/patchMatrix.ts`; see [matrix-client-guide.md](./matrix-client-guide.md).

Source: `native/src/instruments/matrix.h`, `native/src/dsp/matrix_filter.h`; tests:
`native/test/matrix_test.cpp`, `test/patchMatrix.test.ts`. Generic instrument contract:
[instruments.md](./instruments.md). Identity: `id "matrix"`, name `Matrix`, category `synth`,
`InstrumentKind.Matrix = 18`. An original webdsp voice; it does not model any other synthesizer.

## Signal path

```
VCO  saw | pulse | triangle      pitch = tune x note x 2^(pitch patches)
  + noise (mix)                  pulse width = pwm + 0.45 x pwm patches
  -> VCF  LP24 ladder | LP12 | BP | HP     drive -> filter, cutoff = cutoff x keytrack x 2^(5 x cutoff patches)
  -> VCA  gain = level + level patches (0..1), applied squared, x velocity -> soft clip (|x| <= 1) -> Panner -> bus

sources:       LFO 1, LFO 2   -1..1, free running
               ENV 1, ENV 2   0..1, ADSR
               noise          white, -1..1
               VCO out        the oscillator, before the mix
               VCF out        the filter output, before the VCA
destinations:  pitch, pulse width, cutoff, resonance, drive, level
```

A destination's value is its own control plus the sum of `amount x source` over its patches, then
clamped to the range. A patch is therefore three things and nothing else: a source, an amount, a
destination. An amount of 0 is no patch: the note renders exactly as if the cell did not exist.

### What 100% does

| Destination | A patch of amount 1 with a source at its peak moves it by |
|---|---|
| Pitch | 1 octave |
| Pulse width | 0.45 of the cycle (pulse wave only; kept inside 0.02..0.98) |
| Cutoff | 5 octaves |
| Resonance | 0.5 |
| Drive | 0.6 |
| Level | the full range (a gate: ENV 1 at 100% opens the amp as the envelope rises) |

Amounts are signed: a negative amount inverts the source. Several patches to one destination add.

### Audio-rate sources and feedback

Noise, the VCO and the VCF output are patched exactly like an LFO; they are just fast. VCO into
cutoff is audio-rate filter FM, noise into cutoff roughens the tone, VCO into level is amplitude
modulation.

A patch that carries a signal back to somewhere upstream of where it was taken is feedback:

- VCF out into cutoff, resonance, drive, pulse width or pitch (the filter modulates what feeds it);
- VCO out into pitch or pulse width (the oscillator modulates itself).

The loop is closed with **one sample of delay**: a destination that is upstream of the source hears
that source's previous sample. Every other patch uses the source's current sample, so patching VCO
into cutoff or VCF out into level involves no delay. The loop cannot run away: the filter limits its
signal to +-1 internally, the filter is a zero-delay-feedback design that tolerates its cutoff moving
every sample, and the final soft clip is bounded by exactly 1. `native/test/matrix_test.cpp` drives
random patches, every cell at +1 and at -1, full resonance and full drive, and checks the output
stays finite and within +-1.

A cell can be feedback in one patch and not in another, because loops can close through other
patches. `patchMatrix.ts` does the graph analysis for a client.

## Filter (`dsp/matrix_filter.h`)

| `mode` | Filter |
|---|---|
| 0 | LP 24 dB: four one-pole stages in a feedback loop (Moog-style ladder). Resonance 0..1 is loop gain 0..4; at 1 the ladder rings on its own. |
| 1 | LP 12 dB: state-variable filter, Q up to about 20. |
| 2 | Band-pass, unity gain at the centre. |
| 3 | HP 12 dB. |

All modes are zero-delay-feedback (trapezoidal) designs, recomputed every sample, so audio-rate
modulation of the cutoff is stable. The cutoff is clamped to 20 Hz .. 0.45 x sample rate. `drive`
saturates the signal ahead of the filter (transparent at 0). The ladder gives most of the level it
loses to resonance back.

## Parameters

Ids 0..21 are the voice; ids 22..63 are the matrix. The catalogue (`getInstrumentParams`) lists all
64, then the shared `gain` row (id 1000).

| Param | Range | Default | Effect |
|---|---|---|---|
| `tune` | 27.5-1760 Hz | 110 | Root; a note's `pitch` is semitones on top. |
| `wave` | step 0-2 | 0 | Saw, pulse, triangle. |
| `pwm` | 0.05-0.95 | 0.5 | Pulse width (pulse wave only). |
| `noise` | 0-1 | 0 | Noise mixed into the filter input. |
| `cutoff` | 20-18000 Hz | 1200 | Before keytrack and patches. |
| `resonance` | 0-1 | 0.25 | |
| `drive` | 0-1 | 0 | Saturation ahead of the filter; gain 1..8 (quadratic). |
| `mode` | step 0-3 | 0 | LP24, LP12, BP, HP. |
| `keytrack` | 0-1 | 0.5 | Cutoff follows note pitch ^ keytrack. |
| `level` | 0-1 | 0 | VCA level before patches. 0 = the amp opens only by patch. |
| `lfo1rate` / `lfo2rate` | 0.05-40 Hz | 2 / 0.5 | |
| `lfo1wave` / `lfo2wave` | step 0-4 | 0 / 1 | Sine, triangle, saw, square, sample & hold. |
| `env1attack` / `decay` / `sustain` / `release` | 0.5 ms-4 s / 10 ms-4 s / 0-1 / 5 ms-4 s | 3 ms / 0.4 s / 0 / 120 ms | ENV 1. Also sets how long a note lives (below). |
| `env2attack` / `decay` / `sustain` / `release` | same | 2 ms / 0.25 s / 0 / 120 ms | ENV 2. |
| `<source>_<dest>` (42 cells) | -1..1 | 0 | The matrix. Source `lfo1 lfo2 env1 env2 noise vco vcf`, destination `pitch pwm cutoff resonance drive level`. Id = `22 + source * 6 + destination`. |

The defaults are the init patch: a plain voice with `env1_level` = 1 (the amp opens with ENV 1) and
`env2_cutoff` = 0.4 (a short filter sweep). Everything else in the matrix is 0.

Steps (`wave`, `mode`, the LFO waves) and the 42 cells are not LFO modulation targets; every other
control is (generic modulation, [lfo-modulation.md](./lfo-modulation.md)). The cells can be
automated like any parameter (scheduled params, MIDI-style per-step changes), so a patch can change
from step to step.

### Notes and lifetime

ENV 1 is the note's lifetime: a note ends when ENV 1 has finished its release, or when ENV 1 has
fallen to a sustain of 0. With `env1sustain` 0 (the default) a note is a self-ending one-shot. With
sustain above 0 the note is held: pass `duration` on the note or call `instrumentNoteOff`. This is
true whatever ENV 1 is patched to; a patch that takes ENV 1 off the amp (and a base `level` above 0)
leaves a gate of that length.

Velocity scales the final level only. A retrigger on a sounding note is continuous (oscillator and
LFO phases, filter state and envelope levels carry); a fresh note resets everything so identical
inputs render identical samples. Mono per instance, like every instrument here; not tempo-synced.

### Smoothing

Patch amounts move toward their target every 32 frames (they settle within a few milliseconds), so switching a patch
on or off during a note does not click. Other parameters are read per 32-frame step like the rest of
the engine. A silent matrix costs nothing per sample; with every cell active the voice uses about
twice the CPU of the default patch (measured natively: 0.35% vs 0.63% of a core at 48 kHz).

## Sound-design starting points

Not presets (clients own those). Amounts are the cell values.

| Sound | Direction |
|---|---|
| Plain subtractive | The default patch. |
| Wobble | `cutoff` 600, `resonance` 0.5, `lfo1_cutoff` 0.55, `lfo1rate` 2 |
| Pluck | `cutoff` 500, `env2_cutoff` 0.7, `env2_resonance` 0.2, `env1decay` 0.25 |
| Vibrato lead | `wave` 1, `env1sustain` 1, `lfo1_pitch` 0.03, `lfo1rate` 5.5 (pass `duration`) |
| Moving pulse | `wave` 1, `lfo2_pwm` 0.6, `lfo2rate` 0.4 |
| Filter feedback | `resonance` 0.6, `vcf_cutoff` 0.3-0.6: the filter output opens and closes its own cutoff, which raises the level and changes the waveshape; negative amounts do the opposite. |
| Pitch feedback | `vcf_pitch` 0.1-0.3 with a little `resonance`: the filter bends the oscillator. Small amounts detune, large ones become FM noise. |
| Audio-rate FM on the filter | `vco_cutoff` 0.3-0.6, `cutoff` 900, plus `noise_cutoff` 0.2 for roughness |
| Ring-like tone | `level` 0.3, `vco_level` 0.8 |

## Limitations

- Feedback is delayed by one sample, so its colour depends on the sample rate (the engine runs at the
  AudioContext's rate).
- The triangle is naive (not band-limited); its harmonics fall as 1/n^2 so the alias floor is low, but
  a high triangle with an open filter is not clean. Saw and pulse use PolyBLEP.
- The filter's resonance is bounded by a limiter, not by a model of an analogue circuit; extreme
  settings are loud and rough rather than faithful.
- One voice per instance. Overlapping notes need more than one instance.
- No tempo sync on the LFOs. The generic modulation layer's LFOs can sync (they act on the voice
  controls, not the cells).
- Ear-tuning of defaults, `kLevel` and the depths is still to do.
