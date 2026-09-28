# webdsp — architecture

This is a standalone browser audio engine: a WASM/AudioWorklet runtime with a small,
deliberate public API, plus a React test UI that is only a *client* of that API. The engine
has no idea a "pad" or "sampler" exists. It knows about samples, voices, buses, DSP nodes,
and scheduled events. The test UI could be deleted and replaced by a tracker, a step
sequencer, or a MIDI instrument without touching `native/` or `src/runtime/`.

```
Application / UI  (src/app/**)          — React test harness, disposable
        |
        | commands / events
        v
Audio Runtime API  (src/runtime/**)     — the stable public boundary
        |
        v
AudioWorklet  (src/worklet/**)          — realtime thread, message-driven
        |
        v
WASM audio engine  (native/**)          — C++ / Emscripten
        +-- SampleStore    (sample_store.h)
        +-- VoiceManager   (voice_manager.h, voice.h)
        +-- Scheduler      (scheduler.h)
        +-- DSP            (dsp_node.h, dsp/biquad_filter.h, dsp/delay.h)
        +-- Bus / mixer    (bus.h)
        +-- Capture        (capture.h)
        |
        v
Web Audio output
```

## Getting started

```bash
npm install    # also runs `prepare` -> tsup builds lib/ from src/runtime + src/worklet
npm run dev    # Vite dev server for the demo app (src/app)

npm run test:all    # native pure-logic tests + wasm ABI smoke test + vitest
```

`src/worklet/generated/engine.js` (the compiled WASM engine) and `lib/` (the packaged
library — see below) are both **committed**, not gitignored, so the steps above work with no
native toolchain. You only need Emscripten if you're changing `native/`:

```bash
git clone https://github.com/emscripten-core/emsdk.git .emsdk
(cd .emsdk && ./emsdk install latest && ./emsdk activate latest)
npm run build:wasm    # recompiles native/src/api.cpp -> src/worklet/generated/engine.js
                       # commit the regenerated file alongside your native/ change
```

---

## Using this as a package

This repo is both the engine's source *and* an installable package — `npm install
<this-repo>` (git URL or a published registry version) gives you a working engine with no
Emscripten, no TypeScript build step, and no React, because `lib/` (built by `tsup.config.ts`
from `src/runtime` + `src/worklet` + `src/sequencing`) is committed and `files` in
`package.json` is what actually ships. `src/app/` — the demo UI — is never published; `react`/
`react-dom` are devDependencies of *this repo*, not of the package, since the engine itself
has zero React dependency by design.

```ts
import { AudioRuntime } from "webdsp";
import { defaultWorkletUrl } from "webdsp/worklet-url";
// import { compilePattern, LookaheadPlayer } from "webdsp/sequencing"; // optional

