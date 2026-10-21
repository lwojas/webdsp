# Dual-filter machine — proposal

Status: **partly realised.** The shared routing block (candidate C, stage 1) now exists as `native/src/dsp/dual_filter.h` and is used by the Edge instrument ([`edge-instrument.md`](./edge-instrument.md)), generalised to four stage modes (LP12, LP24, HP12, band-pass) and a `single/series/parallel` routing with a `cutoff` + `spread` control; split was not built. The dedicated window/scoop machine described below remains a proposal. Follows the shape of
[`pulse-instrument-proposal.md`](./pulse-instrument-proposal.md) and
[`bass-instrument-proposal.md`](./bass-instrument-proposal.md). The question this document
answers is not "how do we build it" but **"is there a sonic territory here that the shipped
machines do not already cover, and is routing two filters enough to claim it?"** Section 9 gives a
conditional answer and a cheap way to settle it before any ABI exists.

## 1. Inspiration (what is actually known)

The Access Virus line (documented at theusualsuspects.io/technical/osirus-c, summarised in
[`synth-dsp/references.md`](./synth-dsp/references.md)) is described as having a "twin multimode
resonant filter" that can be configured in series, parallel or split, with slopes from 12 to
36 dB/oct. **That is the whole of what the source establishes.** It gives no topology maths, no Q
handling, no definition of what "split" means, no saturation, no modulation structure.

Consequences for this document:

- We are **not** reproducing the Virus filter and make no claim to. The words "series",
  "parallel" and "split" are used below with **our own** definitions (section 3).
- Everything in sections 3-8 is original webdsp design, built from the existing `BiquadFilter`.
- The Virus is a reason to look at routing as a design axis, nothing more.

## 2. What webdsp already provides

| Area | Finding | Consequence |
|---|---|---|
| Filter | `BiquadFilter`: RBJ LP/HP only, 12 or 24 dB/oct (slope crossfaded), per-channel state, in-place block `process`, `setParam` cutoff/Q/mode/slope | Two instances give everything below. **No band-pass or notch primitive**; those only arise from combining LP and HP |
| Cutoff movement | `FilterModulation` rate-limits cutoff moves to 1.5 oct per control step, because jumpy biquad coefficients at high Q diverge (measured) | Applies to *each* filter here; two filters double the exposure |
| Resonance headroom | Bass/Pulse map resonance to Q (`kQFlat + r^2 * kQSpan`) and apply `1 / (1 + kQComp * (q - kQFlat))` | Same pattern, but needs a chained-filter rule (section 6) |
| Source / lifecycle | PolyBLEP saw/pulse inline forms, `AdsrEnvelope` (amp + internal timbre), `PercCore` (note lifecycle, pan, routing), 16-frame control rate | Reuse as-is |
| Modulation | Generic parameter modulation and `setEffectLfo` ([`modulation.md`](./modulation.md), [`lfo-modulation.md`](./lfo-modulation.md)) | `cutoff` and `width` are automatable/LFO-able from outside; **no internal LFO is needed** |
| Neighbours | Bass, Pulse, PwmBeast, PercSynth: oscillator(s) into **one** resonant LP (24 dB) with an envelope. PercSynth adds an LP/HP switch. Pad, Hypersaw, Fm: one static 12 dB LP | The "resonant filtered oscillator" territory is **full** |

Also true today and relevant: a user can already put two `BiquadFilter` FX in series on a track
insert chain. What that cannot do is **per-note** behaviour: a filter envelope retriggered by each
note, keytracking, velocity, and separate source routing. That per-note behaviour, not the
two-filters part, is what a machine would add over the FX chain.

## 3. The routings (our definitions)

Two sources, osc 1 and osc 2, summed or not depending on routing; two filters, A and B.

```text
series      (osc1 + osc2) -> A -> B -> out
parallel    (osc1 + osc2) -+-> A -+-> sum -> out
                           +-> B -+
split       osc1 -> A -+
            osc2 -> B -+-> sum (optionally panned) -> out
```

| Routing | What it is | Linear response (shared input) | Territory |
|---|---|---|---|
| **Series** | Filters multiply | LP then HP = **window** (band-pass-like, independent skirts); LP then LP = steeper low-pass | Vocal/reedy/nasal, "wah", thinned leads, band-limited bass |
| **Parallel** | Same input, outputs add | LP + HP = **scoop**: with the LP edge below the HP edge the middle is removed | Hollow, phaser-like, scooped pads and stabs |
| **Split** | Each filter gets its own oscillator | Sum of two *different* spectra | Two layers with independent timbre (dark body + bright layer), stereo decorrelation |

