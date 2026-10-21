# Edge

A clean, sharp, transient-focused synthesizer for plucks, stabs, sequenced riffs and bright leads.
It is deliberately **not** another analogue-style "oscillator into a resonant filter" voice: the
brightness lives in the oscillator, a short signed pitch transient makes the pluck, and the filter
is a pair of stages with routing that you can leave flat. There is no drive stage and the default
patch uses almost no resonance. Source: `native/src/instruments/edge.h`, `native/src/dsp/edge_osc.h`,
`native/src/dsp/dual_filter.h`; tests: `native/test/edge_test.cpp`. Generic instrument contract:
[instruments.md](./instruments.md). Identity: `id "edge"`, name `Edge`, category `synth`,
`InstrumentKind.Edge = 17`.

An original webdsp voice. The Access Virus's configurable dual filter was an inspiration for
routing as a design axis only (see [dual-filter-proposal.md](./dual-filter-proposal.md)); nothing
here reproduces it, and nothing here models any other synthesizer.

## Why it sounds different

The existing synths are all some form of PolyBLEP oscillator -> resonant low-pass -> envelope,
with resonance and drive supplying the edge. Edge changes the three things that define that sound:

| | Existing machines | Edge |
|---|---|---|
| Where brightness comes from | A filter opening (and resonance, drive) | The oscillator's **harmonic count**, swept by an envelope; the filter can stay flat |
| What makes a note a pluck | A decaying resonant filter sweep | A **pitch transient** (ms-scale, signed) plus a bright-then-pure oscillator |
| Filter | One low-pass (Pulse/Bass/PWM Beast 24 dB, others 12 dB) | **Two stages**, each LP12/LP24/HP12/BP, single / series / parallel; window and scoop shapes |
| Nonlinearity | tanh drive, folding, feedback | None. A bounded soft clip at the very end only |

## Signal path

```
band-limited oscillator (+ detuned copy)              shape: saw -> square -> pulse
   brightness = harmonic count 1..256, swept by the tone envelope
   pitch = tune x note x 2^((pitch transient + vibrato) / 12)
     -> DualFilter: stage A, stage B in {LP12, LP24, HP12, BP}; single | series | parallel
        cutoff A = cutoff x keytrack x 2^(tone env x filterenv x 6 + LFO x lfofilter x 3)
        cutoff B = cutoff A x 2^spread
     -> x amp env^2 x velocity x resonance compensation -> soft clip (|x| <= 1) -> Panner -> bus

envelopes:  amp ADSR (applied squared) | tone (exponential: filter + brightness) | pitch transient (exponential)
LFO:        one sine -> pitch and/or both cutoffs
```

Mono per instance (chords and overlap = several instances). A retrigger is continuous (oscillator
phases, filter state and amplitude level carry; the transient and tone envelopes restart); a fresh
note resets everything, so identical inputs render identical samples. Not tempo-synced. No
allocation after `configure()`.

### The oscillator (`dsp/edge_osc.h`)

A set of 16 single-cycle tables, each a falling saw built from exactly H harmonics
(H = 1, 2, 3, 4, 6, 8, 11, 16, 23, 32, 45, 64, 91, 128, 181, 256), Lanczos-windowed so the hard
cutoff rings less. Picking a table picks how many harmonics the oscillator has, so **brightness is an
oscillator control**: a pure sine at one end, a 256-partial saw at the other, crossfaded smoothly
between table levels in between. It is alias-free by construction: harmonics above 0.45 fs are never
used (quantised down to a table level, tracking the highest pitch of each 16-sample step, so a pitch
transient cannot fold back). Reading is linear interpolation of a 2048-point table. The table set is
a shared read-only singleton built in `configure()` (never in `render()`).

