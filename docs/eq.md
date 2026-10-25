# EQ

`EffectKind.Eq` (kind 10, `native/src/dsp/eq.h`): a four-band parametric EQ, bands in series, each a
standard RBJ ("Audio EQ Cookbook") biquad. It is built to be driven by an interactive
frequency-response curve (Ableton EQ Eight style), but the engine knows nothing about any UI: the
effect is 28 plain `NodeParam`s, and the curve is computed client-side from the same values.

```
in -> band 1 -> band 2 -> band 3 -> band 4 -> out      (a disabled band is skipped)
```

## Band types (`EqBandType`)

| value | type | uses |
| --- | --- | --- |
| 0 | `Bell` | frequency, gain, Q |
| 1 | `LowShelf` | frequency, gain, shelfSlope |
| 2 | `HighShelf` | frequency, gain, shelfSlope |
| 3 | `LowPass` | frequency, slope |
| 4 | `HighPass` | frequency, slope |
| 5 | `Notch` | frequency, Q |

Unused fields are stored and ignored, so switching a band's type back and forth keeps its other
settings. `eqBandUses(type, field)` tells a UI which controls to show.

## Parameters (`NodeParam` 49..76)

Band `b` (0..3), field `f`: id = `49 + 7*b + f` (`NodeParam.EqBand1On` ... `EqBand4Slope`;
`eqBandParam(b, field)` computes it). Names in the param table are `b1On`, `b1Type`, `b1Freq`,
`b1Gain`, `b1Q`, `b1ShelfSlope`, `b1Slope`, then `b2...`.

| field | f | range | default | modulatable |
| --- | --- | --- | --- | --- |
| `On` | 0 | 0/1 | 0 | no (switch) |
| `Type` | 1 | 0..5 | band 1 LowShelf, 2-3 Bell, 4 HighShelf | no (step) |
| `Freq` | 2 | 20..20000 Hz | 100 / 500 / 2000 / 8000 | yes (log) |
| `Gain` | 3 | -18..+18 dB | 0 | yes |
| `Q` | 4 | 0.1..18 | 0.707 | yes |
| `ShelfSlope` | 5 | 0.1..1 (RBJ S) | 1 | yes |
| `Slope` | 6 | 12 or 24 dB/oct | 12 | no (step) |

All bands are off by default, so a fresh EQ is a bit-exact pass-through (same "inert until
parameterised" rule as the other effects). Frequency is clamped to 0.49 x the sample rate.
Low/high-pass use a Butterworth section (Q 0.707); at 24 dB/oct two sections with the 4th-order
Butterworth Q split 0.5412 / 1.3066, exactly as `BiquadFilter`.

## Processing

`setParam` only marks a band dirty; its coefficients are recomputed once at the start of the next
`process()` call, never per sample. State is transposed direct-form II in doubles, per channel and
section, and persists across blocks (verified: odd block sizes give identical output). Enabling a
band clears its state; disabling a band skips it. Stereo is two independent channels with the same
filters. Nothing allocates in `process()`. Coefficient changes are a single step with no smoothing
(see Known limitations).

## Drawing the curve (client side)

`src/runtime/eq.ts` (exported from the package root) is pure math with no DOM or framework:

```ts
import { defaultEqBands, eqBandToParams, eqCurve, eqBandParam, EqBandType } from "webdsp";

const bands = defaultEqBands();                 // 4 EqBand objects; your project state
bands[1] = { ...bands[1], enabled: true, gain: 6, q: 1.5 };

// Push to the engine (or only the field that changed):
for (const [param, value] of eqBandToParams(1, bands[1])) rt.setEffectParameter(bus, fx, param, value);
rt.setEffectParameter(bus, fx, eqBandParam(1, "gain"), 4);

// Draw: log axis, one path per band plus the sum.
const { frequencies, combinedDb, bandDb } = eqCurve(bands, sampleRate, 256);
```

`eqResponseDb(bands, hz, sr)` / `eqBandResponseDb(band, hz, sr)` give single points (use these to
place a handle: x = log(frequency), y = the band's dB at its own frequency, or 0 for filters
without gain). Suggested gestures: drag = frequency (x) and gain (y); wheel or modifier-drag =
Q (bell/notch) or shelf slope; the pass filters' slope is a 12/24 toggle. Disabled bands return 0
dB so they draw flat. The same maths is pinned to the DSP by the matching analytic checks in
`native/test/eq_test.cpp` and `test/eq.test.ts`.

## Known limitations / to revisit

**No coefficient smoothing (open issue, to be revisited).** When a parameter changes, the band's
biquad coefficients are replaced in one step at the start of the next block, and the filter state
carries over unchanged. This is the same behaviour as `BiquadFilter` and keeps the EQ simple, but
it means a fast or large change can be audible:

- *Dragging a handle, stepped automation, or an LFO on frequency/gain/Q* can produce zipper noise
  or a small click, most noticeably with high-Q bells and notches, large gain moves and fast
  frequency sweeps. LFO and scheduled-ramp updates arrive at the control rate (about every 32
  frames for LFOs), which is smooth enough for slow moves but not guaranteed click-free.
- *Changing a band's type* while audio runs swaps the filter shape abruptly; the state carries over
  but the output can step.
- Enabling a band clears its state, so it starts clean, but the response itself switches on at once.

Not yet measured: how audible this is in practice at typical UI-drag and LFO rates. Candidate fixes
when this is revisited, roughly in order of cost: (1) interpolate the coefficients (or the
frequency/gain/Q targets) over a short ramp, a few ms, recomputing every N samples; (2) crossfade
between old and new filter outputs for type/slope changes, as `BiquadFilter` already does for its
12/24 dB slope switch; (3) a state-variable/lattice structure that tolerates coefficient changes
better. Whatever is chosen should keep the explicit, serialisable parameter model and the
allocation-free `process()`.

## Not included

The interactive curve editor is a client feature (webseq `EqCurveEditor.tsx`, webtracker
`EqCurveEditor.jsx`), built on `eqCurve()`; webdsp itself has no UI. The live spectrum behind the curve (output, with the input fainter) is the generic analysis tap, see `docs/spectrum.md`. No mid/side or per-band stereo, no auto-gain,
no oversampling, no more than 4 bands (adding bands would append params after id 76).

Tests: `native/test/eq_test.cpp`, `test/eq.test.ts`, `test/paramIds.test.ts`, `native/test/smoke.mjs`.
