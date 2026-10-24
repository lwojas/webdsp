# FX chains

Every bus (the master bus and each `createBus()` track bus) owns an **ordered chain of
independent effect instances**. The same effect kind can appear any number of times.

```
track voices -> [Filter A -> Filter B -> Delay -> Filter C] -> bus gain/pan -> [sum]
                                                                  |
master: [sum] -> [Filter -> Filter -> Delay] -> bus gain/pan -> output
```

Native: `native/src/fx_chain.h` (`FxChain`), used by `Bus` (`native/src/bus.h`) for track and
master alike. Runtime: `AudioRuntime.addEffect / removeEffect / moveEffect / setEffectEnabled /
setEffectParameter / getEffectChain`.

## Model

- **Instance** = `{ id, kind, enabled }` plus its own DSP object. State and parameters are never
  shared: two `Filter` instances are two `BiquadFilter` objects.
- **Kind** (`EffectKind`): Filter, ChorusFlanger, Delay, Reverb, Compressor, Saturation,
  Phaser, Bitcrusher, Utility, DubDelay (tempo-synced, [`dub-delay.md`](./dub-delay.md)). Kind
  says *what DSP runs*; id says *which instance*.
- **Id** (`EffectId`): a number handed out by `addEffect()` (unique per runtime, chosen on the
  main thread so no round trip is needed). Ids are the only public handle.
- **Order** is the array order; effects run first to last. `addEffect(..., {index})` inserts,
  `moveEffect` reorders. Order is a real signal-flow choice for nonlinear/dynamics effects
  (e.g. Saturation -> Filter differs from Filter -> Saturation).
- **Parameters** reuse the existing `NodeParam` ids: `setEffectParameter(bus, id,
  NodeParam.FilterCutoff, 800)`. A param that doesn't belong to the instance's kind is ignored.
- **Enable/bypass**: `setEffectEnabled`. A disabled instance is skipped (bit-exact
  pass-through) and its internal state is cleared, so re-enabling starts clean rather than
  replaying a stale tail.
- **Remove** destroys that instance only; siblings of the same kind keep their state.
- Newly added instances are inert until parameterized (same convention as before: filter
  bypassed until cutoff/mode is set, mix 0 for delay/reverb/saturation/chorus).

## Example

```ts
const bus = rt.createBus();
const lo = rt.addEffect(bus, EffectKind.Filter);
const echo = rt.addEffect(bus, EffectKind.Delay);
const hi = rt.addEffect(bus, EffectKind.Filter);
rt.setEffectParameter(bus, lo, NodeParam.FilterCutoff, 4000);
rt.setEffectParameter(bus, hi, NodeParam.FilterMode, FilterMode.HighPass);
rt.setEffectParameter(bus, hi, NodeParam.FilterCutoff, 200);
rt.moveEffect(bus, hi, 0);          // hi -> lo -> echo
rt.setEffectEnabled(bus, echo, false);
```

## Legacy `setNodeParameter` API

Unchanged and implemented as a thin wrapper over the chain. A write to an effect `NodeParam`
(e.g. `NodeParam.DelayMix`) goes to the bus's **legacy instance** of that kind (engine id
`-1 - kind`), created on first write and ordered filter, chorus/flanger, delay, reverb,
compressor, saturation, phaser, bitcrusher (i.e. kind order) relative to the other legacy
instances, exactly as the old fixed chain.
An untouched kind is absent, which is equivalent to the old inert node. `BusGain` / `BusPan`
remain bus-level, not effects.

**Bypass by mix 0 clears the memory.** Delay, Chorus/Flanger and Reverb skip processing while
their mix is 0. The first block they skip also clears their delay lines / tanks, so raising the
mix later starts clean instead of replaying audio from before the bypass (the same rule as
`setEnabled(false)`, which resets the instance). While the mix is above 0 the tail still rings on
silent input. `native/test/bypass_state_test.cpp`.

Legacy and explicit instances coexist; legacy writes only reach the legacy instance. Legacy
instances are not listed by `getEffectChain()` (it shows what `addEffect` created); the native
`webdsp_fx_*` inspection exports list everything, with legacy ids negative.

## Real-time safety

Chain structure and parameters change only in the command handler (same thread as `process`,
never concurrent). Instances are heap-allocated there. `FxChain::process` loops a fixed array of
resolved node pointers: no lookups, no allocation, no per-sample kind checks.

## Limitations

- At most `MAX_EFFECTS_PER_BUS` (16) instances per bus; each Delay ~0.8 MB and Reverb more, so
  memory scales with use (the old design preallocated all six per bus; effects are now created
  on demand).
- Adding a Delay/Reverb allocates on the audio thread's message handler (not in `process`);
  adding many in one tick can cause a brief glitch.
- Explicit instances have no automatic position relative to legacy ones: legacy instances are
  inserted before the first later-kind legacy instance, else appended.
- No per-instance dry/wet beyond each effect's own mix; no cross-bus sends or sidechain.
- `moveEffect` of a filter mid-stream is not crossfaded.
- The TS chain mirror (`getEffectChain`) is main-thread bookkeeping; it assumes the worklet
  accepted each command (it validates the same rules up front).

## Modulation and Utility

Every instance carries the same stackable LFO bank synths have (`setEffectLfo`, targets from
`getEffectModulationTargets(kind)`), and `EffectKind.Utility` is a one-parameter dB gain stage meant
for automatable track volume. See [`modulation.md`](./modulation.md).

## Phaser and Bitcrusher

Both are ordinary instances (`EffectKind.Phaser`, `EffectKind.Bitcrusher`) with `NodeParam`s,
inert (mix 0) until parameterized.

- **Phaser** (`native/src/dsp/phaser.h`): N first-order all-pass stages swept by a sine LFO,
  optional feedback; right channel's LFO leads by a quarter cycle. `PhaserRate` (Hz, 0.01..10),
  `PhaserDepth` (0..1, +-2 octaves), `PhaserCenter` (Hz, 100..5000), `PhaserFeedback`
  (-0.95..0.95), `PhaserStages` (rounded to even, 2..12), `PhaserMix`.
- **Bitcrusher** (`native/src/dsp/bitcrusher.h`): quantizes to exactly 2^bits levels and
  sample-and-holds every N frames, with no anti-aliasing (the aliasing is the effect).
  `CrushBits` (1..16), `CrushDownsample` (frames, 1..64), `CrushMix`. The hold is a frame
  count, not Hz, so its pitch effect scales with the sample rate.