Shape is a second read of the same table: `S(p) - k S(p + w)`. `k = 0` is a saw; `k = 1, w = 0.5` is a
square; `k = 1, w -> 0.12` is a narrow pulse. Output RMS is held constant across **brightness and
shape** (each table normalised, and the morph's loudness computed from the harmonic weights), so
sweeping either changes timbre, not level. Detune adds a second oscillator that fades in over the
first 10 cents (0 = a single oscillator, exactly), then beats against the first.

Hard sync was considered and left out: a band-limited sync needs a minBLEP-class correction at
every reset, and the harmonic-count control already provides "brightness without a filter", which
is what sync would have been used for. Phase control is the `shape` offset `w`; a fresh note always
starts oscillator 1 at phase 0 and the detuned copy at 0.37.

### The filter pair (`dsp/dual_filter.h`)

Each stage is one of **LP 12 dB, LP 24 dB, HP 12 dB, band-pass** (the existing `BiquadFilter` for
LP/HP, `PercBandpass` for the band-pass). Routing:

| Routing | Signal | Use |
|---|---|---|
| 0 single | A only; B is not run | A plain filter; cheapest |
| 1 series | A then B | LP then HP = a **window** (band-limited, vocal/reedy); LP then LP = steeper |
| 2 parallel | A and B both see the oscillator, summed (x0.6) | LP below HP = a **scoop** (hollow, phaser-like); equal cutoffs LP + HP = an exact notch |

`cutoff` is stage A; `spread` puts stage B `spread` octaves above (positive) or below (negative) it,
so one gesture moves the window and the envelope slides both edges together. Resonance is one control
feeding both stages:

- **Series resonance multiplies** (two peaks of Q at one cutoff give Q x Q), so B follows A only
  partway: `Q_B = 0.707 + 0.3 x (Q_A - 0.707)`. A resonates, B colours.
- **Parallel resonance adds**, so both stages take the full Q.
- **Compensation** is the same shape as Bass's `1/(1 + 0.12 (Q - 0.707))`, applied to the effective
  peak: `Q_A x Q_B / 0.707` in series, the larger Q in parallel, band-pass stages contribute nothing
  (constant peak gain). Measured: a fully resonant series pair peaks at roughly the same compensated
  level as a single resonant stage, not 60x.
- **Band-pass level:** a band-pass passes a sliver of a saw and a narrower one passes less, so it is
  given a make-up gain growing as sqrt(Q), 1.5x when widest to about 6x at full resonance.
- **Stability:** a biquad whose cutoff jumps across many octaves between blocks at high Q can diverge
  (the same effect documented in `filter-modulation.md`). Each stage moves its cutoff by at most half
  an octave per 16-sample control step, and a fresh note jumps straight to its target. Smooth
  envelopes and the LFO are far inside this; it only acts on abrupt moves. Tested with random
  full-range cutoff jumps at maximum resonance on all 48 mode pairs and routings.

Split routing (oscillator 1 -> A, oscillator 2 -> B) was evaluated and **not** included: Edge's
second oscillator is a detuned copy, and filtering two copies separately adds little over
parallel. It would earn its place with an independent second oscillator (different interval or
waveform), which is a different instrument.

## Parameters

| Param | Range | Default | Effect |
|---|---|---|---|
| `tune` | 27.5-1760 Hz | 220 | Root; a note's `pitch` is semitones on top. |
| `shape` | 0-1 | 0 | Saw (0), square (0.5), narrow pulse (1). |
| `bright` | 0-1 | 0.55 | Oscillator brightness, harmonic count 1 (sine) to 256; each 1/8 of the knob doubles it. |
| `brightenv` | -1..1 | 0.4 | Tone-envelope depth on brightness: positive = bright onset that settles. |
| `detune` | 0-50 cents | 0 | Second oscillator; fades in over the first 10 cents. |
| `pitchenv` | -24..24 st | 3 | Pitch transient start offset. Positive starts above (zap), negative below (bloop). |
| `pitchdecay` | 2-400 ms | 25 ms | Time for the transient to fall ~40 dB. |
| `routing` | step 0-2 | 1 | Single, series, parallel. |
| `filtera` / `filterb` | step 0-3 | 0 / 2 | LP12, LP24, HP12, band-pass. |
| `cutoff` | 40-16000 Hz | 3000 | Stage A, keytracked (note pitch ^ 0.7). |
| `spread` | -4..4 oct | -3 | Stage B relative to A. |
| `resonance` | 0-1 | 0.1 | Q 0.71 -> ~13.7 (quadratic); tied across series stages. |
| `filterenv` | -1..1 | 0.5 | Tone envelope on both cutoffs, +-6 octaves. |
| `tonedecay` | 10 ms-2 s | 180 ms | Tone envelope (filter and brightness), ~40 dB. |
| `lforate` | 0.1-20 Hz | 5 | Sine LFO. |
| `lfopitch` | 0-1 | 0 | LFO -> pitch, up to +-1.5 semitones (vibrato). |
| `lfofilter` | 0-1 | 0 | LFO -> both cutoffs, up to +-3 octaves. |
| `attack` / `decay` / `sustain` / `release` | 0.5 ms-0.25 s / 10 ms-4 s / 0-1 / 5 ms-2 s | 1 ms / 0.3 s / 0 / 80 ms | Amp ADSR (linear segments, applied squared). Sustain 0 = self-ending one-shot. With sustain > 0 the note is held: pass `duration` or call `instrumentNoteOff`. |

Steps (`routing`, `filtera`, `filterb`) are not LFO modulation targets; everything else is (generic
modulation, [lfo-modulation.md](./lfo-modulation.md)).

### Velocity

Level (linear), plus a little character: the tone-envelope depth on filter and brightness scales
x(0.6..1), and a soft hit is up to 0.15 darker on the brightness knob. Smooth and monotonic.

## Sound-design starting points

Not presets (clients own those). With `resonance` low and `routing` 0 unless noted.

| Sound | Direction |
|---|---|
| Clean pluck | `bright` 0.3, `brightenv` 0.6, `tonedecay` 70 ms, `pitchenv` 4, `pitchdecay` 12 ms, `decay` 0.18, filter open (`cutoff` 16000, `filterenv` 0). Bite with no filter at all. |
| Techno stab | `shape` 0.5, `bright` 0.7, `detune` 12, series LP24 -> HP, `cutoff` 2500, `spread` -2.5, `filterenv` 0.6, `tonedecay` 120 ms, `decay` 0.22 |
| Sequenced riff | the default patch; vary `cutoff`/`bright` per step |
| Bright mono lead | `shape` 0.8, `bright` 0.85, `detune` 14, `pitchenv` 1, `pitchdecay` 30 ms, `lfopitch` 0.25, `lforate` 5.5, `sustain` 1 (pass `duration`) |
| Sharper digital lead | `bright` 1, `shape` 0.5-1, `pitchenv` 0, parallel LP + HP scoop |
| Bloop / blip | `pitchenv` -12, `pitchdecay` 60 ms, `bright` 0.2 |
| Reedy / vocal | series LP then HP (window), `spread` -2, `cutoff` 1500, `filterenv` 0.4 |
| Hollow stab | parallel LP below HP: `routing` 2, `filtera` 0, `filterb` 2, `spread` +3 |
| Synth bass pluck | `tune` 55, `shape` 0.2, `bright` 0.6, `routing` 0 LP24, `cutoff` 500, `filterenv` 0.5, `decay` 0.25 |

## Limitations

- Parameters are read per render block (as every instrument here); there is no extra smoothing layer.
  The filter cutoffs and brightness update every 16 samples.
- The harmonic cap quantises to a table level, so at high pitch the top partial can sit up to ~40 %
  below Nyquist.
- The pitch transient is applied per sample but the brightness choice per control step; a very short
  transient on a very bright note therefore uses the cap of its highest pitch for the whole step.
- Ear-tuning of defaults, `kLevel` and the ranges is still to do (set `EDGE_WAV_DIR=<dir>` when running
  `edge_test` to dump the sound-design cases as WAVs).