const runtime = await AudioRuntime.create({ workletModuleUrl: defaultWorkletUrl });
await runtime.resume(); // must follow a user gesture — browser autoplay policy
const sample = await runtime.loadSample(arrayBuffer, { name: "kick" });
runtime.trigger({ sampleId: sample.id, gain: 1.0 });
```

Verified directly (not just assumed from file layout): a plain HTML page importing straight
from `lib/index.js`/`lib/worklet-url.js` via native `<script type="module">` — no bundler at
all — successfully created the runtime, decoded a sample, triggered a voice, and reported it
in diagnostics, with zero console errors. If that works, any bundler built to emulate native
ESM resolution (Vite, webpack 5+, etc.) works too.

**Confirmed with a second, more thorough check: a real external consumer.** `webseq`, a
16-track tracker, was built as a separate repo depending on this package (`"webdsp":
"github:lwojas/webdsp"`), not just a smoke-test page. That surfaced two gaps the plain-HTML
check above couldn't catch — one fixed here, one that stays the consumer's responsibility:

- **Vite's *dev server* breaks `defaultWorkletUrl` — production builds don't.** The claim
  above ("any bundler... works too") holds for a production Vite build (confirmed: `webseq`
  produces a correctly-split `engine-processor.js` chunk). It does **not** hold out of the
  box under `vite dev`, because Vite's dependency pre-bundling (`optimizeDeps`, esbuild-
  powered) copies this package's modules into `node_modules/.vite/deps/` before serving
  them. `defaultWorkletUrl`'s `new URL('./engine-processor.js', import.meta.url)` then
  resolves relative to that *copied* location, not the real package directory —
  `engine-processor.js` isn't there, `addModule()` 404s, and `AudioRuntime.create()` throws
  "Unable to load a worklet's module." This can't be fixed inside `webdsp` (the package has
  no visibility into a consumer's `optimizeDeps` config); every Vite-based consumer needs to
  exclude it themselves:

  ```ts
  // consumer's vite.config.ts
  export default defineConfig({
    optimizeDeps: { exclude: ["webdsp", "webdsp/worklet-url"] },
  });
  ```

  See `webseq`'s `vite.config.ts` for this in context, including how it was diagnosed
  (network-request logging showed the worklet being requested from `.vite/deps/` instead of
  `node_modules/webdsp/lib/`).

- **`VoiceParam`/`NodeParam`/`FilterMode` are plain `enum`s, not `const enum`s — this one
  *was* fixed here, and stay that way.** A `const enum` compiles to an *ambient* `declare
  const enum` in a published package's `.d.ts` (tsup's declaration bundling has no other
  option), and TypeScript refuses to inline member access on an ambient const enum under
  `isolatedModules` (error TS2748) — a mode every Vite/esbuild-based consumer requires,
  since esbuild transpiles each file independently and can't see the enum's values. `webseq`
  hit this immediately trying to write `NodeParam.FilterCutoff`; switching to a regular
  `enum` fixed it for every future consumer, at the cost of a real (not inlined) runtime
  object instead of literal constants — negligible here since these are parameter *ids*
  passed to a couple of method calls, never touched on the audio render path. If you're
  tempted to switch back to `const enum` for that bundle-size win, don't.

**`workletModuleUrl` has no automatic default**, and this is deliberate, not an oversight:
`audioContext.audioWorklet.addModule()` always needs a real, fetchable URL, and *where a
package's own files are actually servable from* is a property of the consumer's bundler/host,
not something this package can know in general. `defaultWorkletUrl` (from the
`webdsp/worklet-url` subpath, above) covers the common case — it's computed via `new
URL('./engine-processor.js', import.meta.url)` inside `lib/worklet-url.js`, a pattern Vite,
webpack 5+, and native ESM all resolve correctly relative to the file's real installed
location. If your bundler doesn't support that pattern, copy `lib/engine-processor.js` to a
static/public directory yourself and pass its served URL instead. `AudioRuntime.create()`
throws a clear, actionable error (both at the type level and at runtime, for plain-JS callers)
if `workletModuleUrl` is omitted, rather than silently guessing a path the way an earlier
version of this code did (it used to default to this repo's own Vite dev-server paths via
`import.meta.env.DEV`, which only ever worked when this repo itself was the top-level app —
see `src/app/hooks/useAudioRuntime.ts` for how the demo app now supplies its own explicit
Vite-specific path instead of relying on a runtime default).

Why `defaultWorkletUrl` lives in its own subpath (`src/runtime/workletUrl.ts`) rather than
being exported from the main entry: the `new URL(..., import.meta.url)` pattern is only
correct once bundled into `lib/`, where `engine-processor.js` actually sits next to it. Vite's
production build of *this repo's own demo app* also transitively imports
`src/runtime/index.ts` — if that pattern lived there, Vite's static asset scanner would try to
resolve `src/runtime/engine-processor.js` (which doesn't exist) at build time and fail, the
same way it once failed on a stray `new URL()` reference left in a dead code branch (see git
history / the postmortem this section replaces). Keeping the pattern in a leaf module that
neither `index.ts` nor `src/app` imports sidesteps the collision entirely.

Published surface (`package.json` `exports`): `webdsp` (the runtime API), `webdsp/sequencing`
(the example pattern-compiler + lookahead scheduler — genuinely reusable by a real sequencer,
not demo-only), `webdsp/worklet-url` (the URL helper above), and `webdsp/engine-processor.js`
(the raw built worklet file, for tooling that wants to reference it directly rather than via
the URL helper).

---

## What belongs to the application

Everything under `src/app/` and `src/sequencing/`:

- Pads, pad→sample assignment, per-pad gain/pitch/start/end/loop/reverse controls
  (`src/app/components/Pad.tsx`, `PadGrid.tsx`).
- The example pattern data and role→sample resolution (`src/app/patterns/`).
- The lookahead scheduler that turns a pattern into timed events
  (`src/sequencing/lookaheadPlayer.ts`, `pattern.ts`) — this is *sequencer* code, not engine
  code, and a future tracker/step-sequencer would replace it entirely with its own event
  producer. It's published as the `webdsp/sequencing` subpath (see "Using this as a
  package") purely as a convenience starting point for exactly that kind of replacement —
  publishing it doesn't move it across the runtime boundary; `AudioRuntime` still has no
  idea it exists.
- Sample browser, diagnostics readout, capture UI, transport buttons — all in
  `src/app/components/`.
- The AudioContext bootstrap gesture (`src/app/hooks/useAudioRuntime.ts`).

None of this is reachable from `native/` or `src/worklet/`. The engine cannot construct a
"pad" if it wanted to — the type doesn't exist below `src/app/`.

## What belongs to the audio runtime

Everything under `src/runtime/`, `src/worklet/`, and `native/`:

- `AudioRuntime` (`src/runtime/index.ts`) — the public class. Samples, voice triggering,
  parameters, scheduling, capabilities, diagnostics, capture. Nothing else is exported for
  application use.
- The worklet bridge and command protocol (`src/runtime/workletBridge.ts`,
  `commandProtocol.ts`) — transport only, no business logic.
- The AudioWorkletProcessor (`src/worklet/engine-processor.ts`) — loads the WASM module,
  translates commands into ABI calls, copies rendered audio into the output buffers.
- The WASM engine itself (`native/`) — see below.

## What crosses the runtime boundary

Two things, and only two:

1. **The public TypeScript API** (`AudioRuntime` methods: `loadSample`, `trigger`,
   `release`, `stop`, `setVoiceParameter`, `setNodeParameter`, `createBus`, `schedule`,
   `cancelScheduled`, `startCapture`/`stopCapture`, `getCapabilities`, `getDiagnostics`,
   `getCurrentTime`). This is what an application is allowed to touch.
2. **The command/event protocol** between the main thread and the worklet
   (`src/runtime/commandProtocol.ts`) — an internal wire format, not exposed to
   applications, and free to change without breaking the public API.

The application never reaches into the WASM module, the AudioWorkletNode, or the
AudioContext directly. `AudioRuntime` owns all three.

---

## How samples are represented

`SampleMetadata` (`src/runtime/types.ts`) is `{ id, name, channels, sampleRate, length,
duration, byteLength }`. Nothing assumes two samples share a sample rate, channel count, or
duration — each is tracked independently, both in the JS-side `Map` (`AudioRuntime.samples`)
and in the native `SampleStore` (`native/src/sample_store.h`, keyed by the same numeric id).

Decoding uses the browser's own `decodeAudioData` (`src/runtime/decode.ts`), run once per
load on the main thread, via a throwaway `OfflineAudioContext` configured at the engine's own
sample rate — so the decoded PCM needs no further sample-rate conversion to play at unity
rate. The trade-off: `decodeAudioData` resamples to the deciding context's rate and does not
expose the source file's original sample rate, so `SampleMetadata.sampleRate` reports the
*post-decode* rate, not necessarily the file's native one. This was a deliberate choice — the
project brief explicitly prefers using browser decoding capabilities over writing a custom
demuxer, and the architecturally important property (per-voice resampling for pitch/rate
changes, independent of any given sample's rate) is unaffected either way; see `resampler.h`.

Decoded channel data is copied into WASM linear memory exactly once, at load time
(`engine-processor.ts`'s `"load-sample"` command handler: `malloc` a buffer per channel,
`HEAPF32.set` the data in, hand the pointers to `SampleStore::commit`). Triggering a voice
never touches JS, never allocates, and never re-copies sample data — it just points a `Voice`
at an existing `Sample*`.

## How voices work

`native/src/voice.h` / `voice_manager.h`. A `Voice` is a playing instance of a sample:
position (fractional, for interpolated playback), rate, gain, loop/reverse/start/end, an
optional filter insert, and a short linear release fade (`kReleaseSeconds`) so
stop/release/steal never clicks.

`VoiceManager` owns a pool sized by `maxVoices` — a runtime policy set at
`AudioRuntime.create({ maxVoices })` (default 64), not a hardware fact and not hardcoded
anywhere in the engine. Triggering always succeeds: it reuses an idle voice, or steals
whichever active voice currently has the lowest amplitude estimate. A sample can have any
number of simultaneous voices; the engine has no concept of "channels" mapping 1:1 to
samples.

### Voice handles

`AudioRuntime.trigger()` returns a `VoiceHandle` **synchronously**, even though the actual
command is delivered to the worklet asynchronously via `postMessage`. This works because the
id is allocated on the main thread (a monotonic counter) and sent *as part of* the trigger
command — the native `VoiceManager` uses that externally-supplied id as its lookup key rather
than generating its own and reporting it back. No round trip, no dangling-until-confirmed
handle, and a sequencer can call `release()` on a handle before the corresponding `trigger`
has even reached the audio thread (it will simply find the voice already there once it
processes both messages, in order). `release()`/`stop()` on an id that's already finished or
been stolen is a no-op by design.

## How scheduling works

`native/src/scheduler.h`. The engine's *entire* sequencing surface is "trigger this at this
absolute frame." `Scheduler` holds a time-sorted queue; `Engine::process()`
(`native/src/engine.h`) drains every due event each render quantum and triggers the
corresponding voice with a computed **sub-block delay** (`Voice::trigger`'s `delayFrames`
parameter) so an event landing mid-block still starts on the exact sample, not quantized to
the ~2.7 ms (128-frame @ 48 kHz) block boundary.

The authoritative clock is the render thread's own frame count
(`AudioWorkletGlobalScope.currentFrame`/`currentTime`, read directly in
`engine-processor.ts`) — never a JS timer and never React state. The example sequencer
(`src/sequencing/lookaheadPlayer.ts`) uses a `setInterval` purely to decide *when to top up*
the schedule a little further into the future (the classic "lookahead scheduler" pattern);
the actual playback timing comes entirely from the absolute times it hands to
`AudioRuntime.schedule()`, which the engine executes sample-accurately regardless of when the
message happens to arrive, as long as it arrives before the deadline. This is why using a
timer here does not violate "don't use JS timers as the audio clock" — the timer schedules
into a clock it doesn't own.

The UI playhead (`src/app/hooks/usePlayhead.ts`) is a `requestAnimationFrame` loop that reads
`LookaheadPlayer.getPlayheadStep()`, itself derived every call from
`AudioRuntime.getCurrentTime()`. It is a visualization, not an input — nothing about audio
timing depends on React rendering at all.

## How DSP is composed

`native/src/dsp_node.h`. There is no "effects" subsystem — only `DSPNode`, a tiny interface
(`process(channels, numChannels, numFrames)`, `setParam`, `reset`), and `DSPChain<N>`, a
fixed-capacity ordered list of them. `Voice` owns a one-node chain with a filter
(`dsp/biquad_filter.h`, RBJ cookbook, switchable low-pass/high-pass via
`VoiceParam.FilterMode`). `Bus` (`native/src/bus.h`) owns a two-node chain: the *same*
`BiquadFilter` class (so the master bus gets the identical low-pass/high-pass/cutoff/
resonance capability a voice has, just applied to the whole mix) followed by a delay
(`dsp/delay.h`, feedback delay line). `NodeParam` and `VoiceParam` are deliberately separate
numeric id spaces (see `params.h`) even though `Bus`'s filter and `Voice`'s filter are the
same class — `Bus::setParam` translates its own `NodeParam.FilterCutoff/FilterResonance/
FilterMode` ids into the `VoiceParam` ids `BiquadFilter::setParam` actually understands
before forwarding, rather than the two enums sharing values. Don't assume a `NodeParam` and
a `VoiceParam` with the same integer mean the same thing — they usually don't
(`NodeParam.BusGain == 3 == VoiceParam.FilterResonance`, for instance).

Both `Voice`'s and `Bus`'s nodes are ordinary `DSPNode`s; adding a third kind of node
(saturation, compression, reverb) means writing one more class with that same three-method
interface and adding it to a chain — no change to `Voice`, `Bus`, or the chain mechanism
itself. See "Adding a new master-bus module" just below for exactly what that does and
doesn't require.

### Buses / mixing

`Engine` owns one `Bus` as the master bus (`MASTER_BUS`, busId 0) plus a fixed pool of
`kMaxTrackBuses` (`native/src/engine.h`) additional ones, all pre-allocated at `init()` —
same shape as the voice pool, so handing one out is never a render-thread allocation. An
application gets one via `AudioRuntime.createBus()` (a synchronous main-thread counter bump
against `MAX_TRACK_BUSES` in `src/runtime/types.ts`, kept in sync with the native constant by
hand, not by a generated binding — no command crosses the worklet boundary to allocate one).

Routing and mixing, once per render quantum (`Engine::process`):

1. Every active voice renders into *its own bus's* accumulator buffer (selected by
   `Voice::busId()`, defaulting to master for `busId <= 0` or out of range) — not a single
   shared buffer, so one bus's voices never bleed into another's before that bus's own chain
   runs.
2. Each track bus runs its own filter+delay chain (`Bus::process`) over just its own
   accumulator, then that result is summed into the master accumulator — this sum *is* the
   mixer; there's no separate "Mixer" class.
3. The master bus then runs its own chain over the combined signal (its own direct voices,
   if any, plus every track bus's output) before that becomes the engine's output.

This is: `track voices -> track bus chain -> [sum] -> master bus chain -> output`, matching
an application's likely mental model of "per-channel FX into a master FX chain" exactly,
using the same `Bus`/`DSPChain` machinery for both stages. A freshly-allocated track bus is
inert (`BiquadFilter` starts `bypassed_`, `Delay` starts at `mix_ = 0`) — see "Adding a new
master-bus module" below, unchanged by this: `setNodeParameter`/`NodeParam` work identically
on `MASTER_BUS` and on any `createBus()` result.

### Adding a new master-bus module

Two different things can look like "add a module to the master bus," with very different
cost — check which one you actually need before assuming an engine change is required:

- **Exposing a DSP node that's already implemented.** `Bus` has carried a `Delay`
  (`NodeParam.DelayTime`/`DelayFeedback`/`DelayMix`) since before the filter was added — no
  client application has built UI for it yet, but it needs **zero** engine changes to use
  today: an app can add a "Delay" module purely on the client side by calling
  `setNodeParameter(MASTER_BUS, NodeParam.DelayTime, ...)` etc. Always check `NodeParam`/
  `VoiceParam` (`src/runtime/types.ts`) before assuming a capability is missing — it might
  already be wired in and simply unused by any UI so far.
- **A genuinely new DSP algorithm** (reverb, compression, EQ, saturation, ...). This *does*
  require an engine change: a new `DSPNode` subclass under `native/src/dsp/`, new
  `NodeParam`/`VoiceParam` values in both `params.h` and `types.ts` kept in sync (pinned by
  `test/paramIds.test.ts`), wiring the node into `Bus`'s chain (bumping `DSPChain<N>`'s
  capacity — currently 2), and a WASM rebuild (`npm run build:wasm`, requires Emscripten —
  see "Getting started"). There's no scripting/plugin-loading mechanism for arbitrary
  application-supplied DSP, deliberately — see "DSP library investigation" below for why,
  and Faust/faustwasm as the documented path if the effects list grows enough to want one.
  An application's own module-strip UI (a `{ id, name, parameters, enabled }`-shaped
  abstraction, e.g. `webseq`'s `AudioModule`) never needs to change shape for either case —
  only this repo needs touching for the second one.

**DSP library investigation** (project brief section 7 asked this be done before hand-rolling
anything): mature C/C++ DSP libraries were considered and deliberately not adopted for v1 —

- **libsamplerate** — high-quality, but block-oriented; not a good fit for a continuously
  modulated per-sample-frame read (real-time pitch/rate), and vendoring it purely for
  one-time load-time conformance (which the browser's decoder already handles, see above)
  would be exactly the "large dependency merely because it exists" the brief warns against.
- **Faust / faustwasm** — an excellent fit for *growing the effects library* later (each new
  effect becomes a small `.dsp` file compiled to a WASM module implementing the same
  `DSPNode`-shaped ABI), but it's a second build toolchain and, for `faustwasm`, an
  npm/online-compiler dependency — at odds with keeping v1's toolchain to just Emscripten and
  fully offline. Documented here as the recommended path for effect #3 onward, not adopted
  yet.
- **RBJ Audio EQ Cookbook** biquad formulae — adopted directly (`dsp/biquad_filter.h`): this
  is the standard, well-documented technique, small enough to own and unit-test, and not
  meaningfully improved on by a general-purpose library for a single lowpass filter.

Interpolated resampling (`native/src/resampler.h`, cubic Hermite / Catmull-Rom, with linear
as a cheaper fallback) is likewise hand-written rather than pulled from a library, for the
same reason: it's a small, standard, independently-testable algorithm
(`native/test/resampler_test.cpp`), not a novel one.

## How future sequencers can integrate

Nothing in `src/runtime/` or `native/` changes. A tracker, step sequencer, or piano roll
needs only to:

1. Produce `ScheduledEvent[]` — `{ time, sampleId, gain?, pitch?, start?, end?, loop?,
   reverse?, bus?, duration? }` (`src/runtime/types.ts`). No `padId`, no step index, no
   track number. `duration` (seconds) auto-releases the voice that many seconds after
   playback starts, computed on the audio render thread — the mechanism a tracker/sequencer
   needs for note length; don't reach for a second timed `release()` call or a JS timer.
2. Call `AudioRuntime.schedule(events)` (for events already known ahead of time) or
   `AudioRuntime.trigger(params)` (for live/real-time input, e.g. a MIDI note-on).
3. Optionally read `getCurrentTime()`/`getDiagnostics()` for its own UI.

`src/sequencing/pattern.ts` + `lookaheadPlayer.ts` are one example of step (1) for a
step-sequencer-shaped input; they are explicitly *not* part of the runtime API and are not
meant to be the permanent representation of anything (project brief section 9).

## Where WASM is used

The entire DSP/voice/scheduling core (`native/`) is C++ compiled with Emscripten
(`native/build.sh`) to a single WASM module with `MODULARIZE`+`EXPORT_ES6`. The ABI
(`native/src/api.cpp`) is a flat `extern "C"` surface — numbers and pointers only, no
embind — kept deliberately small and readable top-to-bottom as the contract it is, rather
than generating bindings for a large surface.

Two separate builds come out of `native/build.sh`, both from the same source:

- `src/worklet/generated/engine.js` — `ENVIRONMENT=worker`, what the browser actually loads.
- `native/build/engine.node.mjs` — `ENVIRONMENT=node`, used only by
  `native/test/smoke.mjs` so the compiled ABI can be exercised without a browser. Building
  one artifact for both would leak a Node-only code path into the shipped bundle (this was
  tried first; Vite correctly flagged it as an externalized `node:module` import).

**Why a flat extern "C" ABI and not Rust/wasm-bindgen or Emscripten's embind:** the project
started with no WASM toolchain installed at all (no rustc, no emcc). Emscripten + C++ was
chosen because the environment already had a usable Homebrew/Xcode C++ toolchain to prototype
against locally before ever touching `emcc`, and because a hand-written flat ABI keeps the
JS↔WASM boundary auditable in one file (`api.cpp`) rather than behind a code generator.

### Threading model

Everything in `native/` runs on a single thread: the browser's dedicated audio-rendering
thread that also owns the `AudioWorkletProcessor`. Command handling
(`engine-processor.ts`'s `port.onmessage`) and rendering (`process()`) never execute
concurrently — the browser guarantees they alternate on the same thread — so `SampleStore`,
`VoiceManager`, and friends need no locks, atomics, or `pthread` build flags. This is also
why no `SharedArrayBuffer`/`Atomics` command queue was built for v1: the realtime hazard
`SharedArrayBuffer` would address (JS main thread blocking the render thread) doesn't exist
here, since main-thread↔worklet communication is already async `postMessage`, never a
blocking call. `vite.config.ts` still sets the COOP/COEP headers that `SharedArrayBuffer`
would require, so adopting it later (e.g. for lower command latency/jitter than `postMessage`
provides) needs no deployment change — see "Intentionally deferred".

### A note on diagnostics (a real bug found during verification)

`engine-processor.ts` originally measured render time via `performance.now()` for a `cpuLoad`
diagnostic. This throws — `AudioWorkletGlobalScope` does **not** get `performance` (it isn't
part of the `WindowOrWorkerGlobalScope` mixin worklets receive), and an uncaught exception in
`process()` causes Chrome to silently stop calling it forever. This was caught during manual
browser verification (a real Chromium instance, sample loading + triggering never showed up
in diagnostics) rather than by the type checker, since the ambient `.d.ts` for the worklet
scope didn't declare `performance` at all — TypeScript had no way to know either way. Fixed
by removing the measurement rather than guessing at a workaround, and by wrapping both
`process()` and the command handler in `try`/`catch` that reports failures as `"error"`
`WorkletEvent`s instead of letting an exception anywhere disable the node permanently — a
malformed command should degrade gracefully, not take down the whole audio graph. `cpuLoad`
and `underruns` were removed from `RuntimeDiagnostics` entirely rather than left as
fields that always read a misleading `0`; see "Intentionally deferred" for how they could be
added back.

---

## Automated tests for the non-realtime parts

- **`native/test/*_test.cpp`** — pure C++ logic (`resampler.h`'s interpolation,
  `scheduler.h`'s ordering/boundary behavior), compiled natively with the system compiler
  (`clang++`, no `emcc` needed) for fast iteration. Run via `native/test/run.sh`.
- **`native/test/smoke.mjs`** — exercises the actual compiled WASM ABI under Node
  (`native/build/engine.node.mjs`): sample commit, polyphony, sample-accurate mid-block
  scheduling, release/reclaim, capture. This is the one test that touches the real compiled
  artifact rather than a native recompile of the same source, so it's what would catch an
  ABI-level regression (wrong argument order, a renamed export).
- **`test/*.test.ts`** (vitest) — `paramIds.test.ts` pins the `VoiceParam`/`NodeParam`
  numeric contract with `native/src/params.h` (no cross-language check exists, so this is
  what catches a silent reorder); `pattern.test.ts` / `resolvePattern.test.ts` cover the
  sequencing example's pure logic; `lookaheadPlayer.test.ts` drives the lookahead scheduler
  against a faked `AudioRuntime` (`getCurrentTime`/`schedule`/`cancelScheduled`) under fake
  timers — this is also what caught a real ordering bug (see below).

**Deliberately not unit-tested:** `AudioRuntime`, `WorkletBridge`, `decode.ts`, and the
worklet itself all require a real `AudioContext`/`AudioWorklet`/`decodeAudioData`, none of
which exist under Node or jsdom. That integration seam was instead verified manually against
a real headless Chromium instance (sample load → decode → pad trigger → polyphony → voice
stealing → sample-accurate sequencing → capture round-trip, all confirmed with zero console
errors) rather than faked into a false sense of coverage.

**A second real bug caught by the tests, not just the browser check:** the first version of
`LookaheadPlayer.start()` called the initial `tick()` *before* arming `setInterval`. A
non-looping pattern that fit entirely within the first lookahead window would call
`stop()` synchronously from inside that initial `tick()` — but `stop()` only clears a timer
that already exists, so the subsequent (now-dead) `this.timerId = setInterval(...)` line
silently resurrected a player that had just legitimately finished. Writing
`lookaheadPlayer.test.ts`'s "stops itself after one pass" case surfaced this immediately;
fixed by arming the interval first.

---

## Intentionally deferred

Left out of v1 on purpose, with the extension point noted:

- **Dynamic/arbitrary DSP chains.** `DSPChain` is fixed-order and fixed-capacity, built once
  at construction (filter on `Voice`, delay on `Bus`). Reordering or inserting nodes from the
  application would need a chain-mutation command — straightforward given the existing
  `DSPNode` interface, just not built for v1's two-node demonstration.
- **A wider effects library via Faust.** See "DSP library investigation" above.
- **`SharedArrayBuffer`/`Atomics` command queue.** `vite.config.ts` already sets the required
  COOP/COEP headers. Would lower command latency/jitter versus `postMessage`; not needed
  today because nothing on the realtime path currently blocks on it.
- **CPU load / underrun diagnostics.** Removed (not stubbed) after discovering
  `AudioWorkletGlobalScope` has no `performance.now()` and the Web Audio API exposes no
  per-node underrun counter. A future version could get a coarse CPU estimate by comparing
  `currentFrame` cadence against a periodic main-thread-timestamped round trip, or true
  underrun detection via `AudioContext` state/glitch signals as they become available.
- **Persistent sample storage (IndexedDB/OPFS).** Samples currently live only in WASM
  memory for the session — reloading the page re-decodes from the original files. The
  `SampleMetadata`/`SampleId` model doesn't change to add persistence; only `decode.ts`'s
  load path would grow a cache-check step.
- **True source sample-rate preservation.** See "How samples are represented" — deferred
  behind bypassing `decodeAudioData` for a custom demuxer, which the project brief's
  preference for browser-native decoding argues against for v1.
- **Per-voice pan, ADSR envelope, per-voice send buses.** Not requested; `Voice` has exactly
  the parameters section 5 of the brief asked for (gain, rate/pitch, start, end, loop,
  reverse, trigger/retrigger, release/stop) plus the filter demonstrating DSP composability.
- **Capture duration cap (30s, `native/src/capture.h`).** Capacity is reserved up front so
  the realtime `appendBlock()` path never allocates; a streaming/backed-by-disk capture
  would remove the cap without changing the `Capture` class's public shape.