### Exact behaviour worth stating

- **Parallel LP + HP at equal cutoff is an exact notch.** With `LP = 1/D`, `HP = s^2/D`, the sum
  is `(1 + s^2)/D`, which is zero at the cutoff for *any* Q. Q only sets the notch width and the
  size of the shoulders. Separating the cutoffs turns the notch into a wide scoop; crossing them
  (HP edge below LP edge) makes both pass the middle and sum to about +6 dB there, i.e. a broad
  bump, not a scoop. So for the parallel routing the LP edge must sit **below** the HP edge.
- **Series LP then HP is a window** whose LP edge is above its HP edge. Narrowing it removes
  energy at both ends, so output level falls as the window closes (section 6).
- **Series order matters only nonlinearly.** LP -> HP and HP -> LP have the same linear
  magnitude; they differ in where resonance peaks land relative to each other and in clipping
  behaviour. We fix the order (A = LP first) rather than exposing it.

### Does split earn a slot?

Honestly: **only conditionally.** With identical oscillators and zero interval, split is nearly
parallel (each filter sees half the signal). It differs audibly only when (a) the two oscillators
differ (waveform, pitch interval), giving each filter different harmonic material, or (b) the two
filter outputs are panned apart, which is a stereo effect. Both are legitimate and cheap
(split is an input selector on the same code path), but the extra routing value must be heard at
the listening gate (section 9). **Default plan: include it; first thing cut if it does not
sound distinct from parallel.**

## 4. Controls and per-topology behaviour

Principle: expose the **shape of the passband**, not "filter A and filter B". The machine's
controls are a centre and a width, so one gesture moves both edges.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `tune` | 27.5-440 Hz | 110 | Root; note pitch on top (as Bass/Pulse) |
| `routing` | stepped 0-2 | 0 | series / parallel / split |
| `cutoff` | 60-8000 Hz | 800 | **Centre** of the window or scoop, keytracked |
| `width` | 0.25-5 oct | 2 | Distance between the two edges, geometric about `cutoff` |
| `resonance` | 0-1 | 0.2 | Q (section 6); low default on purpose |
| `filterEnv` | -1..1 | 0.4 | Envelope depth on the centre (+-6 oct like PercSynth); negative sweeps down |
| `filterDecay` | 0.02-3 s | 0.4 | Filter envelope time |
| `mix` | 0-1 | 0.5 | Osc 2 level against osc 1 (power-normalised) |
| `interval` | -12..+12 st | 0 | Osc 2 relative to osc 1 (waveforms fixed: osc 1 saw, osc 2 pulse) |
| `attack` / `decay` / `sustain` / `release` | as Bass | | Amp envelope |

Edges derive from the two controls: `lo = cutoff * 2^(-width/2)`, `hi = cutoff * 2^(+width/2)`.

| | Filter A | Filter B | Resonance goes to |
|---|---|---|---|
| **Series** | LP at `hi` | HP at `lo` | A fully; B tied (section 6) |
| **Parallel** | LP at `lo` | HP at `hi` | Both, summed not multiplied |
| **Split** | LP at `lo` (osc 1) | HP at `hi` (osc 2) | Both, independent layers |

The same `cutoff` and `width` keep one meaning ("where" and "how wide") in every routing; only
which edge belongs to which filter flips. Cutoff and width follow the instrument's note: both
edges move together with `filterEnv` and keytrack, so the window **slides** rather than just
opens. Slope is fixed at 12 dB per filter in v1 (series totals 24 dB). Offering 12/24 per
filter is a later, cheap extension since `BiquadFilter` already supports it (open item,
section 10).

Not exposed: filter order, individual modes, oscillator internals, Q tie, compensation, keytrack
amount, floors, the 16-frame control step.

## 5. Implementation cost

| Item | Cost |
|---|---|
| Biquads | Two 12 dB biquads per instance, about 2x Bass's single 24 dB (also 2 biquads) in series, same in parallel/split. About 10 mult/adds per sample plus coefficient work |
| Coefficients | 2 `sin`/`cos` pairs per 16-frame control step, gated by the existing 0.2% change threshold |
| Memory | Parallel/split need one scratch copy of the signal (scratch buffers already exist in the `PercCore` render) |
| Oscillators | Two PolyBLEP forms already inline in Bass/Pulse/PwmBeast |
| Total | Roughly Pulse-class, order of 80 flops/sample/instance. Fine for the 64-instance ceiling at 48 kHz |

