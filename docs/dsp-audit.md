# DSP node / application-facing capability audit

ECS-13. Inspects `FilterNode`/`DelayNode`-equivalents, `DSPChain`, `Bus`, the shared
native/TypeScript parameter contract, `test/paramIds.test.ts`, the WASM build, and current
test coverage, as of commit `bd633ca`. Records what exists and what's wired vs. exposed —
no architecture proposal, per the ticket.

## 1. DSP node inventory

There is no "effects" subsystem — only `DSPNode` (`native/src/dsp_node.h:9`), a 3-method
interface (`process`, `setParam`, `reset`), and `DSPChain<Capacity>`
(`native/src/dsp_node.h:24`), a fixed-order, fixed-capacity array of node pointers.

Two concrete `DSPNode` implementations exist today:

| Node | File | Params (id space) | Notes |
|---|---|---|---|
| `BiquadFilter` | `native/src/dsp/biquad_filter.h` | `VoiceParam.FilterCutoff/FilterResonance/FilterMode` (voice) or `NodeParam.FilterCutoff/FilterResonance/FilterMode` (bus, translated — see §3) | RBJ cookbook low-pass/high-pass. `bypassed_` starts `true`; any `setParam` call un-bypasses it. |
| `Delay` | `native/src/dsp/delay.h` | `NodeParam.DelayTime/DelayFeedback/DelayMix` | Feedback delay line, per-channel ring buffer, 2s max window. `mix_` starts `0.0` (bypass). |

Both are instantiated twice each: once per `Voice` (`Voice` owns only a `BiquadFilter`,
`native/src/voice.h:185`) and once per `Bus` (`Bus` owns one `BiquadFilter` + one `Delay`,
`native/src/bus.h:52-53`). No reverb, compressor, saturation, chorus, or phaser node exists
anywhere in `native/src/` — confirmed by `find native/src/dsp -type f`, which returns only
`biquad_filter.h` and `delay.h`. Any of those would be new engine work (§5), not exposure of
something already present.

## 2. Bus / chain wiring

- `Voice` (`native/src/voice.h`) has **no chain at all** — it calls
  `filter_.process(...)` directly (`voice.h:149`), a single hardcoded node, not a
  `DSPChain`.
- `Bus` (`native/src/bus.h:21-24`) builds `DSPChain<2>` in `configure()`: `filter_` added
  first, `delay_` second, in that fixed order, every time. There is no code path that
  builds a `Bus` with a different node set or order.
- **Chain capacity is `2`**, hardcoded as the `DSPChain` template parameter
  (`native/src/bus.h:21`, `DSPChain<2> chain_;` at `bus.h:54`). `DSPChain::add()`
  (`dsp_node.h:27`) silently no-ops past capacity — a third `add()` call would compile and
  run but the third node would never process. Nothing currently attempts a third `add()`.
- `Engine` (`native/src/engine.h`) owns exactly one `masterBus_` (`engine.h:200`) plus a
  pre-allocated pool of `kMaxTrackBuses = 32` (`engine.h:29,42-43`) track buses, mirrored on
  the TS side by `MAX_TRACK_BUSES = 32` (`src/runtime/types.ts:22`). Signal flow per block
  (`Engine::process`, `engine.h:120-181`): each track bus's voices accumulate into that
  bus's own buffer → that bus's chain (filter → delay) runs → result sums into the master
  accumulator → `masterBus_`'s own chain (filter → delay) runs → output. One `Bus` class,
  same two-node chain shape, used for both stages.
- `AudioRuntime.createBus()` (`src/runtime/index.ts:223-228`) is a synchronous main-thread
  counter bump against `MAX_TRACK_BUSES` — no worklet round-trip, since every track bus is
  pre-allocated at `init()`. A freshly-allocated bus is inert (filter bypassed, delay mix 0)
  until parameterized.

## 3. Shared native/TypeScript parameter contract

Two independent numeric id spaces, manually kept in sync between
`native/src/params.h` and `src/runtime/types.ts`, pinned by `test/paramIds.test.ts` (no
cross-language check exists otherwise):

```
VoiceParam         NodeParam                FilterMode (shared value space, not an id)
Gain            0  DelayTime          0     LowPass   0
Rate            1  DelayFeedback      1     HighPass  1
FilterCutoff    2  DelayMix           2
FilterResonance 3  BusGain            3
FilterMode      4  FilterCutoff       4
                   FilterResonance    5
                   FilterMode         6
```

Confirmed identical in `native/src/params.h`, `src/runtime/types.ts`, and asserted
literal-for-literal in `test/paramIds.test.ts` (all three currently agree — no drift).

