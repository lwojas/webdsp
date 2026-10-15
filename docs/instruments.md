# Synthesized instruments

How webdsp generates sound with DSP instead of sample playback, how a client application
integrates it, and how to add a new instrument. Kind-specific detail lives in one doc per
instrument — currently [`kick-instrument.md`](./kick-instrument.md).

## Concept

An **instrument** is an opinionated, purpose-built sound machine with a small, deliberately
designed control set — not a generic synth toolbox. The application says *"play a note, set
`decay` to 0.6"*; it never sets an oscillator frequency or filter cutoff. **The instrument owns
its sound design.**

```
Application ── notes + instrument params ──▶ Instrument (private DSP) ──▶ bus
                                                                           │
sample Voice ─────────────────────────────────────────────────────────────▶│
                                                       track FX chain ◀────┘
                                                            ▼
                                                  mixer → master FX → output (+ capture)
```

- **Instrument vs. effect.** An instrument is an audio *source* (like a sample `Voice`). An
  effect (`DSPNode`, FX chain) processes an existing signal. An instrument may use DSP blocks
  internally; that never couples it to the track/master FX architecture.
- **One instance = one monophonic machine.** A new note restarts it (like drum hardware). For
  overlapping parts, create several instances. The engine has no second voice-allocation policy.
- **Routing.** Each instance renders into a bus (`MASTER_BUS` or a `createBus()` track bus), so
  track insert FX, bus gain/pan, master FX and capture apply exactly as for samples.
- **Real-time safe.** Creating an instance allocates and runs on the command thread; `process()`
  never allocates (enforced by a test that counts allocations).

## Integrating in a client

```ts
import { AudioRuntime, InstrumentKind, KickParam, EffectKind } from "webdsp";

const runtime = await AudioRuntime.create({ workletModuleUrl });
const bus = runtime.createBus();
runtime.addEffect(bus, EffectKind.Saturation);          // ordinary track FX apply

// 1. Discover controls (from the engine; never hardcode ranges)
const params = runtime.getInstrumentParams(InstrumentKind.Kick);
//   [{ id, name: "decay", min: 0.05, max: 2, default: 0.45, unit: "s" }, ...]

// 2. Create an instance
const kick = runtime.createInstrument(InstrumentKind.Kick, { bus });

// 3. Shape it (live, any time; values are clamped to the advertised range)
runtime.setInstrumentParameter(kick, KickParam.Decay, 0.6);

// 4. Play — immediately, or sample-accurately in the future
runtime.instrumentNoteOn(kick, { velocity: 0.9 });
runtime.instrumentNoteOn(kick, { time: runtime.getCurrentTime() + 0.5, velocity: 1, pan: -0.2 });

// 5. Clean up / cancel
runtime.cancelScheduled();        // also drops pending instrument notes
runtime.removeInstrument(kick);
```

Notes:
- `time` is absolute seconds on `getCurrentTime()`'s clock, same as `schedule()`. Omitted or past
  means "now". Drive it from a lookahead scheduler exactly as for samples.
- `instrumentNoteOff` is an optional choke; one-shot drums end themselves.
- `getInstrumentParams` is populated from the engine's `ready` event
  (`getCapabilities().instruments`), so a generic UI can render knobs for *any* kind:
  label = `name`, range = `min..max`, initial = `default`, suffix = `unit`.
- Param ids are only meaningful relative to their kind (use that kind's enum, e.g. `KickParam`).
- A sequencer step that targets an instrument holds `{ instrumentId, velocity, pan }` instead of a
  `sampleId`; mixing sample and instrument events on one track is fine.
- Limits: `MAX_INSTRUMENTS` (64) live instances. Invalid commands are ignored (unknown id, NaN)
  or reported as a worklet `error` event (rejected create).

## Control model

| Level | Where | Examples |
|---|---|---|
| Per note | `instrumentNoteOn` | `velocity` (linear level), `pan` (−1..+1), `time` |
| Per instance, live | `setInstrumentParameter` | the kind's own table: `tune`, `decay`, `drive`… |
| Per bus | existing bus/FX API | gain, pan, insert effects |

Every parameter is described by `{ id, name, min, max, default, unit }`. The instrument clamps
values itself; non-finite values are ignored.

## Native contract (for engine authors)

`native/src/instrument.h`:

```cpp
class Instrument {
  kind(); configure(sampleRate);
  noteOn(velocity, delayFrames, pan);   // delayFrames = sample-accurate start inside a block
  noteOff(); setParam(id, value); reset();
  render(busChannels, busChannelCount, numFrames, scratch);  // accumulates; no allocation
  isActive();
};
```

Engine side (`engine.h`): `createInstrument / removeInstrument / instrumentNoteOn /
instrumentNoteOff / setInstrumentParam / scheduleInstrumentNote`. Notes use `EventQueue<T>` (the
generalised sample scheduler). Only *active* instruments render, so idle ones never wake a
dormant bus. C ABI: `webdsp_instrument_*`, `webdsp_schedule_instrument_note`,
`webdsp_instrument_param_{count,id,name,min,max,default,unit}`.

## Adding a new instrument

0. Not sure what to build? See the [synth DSP reference](./synth-dsp/README.md) for techniques,
   concepts and a ranked shortlist.
1. `native/src/instruments/<name>.h`: a class deriving `Instrument`, plus a
   `constexpr InstrumentParamInfo` table (the whole public surface). Reuse DSP from
   `native/src/dsp/` (envelopes, filters, saturation, panner) before writing new primitives.
2. `params.h`: add an `InstrumentKind` value and a `<Name>Param` enum.
3. `instrument_registry.h`: add cases to `makeInstrument`, `instrumentParamCount`,
   `instrumentParamInfo`.
4. TS: mirror the kind/param enums in `src/runtime/types.ts`, list the kind in
   `readInstrumentMetadata` (`src/worklet/engine-processor.ts`), and pin ids in
   `test/paramIds.test.ts`.
5. Tests as in `native/test/kick_test.cpp`: audible/bounded/deterministic, each param moves the
   expected aspect, scheduling accuracy, bus routing, zero allocation. WAV dump via `KICK_WAV_DIR`.
6. Write `docs/<name>-instrument.md` (signal path, parameter mapping, design notes).

Design guidance: few, musically meaningful controls; map one control to several internal
changes if that sounds better (e.g. `drive` = pre-gain + asymmetry + level trim); keep output
bounded; make rendering deterministic (reset phases/noise seeds on a fresh note); keep
retriggers continuous (carry phase/envelope level) to avoid clicks.

## Shared vs. per-instrument

- **Shared:** the `Instrument` contract, parameter metadata, registry, ids/lifecycle, scheduling,
  bus routing, TS/worklet command path, and the DSP building blocks.
- **Per-instrument:** the signal path, parameter set and mappings, constants.
- **Not built (yet):** polyphonic instances, per-instrument insert FX, presets, a generic
  oscillator/filter toolbox (deliberately avoided).

## Instrument reference

| Kind | Doc |
|---|---|
| `Kick` | [kick-instrument.md](./kick-instrument.md) |

Research / ideas for future kinds: [`synth-dsp/`](./synth-dsp/README.md).
