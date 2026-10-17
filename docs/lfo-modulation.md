# LFO modulation

A small, reusable way for an application to move a synth parameter over time. It replaced the
synth-specific filter modulation on Bass and Pulse (see [`filter-modulation.md`](./filter-modulation.md)).

```text
synth parameter (BASE, set by the app) ─┐
                                        ├─▶ effective = map(domain, base, Σ lfo·amount) ─▶ Instrument::setParam
LFO ×4 (waveform, rate|division,        ┘
        phase, amount, target)
```

Three separate concerns:

| Concern | Owner | Where |
|---|---|---|
| Native synth behaviour | the synth | `instruments/*.h` (unchanged by modulation) |
| Modulation source | the LFO — knows nothing about parameters | `dsp/lfo.h` |
| Parameter mapping, base/effective, bounds | modulation layer | `modulation.h`, hooked in `Engine::process` |
| Timing for sync | the engine's existing beat grid | `Engine::scheduleTransport` / `setTempo` |

## The LFO (`dsp/lfo.h`)

Output is a normalized **-1..+1**. Waveforms: sine, triangle, square, saw (rising ramp). Closed-form,
no allocation.

* **Free**: `rateHz` 0.01..40; an accumulator that keeps its own time (runs while the instrument is
  silent; a rate change never jumps the phase).
* **Sync**: phase is a pure function of the beat grid, `(frame - anchor) / (beats · framesPerBeat)`,
  exactly how Pulse derives its pump. Stateless, so it stays aligned through silence and tempo
  changes. A tempo change that keeps the anchor jumps phase (as Pulse's pump does); re-anchor on a
  beat for continuity. Re-anchoring (a transport restart) restarts the cycle.
  Divisions (slowest first): 4 bars, 2 bars, 1/1, 1/2., 1/2, 1/4., 1/2T, 1/4, 1/8., 1/4T, 1/8, 1/16.,
  1/8T, 1/16, 1/16T, 1/32 (dotted ×1.5, triplet ×2/3; "1/4" = one cycle per beat).
* **Phase** 0..1 is a constant offset (the starting phase) in both modes. Editing an enabled LFO keeps
  its phase running; only disabled → enabled restarts it. `retrigger` (free mode only) restarts the
  cycle at each note-on; otherwise a note never touches the LFO.

## Targets and domains

A synth declares which parameters are modulatable with one optional field on its parameter table row,
`InstrumentParamInfo::mod` (`ModDomain`):

* `None` (default): not a target (attack/release/legato/stepped params…).
* `Linear`: `base + mod · (max-min)/2`.
* `Log`: `base · 2^(mod · log2(max/min)/2)`, octave domain for Hz params (cutoff, tune). Needs `min > 0`.

`mod` is the sum of `smoothed LFO · amount` of every LFO on that param; **amount** -1..+1 (100% = half
the range either side of the base, in the param's own domain). The result is clamped to `[min,max]`.
Targets are discovered from the engine (`getInstrumentParams(kind)[i].modulation`,
`runtime.getModulationTargets(kind)`), never hard-coded in an app.

Currently modulatable: **Bass** tune, body, bite, cutoff, resonance; **Pulse** tune, cutoff,
resonance, pump, repeat, diffuse. Pad, Fm and the drums declare none (untouched). Pan is per-note,
not a param, so it is not a target yet.

## Base vs effective

The base is stored in the engine slot (`InstrumentModulation`), separate from what the instrument is
told. While a param is targeted, `setInstrumentParam` writes the base only and the next control tick
pushes base + modulation; when the last LFO leaves a param the base is restored. `Engine::
instrumentBaseParam/EffectiveParam` (and `webdsp_instrument_base_param/effective_param`) expose both.

## Rate, smoothing, safety

While an instrument has an active LFO, its render segments are split into 32-frame chunks
(~1.5 kHz control rate) with a param update before each. With no LFO (or zero amount) the render is
**bit-identical** to the unmodulated path (tested). The LFO output is smoothed (1.5 ms one-pole) so
square/saw edges do not zipper, and Log params are slew-limited to 1 octave per chunk because biquad
coefficients jumping many octaves can diverge. Tested finite and in-range with square LFOs at 40 Hz,
resonance 1, on Bass and Pulse. No allocation on the audio thread.

## Limits and API

* 4 LFOs per instrument (`kMaxLfos`), one target each; several may share a target (they add).
* C ABI: `webdsp_instrument_set_lfo(instrument, slot, enabled, waveform, sync, rateHz, division,
  phase, amount, target, retrigger)`, `webdsp_instrument_param_mod`, `webdsp_lfo_*` (limits and name tables).
* TS: `runtime.getModulationInfo()`, `getModulationTargets(kind)`, `setInstrumentLfo(id, slot, {target,
  amount, waveform, sync, rateHz, division, phase, retrigger})`, `clearInstrumentLfo(id, slot)`.
* Add a target to a new synth by marking its param row with a `ModDomain`; no other synth code.

Tests: `native/test/lfo_test.cpp` (waveforms, rate, sync, BPM/anchor, phase, mapping, bounds, layer
semantics), `native/test/modulation_test.cpp` (engine + real Bass/Pulse audio), `test/modulation.test.ts`,
and the ABI checks in `native/test/smoke.mjs`.
