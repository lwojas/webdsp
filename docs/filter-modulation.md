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

## The standard control: `filterMod`

Any synth that modulates its filter exposes **one** param named `filterMod` (0..1, unitless), the
modulation depth. It is never folded into another control. Declare it with
`filterModParam(id, default)`; convert with `filterModOctaves(amount)`:

```text
octaves per unit of signal = filterMod * 4        (kFilterModMaxOctaves)
```

The instrument may scale that further (velocity, accent) and always owns the signal. Choose the
default so the out-of-the-box sound is the one you want. `filterMod = 0` is a static filter at its
base cutoff, exactly. Clients should build the knob from `getInstrumentParams` like any other
(`name` is `filterMod`; the TS enums call it `FilterMod`) and may label it "Filter Mod".

## Adopters

| Synth | Param id | Default | Signal | Other controls that no longer move the filter |
|---|---|---|---|---|
| Bass | `BassParam.FilterMod` = 11 | 0.4 | timbre envelope (x velocity factor 0.4..1) | `bite` = onset drive, thump, velocity->timbre |
| Pulse | `PulseParam.FilterMod` = 9 | 0.3 | repeat contour (unipolar) x accent | `repeat` = drive per cycle only |

Defaults reproduce the previous sound at the previous default settings (Bass bite 0.4 -> 1.6 oct;
Pulse repeat 0.4 -> 1.2 oct). Behaviour change to know about: `bite` and `repeat` used to also set
the filter depth. Now `repeat = 0` still opens the filter on the grid if `filterMod > 0`; set
`filterMod = 0` for no filter movement. Pulse's maximum depth is 4 octaves (was 3).

Not adopted: drums (static per-note filters). Pad has its own `movement` control, which also
drifts detune and stereo image; it does not yet have a separate `filterMod`.