New code is routing, summing and parameter mapping. **No new DSP primitive.** A shared header
`dsp/dual_filter.h` (two `BiquadFilter` plus a routing switch, in the style of
`filter_modulation.h`) is the natural home, which also lets a later machine reuse the routing
without adopting this one's sources.

## 6. Resonance and headroom

This is the real engineering risk of the idea.

- **Series resonance multiplies.** Each 12 dB biquad has gain about `Q` at its cutoff. If both
  edges sit near each other (small `width`) and both resonate, the peak is about `Q_A * Q_B`:
  8 x 8 is 64, +36 dB. Today's single-filter Q compensation does not cover this.
- **Parallel resonance adds.** Peaks sum (at most 2x per coincident peak) instead of
  multiplying, so parallel and split are far tamer.
- **Window loss.** Narrowing a series window removes energy; the same width that makes it
  vocal also makes it quiet.
- **Stability.** Two filters means two sources of the biquad-divergence case that
  `FilterModulation` documents (large coefficient jumps between 16-frame steps at high Q).

Rules (to be tuned by ear, bounded by tests):

1. **Resonance is mostly A's.** `Q_A = qmap(resonance)` like Bass. In series, `Q_B = kQFlat + tie *
   (Q_A - kQFlat)` with `tie` about 0.3 (internal), so B colours rather than screams.
2. **Compensate for the worst coincident case.** `comp = 1 / (1 + k * (P - kQFlat^2))`,
   `P = Q_A * Q_B` for series, `max(Q_A, Q_B)` for parallel/split. Conservative on purpose;
   quiet at high resonance is a smaller sin than clipped.
3. **Window make-up.** For series, add a capped gain (at most +12 dB) that rises as `width`
   falls, so closing the window does not simply mute the note.
4. **Parallel sum scaling.** Scale the sum so the passband at default width is about unity
   (about -3 to -6 dB), because the two paths add where they overlap.
5. **Rate-limit both cutoffs** with the same 1.5-octave-per-step safety as `FilterModulation`.
6. **Existing final stage unchanged:** soft clip to +-1 after the filters, as every synth does,
   so nothing can be unbounded even if the rules above are wrong.
7. **No inter-filter saturation in v1.** A nonlinearity between A and B (Moog-style) would add
   character but also aliasing and loudness changes; it belongs to a later experiment.

## 7. Sonic territory, and avoiding "another resonant VA"

State the problem plainly: four shipped synths are "oscillator into a resonant 24 dB low-pass
with an envelope", and the previous experience ("everything is becoming buzzy/resonant") is
exactly that family. **Two filters do not leave that family by themselves.** A series LP -> LP
at high resonance is just a louder, steeper version of what Bass and PwmBeast already do, and
should be treated as a failure mode, not a feature.

The machine should occupy a different place: **spectral shape over spectral peak.**

| Existing territory | Defined by | This machine |
|---|---|---|
| Bass, PwmBeast, PercSynth, Pulse | A resonant peak sweeping through a rich source; low-pass roll-off | A passband **shape**: window or scoop, set by centre and width, with resonance as a minor accent |
| Pad, Hypersaw | Wide, static stereo ensemble through a gentle LP | Mono-core (optional stereo split), moving band |
| Fm | Sidebands generate the brightness | Subtractive, but band-limited: the low end is removable |

What gives it an identity, in order of importance:

1. **Passband shape as the main control.** Window = band-limited, nasal/vocal/reedy leads and
   thin "radio" basses. Scoop = hollow, phaser-like pads and stabs. Neither is available from
   a single LP, and a single LP+HP FX pair cannot be enveloped per note.
2. **Low default resonance.** The default patch should be a clean, mid-forward vowel-ish tone, so
   the first impression is not buzz. Resonance is an accent, not the voice.
3. **The window moves as a window.** One envelope slides centre and width together: a vowel-like
   sweep, with the fundamental leaving and returning, is the signature gesture.
4. **Two different sources.** Saw plus pulse at a chosen interval gives the filters distinct
   material, which is what makes split meaningful.

Honest weaknesses:

- Removing the low edge removes the fundamental; on bass notes a narrow window can sound **thin
  or weak** (the opposite of Bass). A floor tied to the note's fundamental (as LP has) may be
  needed, or this machine may simply not be a bass machine.
- A band-pass-like sound on a saw is a well-known timbre (wah/formant); it can come across as a
  single trick unless `width` and `routing` sweeps feel musical.
- Parallel scoop on a mono saw can sound like a static phaser.

## 8. Candidates

| | **A. Dual-filter machine (this)** | B. Add a routing option to PercSynth | C. `dsp/dual_filter.h` helper only | D. Nothing: use two FX |
|---|---|---|---|---|
| Idea | New kind: window/scoop passband machine | A second filter and routing in the existing kind | Shared routing block; no new kind | Two `BiquadFilter` FX in the track chain |
| New identity | Possible (section 7) | Dilutes a percussion synth | None by itself | Static only |
| Per-note envelope/keytrack | Yes | Yes | Depends on user | **No** |
| Param surface | ~13 | +4 on a 23-param kind | 0 | 0 |
| Cost / risk | Medium; listening-gated | Medium; risks existing Perc patches | Low | Zero |
| Weakness | Might sound like "a band-pass wah" | Muddles a purpose-built kind | Nothing audible changes | Nothing per note |

Recommendation: **C first, then A only if a listening gate says the territory is real.** B is
rejected because a machine is meant to be opinionated and PercSynth is already the widest one.
D is the baseline to beat: if a user can get the sound from two track FX, the machine has to
justify itself through per-note behaviour.

## 9. Does it justify a new machine?

Conditional yes, with an explicit test, not a promise.

It is justified only if all of these hold at the listening gate:

1. **Window and scoop sound clearly unlike** Bass/PwmBeast/PercSynth at comparable patches (not
   "the same with extra steps").
2. The default patch is **not buzzy**; resonance can be zero and it still sounds like an
   instrument.
3. The `width` sweep and enveloped slide are **musical gestures** at several pitches, not just
   at one tuned note.
4. **Split is audibly distinct from parallel** (or it is cut).
5. It remains usable at low notes (or is deliberately a mid/lead machine).

Failing 1 or 2 means it is not a new identity, and the right outcome is to keep only the shared
`dual_filter.h` (Candidate C) and drop the kind. Failing 4 drops the routing, not the machine.

## 10. Open decisions

- Slope per filter (12 only, or 12/24 selectable): cheap to add, adds a control.
- Whether `width` should be allowed negative (crossed edges) as a deliberate deep-attenuation
  zone, or clamped positive.
- Panning of split (fixed left/right, or a `width`-style control).
- Whether the final low-end floor is needed (section 7).
- Working name (the kind is "Twin" in notes only; `category` would be `"synth"`).

## 11. Tests (when and if built; `native/test/dualfilter_test.cpp`)

- **DSP:** series LP+HP magnitude is band-limited at `lo` and `hi`; parallel LP+HP at equal
  cutoff is a notch (>= 30 dB down, any Q); crossed parallel edges bump, not scoop; edges
  derived from `cutoff`/`width` exactly.
- **Headroom:** peak after compensation at max resonance, all routings, all widths: finite,
  |y| <= 1, DC mean small; series worst case (`width` near 0, max Q) not louder than a plain
  max-Q render by more than a bounded amount.
- **Stability:** fast `filterEnv` on both cutoffs at max Q for 5 s: finite (the divergence case).
- **Anti-triviality:** spectral distance from a single-LP render of the same notes is above a
  threshold in window and scoop routings; split differs from parallel when sources differ and
  matches closely when they do not (documenting the equivalence honestly).
- **Housekeeping:** determinism, retrigger continuity, release cleanup, zero allocations,
  TS `paramIds`, WASM `smoke.mjs`. WAV dumps via a `DUALFILTER_WAV_DIR` variable.

## 12. Staged plan

0. **Paper gate (no ABI).** Offline test harness that renders the three routings with fixed
   notes and dumps WAVs next to Bass/PwmBeast/PercSynth at comparable settings. Judge section 9
   criteria 1-5 by ear. Stop here if they fail.
1. `dsp/dual_filter.h` (two `BiquadFilter`, routing, edge mapping, compensation) plus unit
   tests. Useful on its own (Candidate C).
2. `instruments/dualfilter.h`, `params.h`, registry, TS enums.
3. Tests, WAV fixtures, **second listening gate** with the full instrument.
4. `npm run build:wasm`, `build:lib`; commit generated outputs.
5. `docs/dual-filter-instrument.md`, `ARCHITECTURE.md`, `docs/instruments.md` row.
6. Clients later (webseq, webtracker).

## 13. Related, decided separately

Pulse's future character (Pluck, Resonant VA, or its present Duck & Breathe) is a **separate
product decision** and is recorded in [`pulse-character-decision.md`](./pulse-character-decision.md).
This proposal does not depend on it and does not change Pulse.
