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

## Who uses it

| Synth | Use |
|---|---|
| Pad | `movement` drift + envelope -> cutoff (protected; its sound is fixed) |
| Fm | static tone filter (zero depth) |

**Bass and Pulse do not use it.** They briefly did (commits f5dd871..598445a); that changed their
character, so both were restored to their own inline cutoff code (Bass keeps legato/slide, Pulse
keeps pump-after-ADSR). Bass's `filterMod` param (id 11) was removed. The `filterModParam` /
`filterModOctaves` helpers remain in the header but no shipped instrument exposes the control.

Instrument-to-parameter modulation by the user is a separate, generic mechanism: see
[`lfo-modulation.md`](./lfo-modulation.md). This class is internal filter mechanics only.
