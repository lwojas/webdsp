# Filter modulation

A small shared capability for synth instruments: *"here is my internal modulation signal; apply
this much of it to my filter."* Source: `native/src/dsp/filter_modulation.h`; tests:
`native/test/filter_modulation_test.cpp`.

It is deliberately **not** a modulation system: no LFO, source, route or destination types (see
[`adsr-envelope.md`](./adsr-envelope.md)). The instrument owns the signal and what it means; this
class owns only the filter-side mechanics.

```text
effective = clamp(max(base * 2^(depthOctaves * signal), floorHz), 30 Hz, min(18 kHz, 0.45 fs))
```

| Input | Meaning |
|---|---|
| `base` | Resting cutoff in Hz, after the instrument's own shaping (e.g. keytrack). |
| `signal` | The instrument's modulation value, **-1..+1** (clamped; NaN-safe). Unipolar sources use 0..1; negative closes the filter. 0 = exactly `base`. |
| `depthOctaves` | Octaves moved by a signal of 1. Octave domain so depth sounds alike at any cutoff and can't cross zero. |
| `floorHz` | Optional instrument floor (Bass/Pulse: 0.9 x fundamental). |

`FilterModulation::apply(filter, base, depth, signal, floor)` computes this and sets the
`BiquadFilter` cutoff. Call it at the instrument's control rate (both synths: every 16-sample
sub-block). It reuses the existing smoothing (control-rate stepping + a 0.2% change threshold
before coefficients are recomputed) and adds one safety net: the cutoff moves at most 1.5 octaves
per call, because a biquad whose coefficients jump across many octaves between blocks can diverge
(measured). The limit sits above everything Bass and Pulse do, so they are unchanged; only
square-wave-like sources are affected. The first call after `reset()` jumps to the target.
`cutoffHz()` is the pure calculation without the limit. No allocation; one `exp2` per call.

## Adopters

- **Bass:** signal = timbre envelope, depth = `bite` x 4 oct x velocity factor.
- **Pulse:** signal = repeat contour (unipolar), depth = 3 oct x accent.
- Drums: not adopted. Their filters are per-note static settings with no modulation source.
- No new public parameters: `bite` / `repeat` already are the depth controls. A future synth with a
  slow internal movement exposes its own depth param and passes its own signal.
