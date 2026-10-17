# FM instrument

`InstrumentKind.Fm` (`id: "fm"`, category `synth`) — a small, fixed two-operator phase-modulation
voice for bass and lead. Source: `native/src/instruments/fm.h`; tests: `native/test/fm_test.cpp`.
Design rationale and the rejected alternatives: [`fm-instrument-proposal.md`](./fm-instrument-proposal.md).
Research input: [`synth-dsp/techniques.md`](./synth-dsp/techniques.md) #4 (and #5 for the feedback idea).

## What it is for

The existing synths are subtractive (oscillator -> filter): brightness is *removal*. FM *generates*
partials from one control, so it makes sounds they cannot:

- **Hollow / odd-harmonic and inharmonic colours** (clarinet-square, glassy, metallic, bell). A
  low-pass cannot take out 2f, 4f, 6f and keep 3f, 5f, 7f.
- **A sine-pure sub that suddenly speaks** with a bright, glassy attack: the index envelope puts new
  partials in at the onset and takes them out again, with the level untouched.
- **Velocity-brightness** that is real spectral change, and a **clean digital bass** with a controllable
  "slap".

## Signal path

```
modulator (sine, internal self-feedback)  --index(t)-->  phase of carrier (sine)
   freq = ratio x carrier                                   freq = note
                                   |  (+ clean carrier share at low pitch: "bass weight")
              BiquadFilter LP 12 dB, cutoff = note x 2^(1 + 7 tone)   (static tone)
                                   |
          x amp env^2 x velocity x 0.5 -> soft clip (|y| <= 1) -> Panner -> bus
```

## Parameters (the whole public surface, 11)

| Param | Range | Default | Unit | Meaning |
|---|---|---|---|---|
| `tune` | 27.5-880 | 110 | Hz | Root. Note `pitch` is semitones on top. |
| `ratio` | 0-7 (step) | 1 | step | Modulator/carrier ratio, a colour switch: 0.5, **1**, 2, 3, 4, 5, 1.414, 3.5. |
| `timbre` | 0-1 | 0.35 | | Resting FM depth: sine -> rich. 0 = a pure sine (with snap 0). |
| `snap` | 0-1 | 0.6 | | Extra depth at note start that decays (bright attack). |
| `tone` | 0-1 | 0.75 | | Static low-pass, relative to the note. 0 ~ fundamental only, 1 = open. |
| `attack` | 0.001-0.25 | 0.003 | s | |
| `decay` | 0.03-2 | 0.3 | s | Also the snap's time scale (x0.35). |
| `sustain` | 0-1 | 0.6 | | The level the note actually holds at. |
| `release` | 0.01-1.5 | 0.1 | s | Read at note-off. |
| `legato` | 0-1 | 0 | | >= 0.5: a note on a held note continues the voice (as Bass). |
| `slide` | 0-0.5 | 0 | s | Legato glide time, linear in semitones. |

Ranges come from `getInstrumentParams(InstrumentKind.Fm)`; never hardcode them. `ratio` is a step
value (the engine rounds it); the mapping to actual ratios is internal, so a UI can show 8 detents.

What `ratio` does (sidebands at `f(1 +- k r)`):

| ratio step | r | Character |
|---|---|---|
| 0 | 0.5 | sub-octave growl (period doubles) |
| 1 | 1 | all harmonics, saw-like: the bass/lead workhorse |
| 2 | 2 | odd harmonics only: hollow, clarinet/square |
| 3 | 3 | all but every 3rd harmonic |
| 4 | 4 | odd harmonics, energy higher up: thin, bright |
| 5 | 5 | all but every 5th |
| 6 | 1.414 | inharmonic: metallic |
| 7 | 3.5 | inharmonic: bell, glassy |

## Design decisions

- **Ratio, not two frequencies.** `modulator = carrier x ratio`, derived from the live carrier pitch
  every 16 samples, so the spectrum has the same shape at every note, with `tune`, `pitch`, legato and
  during a slide. Stepped, because continuous ratios are mostly out-of-tune clang.
- **Index, never exposed raw.** `timbre` (resting) + `snap` x internal envelope, scaled by velocity
  (0.55 + 0.45 v: harder = brighter, always on). Peak index is ~5 resting and +7 snap (radians). The
  index is interpolated per sample inside each 16-sample sub-block.
- **Envelope-controlled FM without a second ADSR.** One internal `AdsrEnvelope`: attack = public attack,
  decay = 0.35 x public decay, sustain 0, release = public release. `snap` scales it. The amplitude
  envelope is the Bass shape (squared, `sqrt(sustain)`), so the public sustain is the real level.
- **Bandwidth guard.** The effective index is limited so `(index + 1) x modulator Hz < 0.4 fs`. High
  notes and high ratios cannot alias at any sample rate; no oversampling needed. The same cap bounds the
  feedback.
- **Feedback: fixed, internal, on the modulator only,** proportional to the effective index (so index 0
  is an exact sine), two-sample averaged, max 1.1. It makes the modulator sawtooth-like, which is the
  difference between a plain "electric piano" two-op and a usable bass/lead spectrum.