Numbers overlap across the two enums with different meanings
(`NodeParam.BusGain == 3 == VoiceParam.FilterResonance` — a bug if the two id spaces were
ever accidentally mixed at a call site). `Bus::setParam` (`bus.h:37-49`) explicitly
translates its own `NodeParam.FilterCutoff/FilterResonance/FilterMode` into the
`VoiceParam` ids `BiquadFilter::setParam` understands before forwarding — this translation
is the only thing preventing that collision from being live; anything that bypassed
`Bus::setParam` and called `BiquadFilter::setParam` with a raw `NodeParam` value would be
wrong (e.g. `NodeParam.FilterCutoff = 4` would be read as `VoiceParam.FilterMode = 4`,
not `VoiceParam.FilterCutoff = 2`).

Both TS enums are plain `enum` (not `const enum`), deliberately — an ambient
`const enum` breaks under `isolatedModules` for a published package's consumers (see
`ARCHITECTURE.md`, and `8d9eee5`/recent history around this repo's own const-enum fix).

## 4. End-to-end wiring check: what's implemented vs. exposed to the app

Traced `setNodeParameter`/`createBus` end-to-end to confirm the full path is live, not
just declared:

`AudioRuntime.setNodeParameter(bus, param, value)` (`src/runtime/index.ts:212-214`)
→ `{ type: "set-bus-param" }` command (`src/runtime/commandProtocol.ts:39`)
→ `engine-processor.ts:114-115` → `Module._webdsp_set_bus_param`
→ `native/src/api.cpp:81-83` → `Engine::setBusParam` (`engine.h:75-81`)
→ `Bus::setParam` (`bus.h:37-49`) → `Delay::setParam` / `BiquadFilter::setParam`.

