# Modulation architecture

One modulation model for every parameter owner, instrument or effect:

```text
              ┌─────────────── Parameter (owner: synth control | FX NodeParam) ───────────────┐
              │                                                                                │
 Automation ──┤  BASE value, written by the application (setInstrumentParameter /             │
 (app side)   │  setEffectParameter), e.g. from a curve evaluated against its own timeline     │
              │                                                                                │
 LFO ×4 ──────┤  engine-side bank, ParamModulation: Σ (smoothed LFO · amount), per target      │
              └──────────────┬─────────────────────────────────────────────────────────────────┘
                             ▼
              effective = map(domain, base, Σ lfo)  ──▶  Instrument::setParam / DSPNode::setParam
```

**Parameters have targets. Automation and LFOs are sources. Instruments and FX expose parameters.
The DSP resolves the stack.** The application edits and drives it.

## Pieces

| Piece | Where |
|---|---|
| LFO source (parameter-agnostic, -1..+1) | `dsp/lfo.h` |
| The bank: base/effective storage, summing, domain mapping, smoothing, slew, bounds | `modulation.h` `ParamModulation` |
| Instrument attachment (table from the synth kind, `Instrument` as sink) | `modulation.h` `InstrumentModulation`, hooked in `Engine::process` |
| FX attachment (table from the FX kind, the effect node as sink) | `fx_chain.h` (one bank per chain slot) |
| FX parameter self-description | `fx_params.h` (`InstrumentParamInfo`, same shape as a synth's) |

`ParamModulation` knows nothing about synths or effects: it is configured with a table of
`ModParamSpec {id, min, max, def, domain}` and pushes values through a `set(id, value)` sink.
`InstrumentModulation` and `FxChain` are the two (small) attachments. The behaviour (summing,
`Linear`/`Log` domains, 1.5 ms smoothing, 1 octave/chunk slew on log params, clamping, bit-identical
output without LFOs) is therefore identical wherever LFOs are attached. See
[`lfo-modulation.md`](./lfo-modulation.md) for the LFO and domain details.

## How sources combine

* **Automation = base.** The app writes the parameter's base value (`setEffectParameter`,
  `setInstrumentParameter`) as often as it likes. While an LFO targets the parameter the engine keeps
  that write as the *base* and plays `base + modulation`; when the last LFO leaves, the base is
  restored. So automation and any number of LFOs on one parameter need no coordination.
* **Several LFOs on one target add** (in the parameter's domain, before clamping). Up to 4 per owner
  (`kMaxLfos`), each with one target.
* Automation lives in the application, not the engine, because its coordinates are the
  application's (a sequencer's pattern/chain position). Nothing in webdsp knows what a pattern is.
  An application with an engine-clock curve can write the base from any timer the same way.

## FX modulation

Each FX instance owns a bank. The engine processes a modulated effect in 32-frame chunks with a
parameter update before each (`FxChain::process`); an effect with no LFO runs the single call it
always did (tested bit-identical, also for a zero-amount LFO). Synced LFOs follow the transport
(`FxChain::setClock`, fed by `Engine::process` with the block start and beat grid). A bypassed
effect is skipped entirely; re-enabling resets its smoothing.

Targets come from the effect's own table (`fx_params.h`), never from the application. Stepped or
click-prone controls opt out with `ModDomain::None`: filter mode/slope, phaser stages, bitcrusher
downsample, and **delay time** (its read pointer jumps; automate it as a base value instead).

API: C ABI `webdsp_fx_set_lfo(bus, effect, slot, enabled, waveform, sync, rateHz, division, phase,
amount, target)`, `webdsp_fx_base_param / effective_param`, `webdsp_fx_param_count/id/name/min/max/
default/unit/mod(kind, i)`. TS: `runtime.getEffectParams(kind)`, `getEffectModulationTargets(kind)`,
`setEffectLfo(bus, effect, slot, {target, amount, waveform, sync, rateHz, division, phase})`,
`clearEffectLfo(bus, effect, slot)`. Parameter `name` is the stable string an app addresses a control
by (`"cutoff"`, `"mix"`); `target` is the `NodeParam` id.

## Utility FX

`EffectKind.Utility` (8): a minimal gain stage, `NodeParam.UtilityGain` = dB, -60 (true silence)
.. +12, default 0 (unity, bit-exact pass-through). Gain is smoothed (~5 ms) so coarse automation
steps and LFOs never zipper. It is an ordinary effect instance, so it is automatable, LFO-modulatable
and reusable anywhere. It is the intended mechanism for per-track volume automation: the mixer's
`BusGain` stays the track's plain mix state. More functions (width, mono, phase) would be further
`NodeParam`s, not a new effect.

## Adding to it

* New synth control or FX parameter: add a row to its table; it is a target unless marked `None`.
* New FX kind: a `FxKind`, a node, a `kFxParamTables` entry, a `makeNode` case.
* Tests: `native/test/fx_modulation_test.cpp` (tables, Utility, stacking, base under LFO, safety,
  sync, engine level), `native/test/lfo_test.cpp` / `modulation_test.cpp` (instrument path, unchanged),
  `test/modulation.test.ts`, ABI checks in `native/test/smoke.mjs`.