- **Sine only.** Predictable sidebands; a waveform param would make this a general oscillator.
- **Filter: optional shaping, not essential.** FM makes the harmonics, and the index envelope does the
  job a filter envelope does elsewhere, so there is **no `filterMod`** (and none of the shared
  `FilterModulation` depth). `tone` is a static 12 dB low-pass whose cutoff is *relative to the note*
  (brightness is pitch-invariant). It tames the top sidebands for bass and darkens leads. It follows
  pitch changes at 1 semitone per sub-block, because a cutoff jump on a pitch jump clicks.
- **Bass weight (internal).** PM's fundamental follows `J0(index)` and nulls (about -30 dB) near index
  2.4 and 5.5, and the snap sweeps through that on every note. At low pitch a fixed share of the clean
  carrier is blended in: 0.5 at/below 80 Hz fading to 0 at 320 Hz, normalised so index 0 is exactly the
  carrier. At 55 Hz ratio 1 stays within ~6 dB of the plain carrier over the whole timbre range.
- **Legato / slide:** identical semantics to Bass (no re-attack, held velocity kept, linear-in-semitone
  glide, last note wins, the sequencer's `duration` is the hand-off). See
  [`bass-instrument.md`](./bass-instrument.md#legato-and-slide).

## Bass vs lead

| | Bass | Lead |
|---|---|---|
| Register | `tune` 27.5-110, ratio 1 (or 2 for a hollow slap) | `tune` 220-880, ratio 1/3/2 |
| Fundamental | bass weight keeps it | the note itself; no weight above 320 Hz |
| Sound | `timbre` 0.2-0.4, `snap` 0.6-1, `decay` 0.15-0.4, `tone` 0.5-0.8 | `timbre` 0.4-0.6, `snap` 0.2-0.5, `sustain` high, `legato` + `slide` |
| Useful | punchy, clean sub with bright slap | glassy to sawish, animated by velocity |

Same architecture, no mode switch. Ratio 2 and 4 are hollow; 6 and 7 are metal; 3 and 5 are
"almost-harmonic" and good for edgy leads.

## Behaviour

- **Mono per instance** (engine contract); chords/layers = more instances. Polyphony is the engine's.
- **Fresh note:** phases reset (carrier at its peak, modulator at 0), filter and feedback cleared;
  identical inputs render identical samples.
- **Retrigger** (note-on while sounding/releasing, non-legato): continuous. Phases carry, both envelopes
  re-attack from their current level, pitch jumps; the snap re-fires.
- **Note-off:** starts `release` for amplitude and for the internal index envelope. Repeated note-off is
  a no-op. After the tail the instrument writes exact silence and goes inactive. `sustain 0` plucks.
- **Timed notes:** pass `duration` on `instrumentNoteOn` (sequenced notes), as for Bass.
- **Velocity:** linear level (output bounded by a soft clip for any velocity) plus brightness.
- **Pitch range:** fundamentals clamped to 20 Hz - 4 kHz (and 0.2 fs).

## Cost

Per sample: 2 sines (3 in the bass register), 1 biquad, 2 linear ADSRs. Control rate (every 16
samples): index limit, filter cutoff gating, glide. Roughly Bass-class (estimate, not a benchmark).
No allocation in `render` (counted in `fm_test.cpp`). Voice state: 2 phases, feedback pair, index,
weight, 2 envelopes, 1 filter.

## Tests

`native/test/fm_test.cpp` (`npm run test:native`; `FM_WAV_DIR=<dir>` dumps WAVs including four demo
patches): finite/bounded/deterministic; carrier pitch for several tunes and note offsets; modulator
follows the carrier (same spectrum shape an octave up, via `pitch` or `tune`; no energy between
harmonics for integer ratios; sideband positions for ratio 3 and 1.414); index 0 is exactly the carrier
(also with bass weight; all ratios identical); more `timbre` = more harmonic activity with level within a
few dB; the index envelope (bright onset settling while the level is flat, `snap` 0 = no movement,
follows `decay`); ADSR timing and true sustain; release/retrigger continuity (no steps) and exact
silence after the tail; parameter metadata, clamping and non-finite handling; polyphony (24 instances
finite, and the mix equals the sum of the parts); a random-parameter fuzz at 22.05/48/96 kHz; the
bandwidth guard (no energy near Nyquist at 5x ratio, full index, high note); velocity (linear level,
brighter); tone; bass fundamental retention; legato/slide; scheduled start and duration accuracy; bus
routing; anti-triviality guards (hollow odd-only spectra, inharmonic partials); zero allocation.

## Tuning status

All constants (index scale 5 + 7, feedback 1.1, weight 0.5, snap decay 0.35, tone map) are first-pass
values set by measurement, **not yet by ear**. Dump WAVs with `FM_WAV_DIR` and tune. If the bass lacks
weight, raise `kBassWeight`; if the onset is too sharp, lower `kSnapIndex`.