This chain is real and functions (see §6 for what's tested). **No application code calls
any part of it.** Searched `src/app/` (all components + hooks) for `NodeParam`,
`VoiceParam`, `setNodeParameter`, `setVoiceParameter`, `createBus`, `FilterMode` —
zero matches. Concretely, the app UI currently exposes only:

- **Pad controls** (`src/app/components/Pad.tsx:5-10`): gain, pitch, start, end, loop,
  reverse — a `TriggerParams` subset. No filter controls (per-voice `VoiceParam.FilterCutoff/
  FilterResonance/FilterMode` are unused by the UI, same as the bus params below).
- **SequencerPanel, SampleLoader, CapturePanel, Diagnostics**: pattern playback, sample
  loading, capture start/stop, and read-only diagnostics — none call
  `setNodeParameter`/`setVoiceParameter`/`createBus`.

So, specifically answering the ticket's question — **`NodeParam.DelayTime`,
`DelayFeedback`, and `DelayMix` on the master bus already exist, are already wired
correctly through the full ABI, and require zero engine/native changes to use.** An
application can add a "Delay" module today purely by calling
`runtime.setNodeParameter(MASTER_BUS, NodeParam.DelayMix, 0.3)` (etc.) from `src/app/` —
this is a UI-only gap, not a DSP capability gap. The same is true of:

- Master/track-bus filter (`NodeParam.FilterCutoff/FilterResonance/FilterMode`) — engine
  has carried this since before delay was added (per `bus.h`), unused by any UI control.
- Track-bus gain (`NodeParam.BusGain`) and `createBus()` itself — no UI creates a second
  bus or routes any pad to one; every trigger currently goes to `MASTER_BUS` implicitly
  (`Pad`/`SequencerPanel` never pass `bus` in `TriggerParams`).
- Per-voice filter (`VoiceParam.FilterCutoff/FilterResonance/FilterMode`) — same situation
  at the voice level.

## 5. Path for adding a new node

Two different asks, different cost — established in `ARCHITECTURE.md` ("Adding a new
master-bus module") and reconfirmed by the code read above:

**A. Expose an already-implemented capability (e.g. delay, bus filter).** Zero engine
changes. Add a UI control in `src/app/` that calls the existing `setNodeParameter`/
`setVoiceParameter`/`createBus` methods with the existing `NodeParam`/`VoiceParam` ids.
This is pure application work.

**B. Add a genuinely new DSP algorithm (reverb, compression, saturation, chorus,
phaser — everything on the source plan's list).** Required engine changes, concretely:

1. New `DSPNode` subclass under `native/src/dsp/` (3 methods: `process`, `setParam`,
   `reset`), same shape as `biquad_filter.h`/`delay.h`.
2. New `VoiceParam`/`NodeParam` values added to **both** `native/src/params.h` and
   `src/runtime/types.ts`, kept numerically in sync by hand (no generated binding) and
   pinned by adding assertions to `test/paramIds.test.ts`.
3. Bump `DSPChain`'s capacity past `2` wherever the new node is inserted
   (`Bus::configure`, `bus.h:21`, currently `chain_ = DSPChain<2>{}` /
   `DSPChain<2> chain_;`) and call `chain_.add(&newNode_)`. If the node is per-voice
   instead (like the filter), `Voice` would need its own chain built (it currently calls
   its one `BiquadFilter` directly, with no `DSPChain` at all — see §2).
4. Wire `Bus::setParam` (or `Voice::setParam`) to route the new param ids to the new
   node's `setParam`, same pattern as the existing filter-id translation (§3).
5. WASM rebuild: `npm run build:wasm` (requires Emscripten via `.emsdk/`, present in this
   checkout), regenerating `src/worklet/generated/engine.js`, which is committed.
6. Application-side UI to actually surface the new control (§4's gap, again).

No scripting/plugin-loading mechanism exists for application-supplied DSP; adding a node
always means a native source change + WASM rebuild, never a runtime-loadable module.

## 6. Test coverage

| Layer | What's covered | What's not |
|---|---|---|
| `test/paramIds.test.ts` (vitest) | Every `VoiceParam`/`NodeParam`/`FilterMode` literal value, both enums, against hardcoded expected numbers | Pure numeric contract only — asserts nothing about behavior |
| `native/test/smoke.mjs` (compiled WASM ABI, via `native/build/engine.node.mjs`) | Master-bus filter (`NodeParam.FilterCutoff`/`FilterMode`) measurably attenuates an audible tone; **track-bus filter isolation** — bus 2's filter affects only bus-2 voices, verified not to leak onto bus 0, via three RMS comparisons (`native/test/smoke.mjs:154-211`) | **`Delay` node is never exercised at runtime anywhere** — no test sets `DelayTime`/`DelayFeedback`/`DelayMix` and checks for delayed/wet output. `NodeParam.BusGain` is also never exercised. `createBus()` (the TS API) is not called by any test — the smoke test reaches track bus 2 by passing `busId=2` directly to the native `webdsp_trigger`/`webdsp_set_bus_param` ABI, not through `AudioRuntime.createBus()`. |
| `native/test/*_test.cpp` (native, no emcc) | `resampler.h` interpolation, `scheduler.h` ordering/boundaries | No DSP node logic (filter/delay) — those are only covered via the compiled-WASM smoke test, not pure C++ unit tests |
| vitest (`test/*.test.ts`) | `pattern.ts`/`resolvePattern.ts`/`lookaheadPlayer.ts` (sequencing layer, unrelated to DSP nodes) | No test touches `AudioRuntime.setNodeParameter`/`setVoiceParameter`/`createBus` from the TS side at all (consistent with §4 — nothing in `src/app` calls them either) |

**Net gap:** `Delay` (the exact capability named in the ticket) has correctness verified
nowhere — not in a native unit test, not in the WASM smoke test, not in vitest. Its only
"test coverage" is the id-pinning in `paramIds.test.ts`, which would still pass even if
`Delay::process`/`setParam` were broken.

## 7. Concrete gaps (no proposed rewrite)

1. **UI gap, not engine gap:** master-bus delay, master/track-bus filter, track-bus gain,
   per-voice filter, and multi-bus routing are all implemented and reachable through the
   public `AudioRuntime` API today, with zero application code using any of them (§4).
2. **Test gap:** `Delay` node has no behavioral test at any layer (§6) — the highest-risk
   gap given it's the ticket's specific example of an "already exists" capability.
3. **Test gap:** `NodeParam.BusGain` and `AudioRuntime.createBus()` (the TS-level API, as
   opposed to raw `busId` integers) are untested.
4. **Real capacity ceiling:** `DSPChain<2>` is hardcoded on `Bus` (`bus.h:21`) — adding any
   third bus-level node (reverb, compressor, etc.) requires touching that literal, not just
   adding a class. `Voice` has no chain at all (single hardcoded `BiquadFilter` field) —
   adding a second per-voice node requires introducing a `DSPChain` to `Voice` for the
   first time, a slightly larger change than bumping `Bus`'s capacity.
5. **No new DSP algorithms exist yet:** reverb, compressor, saturation, chorus/flanger,
   and phaser (the source plan's investigation list) all require new `native/src/dsp/`
   classes — none are partially implemented or stubbed anywhere in the current source.
