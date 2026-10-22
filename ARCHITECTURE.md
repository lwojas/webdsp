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
        +-- DSP            (dsp_node.h, dsp/biquad_filter.h, dsp/delay.h, dsp/reverb.h,
        |                   dsp/compressor.h, dsp/phaser.h, dsp/bitcrusher.h)
        +-- Bus / mixer    (bus.h)  +-- FX chain (fx_chain.h)
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
   `cancelScheduled`, `startCapture`/`stopCapture`, `armCapture`, `getCapabilities`,
   `getDiagnostics`, `getCurrentTime`). This is what an application is allowed to touch.
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

### Envelopes

Full reference and consumer guide: [`docs/adsr-envelope.md`](./docs/adsr-envelope.md).

Every voice has an ADSR amplitude envelope (`native/src/dsp/adsr_envelope.h`), applied per
frame in `Voice::render` alongside the sample/voice gains. Three layers, deliberately separate:

| Layer | What | Where |
| --- | --- | --- |
| Public control model | `EnvelopeParams { attack, decay, sustain, release }` — seconds and a 0..1 level, via `AudioRuntime.setSampleEnvelope()` or `loadSample({ envelope })` | `src/runtime/types.ts`, `index.ts` |
| Instrument abstraction | The envelope is a persistent property of the instrument (`Sample`, via `SampleParam.EnvAttack..EnvRelease`), exactly like `SampleParam.Gain`. A future instrument (VA kick, synth) decides which controls it exposes and maps them onto its own `AdsrParams`. | `params.h`, `sample_store.h` |
| DSP | `AdsrEnvelope`: a plain class, no knowledge of voices or samples; takes `AdsrParams`, derives per-frame steps itself. Owned by `Voice` today; a pitch/filter envelope is just another instance. | `dsp/adsr_envelope.h` |

Lifecycle: `trigger` = note-on (attack -> decay -> sustain); `release()` or the trigger's
`duration` = note-off (release); the voice is `Releasing` until the envelope reaches exactly 0,
then goes idle and is reported through `endedVoices`. The voice allocator is unchanged: a
releasing voice is not idle, so it is never reused while idle voices exist, and stealing (quietest
`amplitudeEstimate`, now using the live envelope level) is the only way to take it.

Behaviour choices: segments are linear in amplitude and exact in length (frame counted, rounded
to the nearest frame, minimum one frame for any positive time; 0 = instant). Retrigger of a
still-active voice (steal) does not reset the envelope: attack ramps from the current level, so
the envelope itself never jumps. Live edits are deterministic: attack, decay and sustain are captured at
note-on (they only affect notes triggered afterward); release is read at note-off (it affects
notes still held). Retrigger from any stage (attack, decay, sustain, release) ramps attack from
the current level over the full attack time; a stage of N frames reaches its target on frame N. Defaults (attack 0, decay 0, sustain 1,
release 10 ms) reproduce the pre-ADSR behaviour exactly. A non-looping sample that reaches its
end still ends the voice immediately, regardless of the envelope. Times are clamped to 0..30 s,
sustain to 0..1. Processing is allocation-free (verified by `native/test/adsr_test.cpp`).

### Panning

Voices, buses and samples (instrument default, `SampleParam.Pan`, summed with the voice pan) carry a stereo pan (`-1..+1`, default centre = unchanged behaviour):
constant-power for mono samples, balance for stereo samples and buses, smoothed ~5 ms. Full
reference: [`docs/panning.md`](./docs/panning.md). Output is fixed stereo; a mono sample is
duplicated onto both channels in `Voice::render`, a stereo sample plays natively.

### Gain staging

Each `Sample` also carries its own persistent gain (`SampleParam.Gain`, default unity — see
`native/src/sample_store.h`), set via `AudioRuntime.setSampleParameter()` or `loadSample()`'s
`gain` option. This is the engine's answer to "where does an instrument's own level belong,"
as distinct from `TriggerParams.gain`/`VoiceParam.Gain` (a per-trigger/event level — velocity
today, a future per-cell level) and `NodeParam.BusGain` (a bus's own output stage — see "How
DSP is composed" below). All three multiply together rather than collapsing into one number,
giving the full signal-level hierarchy an application actually needs:

```
sample gain (SampleParam.Gain, per sample)
  x voice gain (VoiceParam.Gain, per trigger/event — velocity, future per-cell level)
  -> this voice's own filter
  -> voice pan (VoiceParam.Pan / TriggerParams.pan + SampleParam.Pan)
  -> [sum into its bus's accumulator]
  -> track bus chain -> track bus's own BusGain (NodeParam.BusGain) -> bus pan (NodeParam.BusPan)
  -> [sum into the master accumulator]
  -> master bus chain -> master's own BusGain (NodeParam.BusGain) -> bus pan
  -> output
```

Read fresh off the `Sample` every render frame (`Voice::render`), not cached on the `Voice` —
so changing an already-loaded sample's gain is heard immediately on every voice currently
playing it, not just on voices triggered afterward. This matters for a host like a tracker
that wants a live-editable "instrument gain" control: no re-trigger, no per-voice bookkeeping,
just one call. A caller is never forced to fold the sample's own level into every `trigger()`
call by hand the way collapsing this into a single `gain` number would require — see
`VoiceParam`/`SampleParam`'s own doc comments in `native/src/params.h` for why the two are
deliberately separate id spaces, same reasoning as `NodeParam` vs. `VoiceParam` below.

### Cue regions and chop playback

A `Sample` may carry cue regions (`SampleRegion { start, end }` in frames; `setSampleRegions()` /
`loadSample({ regions })`; at most `kMaxSampleRegions`). They describe the audio and are
non-destructive: no buffers are copied. *Playback mode is not stored on the sample* — it is
per trigger. A trigger without `region` is chromatic/whole-sample exactly as before; `region: i`
(chop) resolves to that region's `[start, end)` when the voice starts (`Engine::trigger`, or when a
scheduled event fires), so `Voice` is unchanged and rate/pitch/loop/reverse/duration still apply
inside the region. A missing region plays nothing. Regions are edited only from the message
handler; the render thread only reads them, with no allocation. A re-committed sample starts with
no regions (like its gain). Note → region is `regionForNote(note, rootNote, regionCount)`
(consecutive notes = consecutive regions); clients decide the root note and the mode per track.
The region end is a hard stop (as for any `end`), so clients wanting a click-free tail use
`duration` + the release envelope.
Full reference: [`docs/sample-chop.md`](./docs/sample-chop.md).

Removing a sample (`removeSample`, or re-committing its id) is safe while voices are sounding.
The engine first stops every voice still reading that sample, then frees its PCM, so no render
can touch freed memory. Each stopped voice is reported through `endedVoices` on the next render
quantum, the same as a voice that finished normally, so the host can clear its own state for
it. A scheduled event whose sample has been removed is dropped when it comes due rather than
stealing a voice. (Before ECS-84, removing a sample while it was playing was a use-after-free.)

## How voices work

`native/src/voice.h` / `voice_manager.h`. A `Voice` is a playing instance of a sample:
position (fractional, for interpolated playback), rate, gain, loop/reverse/start/end, an
optional filter insert, and an ADSR amplitude envelope (see "Envelopes") whose release is
also the click-free fade for release/stop/steal.

`VoiceManager` owns a pool sized by `maxVoices` — a runtime policy set at
`AudioRuntime.create({ maxVoices })` (default 64), not a hardware fact and not hardcoded
anywhere in the engine. Triggering always succeeds: it reuses an idle voice, or steals
whichever active voice currently has the lowest amplitude estimate. A sample can have any
number of simultaneous voices; the engine has no concept of "channels" mapping 1:1 to
samples.

### Choke groups

`TriggerParams.chokeGroup` (a positive integer; omitted = none) puts a voice in a choke group.
Triggering a voice into group *g* makes `VoiceManager::trigger` first stop every active voice
(playing or in its release tail) of group *g* that plays into a **different bus**, then allocate
and start the new voice as usual. Voices on the same bus are one source and never choke each other,
which is how a host maps "a track" onto the engine without the engine knowing about tracks.

This is deliberately **not** voice stealing. Stealing is the allocator's answer to a full pool and
picks the quietest voice; choking is an explicit relationship between sources, happens whether or
not a voice is free, and touches only the group. The two share no code path: `maxVoices` and the
steal policy are unchanged.

The cut lands at the new voice's exact frame (`delayFrames` of the scheduled event), on the audio
thread, as part of the same trigger: `Voice::choke(atFrame)` starts a ~2 ms linear fade-out at that
frame (a hard cut would click; the sample's envelope release is ignored, so a long tail is cut
rather than decaying) and the voice then finishes through the normal path, so it is reported in
`voice-ended` and reclaimed like any other. Two triggers on the same frame resolve in queue order:
the later one wins. Instruments take the same `chokeGroup` per note (`InstrumentNoteParams`) and group membership spans
both kinds: a closed-hat instrument cuts an open-hat sample and vice versa. An instrument is cut by an
engine-side fade-out of a scratch render (`Engine::chokeInstruments`, `applyChokeFade`) followed by
`reset()`, because one-shot drums ignore `noteOff`. Reference: [`docs/choke-groups.md`](./docs/choke-groups.md).

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

## How armed capture works

`AudioRuntime.armCapture({ startTime, stopTime, bus? })` (`src/runtime/index.ts`) extends the
capture proof of concept (`native/src/capture.h`, "How samples are represented" doesn't cover
this, see `Capture` there) with the same idea `schedule()` already uses for note events: an
absolute engine time, not a JS timer, decides when something starts. `Capture::arm()` reserves
exactly `[startFrame, stopFrame)` up front; `Engine::process()` passes each render block's
absolute `blockStartFrame` into `Capture::appendBlock()`, which clips the copy to the overlap
with that armed window (silently doing nothing for blocks entirely before `startFrame`) and
sets `isFinished()` the instant `stopFrame` is reached — mid-block, on the exact sample, the
same way a scheduled voice trigger gets a sub-block `delayFrames` rather than being quantized
to the next 128-frame quantum.

Unlike the original `startCapture()`/`stopCapture()` pair (still present, unchanged, immediate/
manual — used by the demo app's `CapturePanel.tsx`), an armed capture has no explicit "stop"
command: `EngineProcessor.processUnsafe()` (`src/worklet/engine-processor.ts`) polls
`_webdsp_finished_capture_count()` every render block, the same way it already polls ended
voices, and auto-emits the existing `"capture-complete"` event the moment the engine finishes
one on its own. `AudioRuntime.armCapture()`'s returned `result` promise resolves from that
event via a `pendingArmedCaptures` map kept separate from `stopCapture()`'s own
`pendingCaptures`, so the two capture modes' promise contracts never interfere with each
other — and unlike `stopCapture()`, `armCapture()`'s resolved value includes the raw captured
PCM (`channelData`) alongside the registered `SampleMetadata`, since a caller aligning a
capture to an application-level boundary (a sequencer's pattern loop, say) is likely to also
want to persist the result as more than a live-session-only engine Sample.

`RuntimeCapabilities.maxCaptureSeconds` (sent in the worklet's `"ready"` event, mirroring
`kMaxCaptureSeconds`) lets a caller check a requested window's duration *before* calling
`armCapture()` — which throws synchronously rather than silently truncating an over-long
request. `Capture::arm()` itself also refuses an over-capacity window as a defensive backstop,
but that path should be unreachable as long as a caller checks `maxCaptureSeconds` first.

## How DSP is composed

`NodeParam.BusGain` (below) is a bus's own output-stage gain, applied *after* its DSP chain —
the other end of the signal-level hierarchy from `SampleParam.Gain` (per-sample, applied
before a voice ever reaches a bus's chain — see "Gain staging" above for the full chain and
why the three gain primitives stay separate rather than collapsing into one number).

`native/src/dsp_node.h`. There is no "effects" subsystem — only `DSPNode`, a tiny interface
(`process(channels, numChannels, numFrames)`, `setParam`, `reset`), and `DSPChain<N>`, a
fixed-capacity ordered list of them. `Voice` owns a one-node chain with a filter
(`dsp/biquad_filter.h`, RBJ cookbook, switchable low-pass/high-pass via
`VoiceParam.FilterMode`). `Bus` (`native/src/bus.h`) owns an `FxChain` (`native/src/fx_chain.h`, see "FX chains" below and `docs/fx-chain.md`) of effect instances. Historically a fixed six-node chain — the *same*
`BiquadFilter` class (so the master bus gets the identical low-pass/high-pass/cutoff/
resonance capability a voice has, just applied to the whole mix), followed by a chorus/flanger
(`dsp/chorus_flanger.h`, a sine-LFO-modulated fractional-delay tap — see
`docs/chorus-flanger-node.md`, ECS-18), a delay (`dsp/delay.h`, feedback delay line), a reverb
(`dsp/reverb.h`, a Dattorro plate reverb — see `docs/reverb-node.md`, ECS-15), a compressor
(`dsp/compressor.h`, a feedforward log-domain soft-knee peak compressor — see
`docs/compressor-node.md`, ECS-16), and finally a saturator (`dsp/saturation.h`, a memoryless
tanh waveshaper with drive/asymmetry/output-gain/mix — see `docs/saturation-node.md`, ECS-17),
which runs last so it adds harmonic warmth/glue to the fully-processed signal
(post-dynamics), not just the dry input.
`NodeParam` and `VoiceParam` are deliberately separate
numeric id spaces (see `params.h`) even though `Bus`'s filter and `Voice`'s filter are the
same class — `Bus::setParam` translates its own `NodeParam.FilterCutoff/FilterResonance/
FilterMode` ids into the `VoiceParam` ids `BiquadFilter::setParam` actually understands
before forwarding, rather than the two enums sharing values. Don't assume a `NodeParam` and
a `VoiceParam` with the same integer mean the same thing — they usually don't
(`NodeParam.BusGain == 3 == VoiceParam.FilterResonance`, for instance).

Every one of `Voice`'s and `Bus`'s nodes is an ordinary `DSPNode`; adding another kind
(EQ, ...) means writing one more class with that same three-method interface and
adding it to a chain — no change to `Voice`, `Bus`, or the chain mechanism itself, the same
way adding `ChorusFlanger` as `Bus`'s second node, `Reverb` as its fourth, `Compressor` as its
fifth, `Saturation` as its sixth, and (via the FX chain) `Phaser` and `Bitcrusher` required none. See "Adding a new master-bus module" just
below for exactly what that does and doesn't require.

The filter has a selectable 12 or 24 dB/oct slope (`VoiceParam.FilterSlope` /
`NodeParam.FilterSlope`, default 12 = the original single biquad); see `docs/filter-slope.md`.
The Pad and Fm instruments use the small shared `FilterModulation` helper (`dsp/filter_modulation.h`) for internal cutoff movement; see `docs/filter-modulation.md`. User-facing modulation of instrument *and effect* parameters is the shared LFO bank in `docs/lfo-modulation.md` / `docs/modulation.md` (automation is the application-side base value under it, optionally scheduled on the audio clock — `docs/scheduled-params.md`; the Utility FX is the automatable gain stage).

### FX chains

Each bus's DSP is an ordered `FxChain` of independent effect instances (`docs/fx-chain.md`):
any kind, any number of each, identified by an `EffectId`, with per-instance state,
parameters and enabled flag. Same mechanism on track buses and the master bus. Generic API:
`AudioRuntime.addEffect/removeEffect/moveEffect/setEffectEnabled/setEffectParameter/
getEffectChain`; native `webdsp_fx_*`. The old `setNodeParameter` NodeParam API remains and
addresses a per-kind "legacy instance" created on demand, in the original fixed order.
Structure changes run in the command handler, never inside `process()`, which walks resolved
pointers without allocating. A new effect = a `DSPNode` + one `FxKind` + one `makeNode` case + its `NodeParam` ids
(phaser and bitcrusher were added this way; see `docs/fx-chain.md`).

### Instruments

`native/src/instrument.h`, `instrument_registry.h`, `instruments/kick.h`; full write-up in
[`docs/instruments.md`](./docs/instruments.md) (generic), [`docs/kick-instrument.md`](./docs/kick-instrument.md) and [`docs/percussion-instruments.md`](./docs/percussion-instruments.md) (Snare, Clap, Tom, Hi-Hat), [`docs/bass-instrument.md`](./docs/bass-instrument.md) (sustained Bass; timed note-off via `duration`; kind identity `id`/`name`/`category`; note `pitch`), [`docs/pulse-instrument.md`](./docs/pulse-instrument.md) (Pulse: tempo-locked pump/repeat, transport); [`docs/pad-instrument.md`](./docs/pad-instrument.md) (Pad: drifting stereo ensemble, internal release-tail slots for overlapping notes); [`docs/fm-instrument.md`](./docs/fm-instrument.md) (Fm: two-operator PM bass/lead, stepped ratio, internal index envelope); [`docs/hypersaw-instrument.md`](./docs/hypersaw-instrument.md) (Hypersaw: 7-saw detuned stereo stack); [`docs/pwm-beast-instrument.md`](./docs/pwm-beast-instrument.md) (PWM Beast: pulse-width-modulated character synth with a fold stage and envelope-driven drive/filter); [`docs/percussion-synth.md`](./docs/percussion-synth.md) (Percussion Synth: two oscillators + noise, pitch sweep, FM, resonant filter, drive); [`docs/edge-instrument.md`](./docs/edge-instrument.md) (Edge: band-limited harmonic-count oscillator, signed pitch transient, dual-stage filter with routing); candidate techniques for new kinds are in [`docs/synth-dsp/`](./docs/synth-dsp/README.md). An `Instrument` is a synthesized
audio **source** — a sibling of the sample `Voice`, not a `DSPNode`. Sources render
(accumulating) into a bus chosen by `busId`; the existing track-FX → mixer → master-FX path then
applies unchanged. The instrument owns its sound design; the application sees only
`noteOn/noteOff`, `setParam`, a static parameter table (id/name/min/max/default/unit) and a
caller-chosen `instrumentId`. One object = one monophonic machine instance (retrigger restarts
it, like drum hardware); the engine has no second voice-allocation policy. Notes can be
immediate or scheduled on the same sample-accurate clock as samples (`EventQueue<T>`, the
generalised `Scheduler`; `cancelScheduled` clears both). Creating an instrument allocates and
runs on the command thread; `process()` never allocates. Adding a machine = a header under
`instruments/` + an `InstrumentKind` value + a case in `instrument_registry.h`. Native ABI:
`webdsp_instrument_*`, `webdsp_schedule_instrument_note`. TypeScript: `AudioRuntime.createInstrument /
removeInstrument / instrumentNoteOn / instrumentNoteOff / setInstrumentParameter /
getInstrumentParams`, carried by `create-instrument`, `instrument-note-on`, ... commands.
Parameter metadata is read from the engine at worklet start and delivered in the `ready` event
(`RuntimeCapabilities.instruments`), so JS never duplicates ranges.

### Transport

`Engine::scheduleTransport`, `Instrument::syncTransport`, `native/src/dsp/rhythm.h`; write-up in
[`docs/pulse-instrument.md`](./docs/pulse-instrument.md). The engine holds one **beat grid**
(anchor frame + frames per beat; default 120 BPM at frame 0). It knows nothing about patterns,
steps or tracks — only "a beat sits here, and beats are this long". Clients set it with
`AudioRuntime.setTempo(bpm, {time, anchorTime})` / `webdsp_set_transport(atFrame, anchorFrame, bpm)`;
a change goes through the same sample-accurate queue as notes and applies at block granularity.
Before every `render()` the engine offers the grid to the instrument via `syncTransport(frame,
anchor, framesPerBeat)` (default no-op), so a tempo-aware instrument re-derives its phase from
the absolute frame each call and stays aligned through silence, retriggers and tempo changes
without keeping a clock of its own. Subdivision (rate) is the instrument's business, not the
client's. Transport changes are state, so `cancelScheduled` does not remove them.

### Buses / mixing

`Engine` owns one `Bus` as the master bus (`MASTER_BUS`, busId 0) plus a fixed pool of
`kMaxTrackBuses` (`native/src/engine.h`, 64 since ECS-84 for 4 banks of 16 pads; effect instances, including
delay lines of about 0.8 MB each, are created on demand since the FX-chain change) additional ones, all pre-allocated at `init()` —
same shape as the voice pool, so handing one out is never a render-thread allocation. An
application gets one via `AudioRuntime.createBus()` (a synchronous main-thread counter bump
against `MAX_TRACK_BUSES` in `src/runtime/types.ts`, kept in sync with the native constant by
hand, not by a generated binding — no command crosses the worklet boundary to allocate one).

Routing and mixing, once per render quantum (`Engine::process`):

1. Every active voice renders into *its own bus's* accumulator buffer (selected by
   `Voice::busId()`, defaulting to master for `busId <= 0` or out of range) — not a single
   shared buffer, so one bus's voices never bleed into another's before that bus's own chain
   runs.
2. Each track bus runs its own filter+chorus/flanger+delay+reverb+compressor+saturation chain
   (`Bus::process`) over just its own accumulator, then that result is summed into the master
   accumulator — this sum *is* the mixer; there's no separate "Mixer" class. A bus that has
   never been fed a voice holds no DSP state, so `Engine::process` skips it outright and an
   unused bus costs nothing per block. Once a bus has been fed it runs every block, even when
   its output is silent. Silence doesn't prove the nodes' internal state has decayed (a
   compressor envelope can still be ringing while its output is zero), and skipping on a level
   threshold changed the next sound by up to 0.06 in testing (ECS-84).
3. The master bus then runs its own chain over the combined signal (its own direct voices,
   if any, plus every track bus's output) before that becomes the engine's output.

**Bus-to-bus routing.** A track bus sums into master by default, or into another track bus
when `NodeParam.BusOutput` is set on it (`setNodeParameter(bus, NodeParam.BusOutput, destBus)`;
0 = master). The destination's accumulator receives the sum of its own voices plus every routed
source's post-FX output, then runs its own chain — so `kick bus -> kick FX -> drum bus -> drum
compressor -> master`. `Engine::setBusOutput` rejects self-routes, unknown buses and anything
that would close a cycle (the previous route is kept), then rebuilds `busOrder_` (deepest
first, pre-reserved, outside `process()`) so sources always render before their destination.
A routed-into bus counts as fed, so it runs every block and its tails survive. No new ABI
export or command: it rides `set-bus-param`.

This is: `track voices -> track bus chain -> [sum] -> master bus chain -> master limiter -> output` (the limiter and per-bus peak metering: [`docs/master-limiter.md`](./docs/master-limiter.md)), matching
an application's likely mental model of "per-channel FX into a master FX chain" exactly,
using the same `Bus`/`DSPChain` machinery for both stages. A freshly-allocated track bus is
inert (`BiquadFilter` and `Compressor` both start `bypassed_`, `ChorusFlanger`/`Delay`/
`Reverb`/`Saturation` all start at `mix_ = 0`) — see "Adding a new master-bus module" below, unchanged by this:
`setNodeParameter`/`NodeParam` work identically on `MASTER_BUS` and on any `createBus()`
result.

### Adding a new master-bus module

Two different things can look like "add a module to the master bus," with very different
cost — check which one you actually need before assuming an engine change is required:

- **Exposing a DSP node that's already implemented.** `Bus` has carried a `ChorusFlanger`
  (`NodeParam.ChorusFlangerRate`/`ChorusFlangerDepth`/`ChorusFlangerDelay`/
  `ChorusFlangerFeedback`/`ChorusFlangerStereoPhase`/`ChorusFlangerMix` — see
  `docs/chorus-flanger-node.md`, ECS-18), a `Delay` (`NodeParam.DelayTime`/`DelayFeedback`/
  `DelayMix`), a `Reverb` (`NodeParam.ReverbDecay`/`ReverbDamping`/`ReverbMix` — see
  `docs/reverb-node.md`, ECS-15), a `Compressor` (`NodeParam.CompThreshold`/`CompRatio`/
  `CompAttack`/`CompRelease`/`CompKnee`/`CompMakeup` — see `docs/compressor-node.md`,
  ECS-16), and a `Saturation` (`NodeParam.SatDrive`/`SatAsymmetry`/`SatOutputGain`/`SatMix`
  — see `docs/saturation-node.md`, ECS-17) since before any of them got application UI — no
  client application has built UI for any of them yet, but they need **zero** engine changes
  to use today: an app can add a "Chorus/Flanger", "Delay", "Reverb", "Compressor", or
  "Saturation" module purely on the client side by calling
  `setNodeParameter(MASTER_BUS, NodeParam.ChorusFlangerMix, ...)` /
  `...DelayTime, ...)` / `...ReverbMix, ...)` / `...CompRatio, ...)` / `...SatDrive, ...)`
  etc. Always check `NodeParam`/`VoiceParam` (`src/runtime/types.ts`) before assuming a
  capability is missing — it might already be wired in and simply unused by any UI so far.
- **A genuinely new DSP algorithm** (EQ, ...). This *does* require an engine
  change: a new `DSPNode` subclass under `native/src/dsp/`, new `NodeParam`/`VoiceParam`
  values in both `params.h` and `types.ts` kept in sync (pinned by `test/paramIds.test.ts`),
  adding an `FxKind` value (kept in sync in `fx_chain.h` and `types.ts`'s `EffectKind`) and a `makeNode` case in `fx_chain.h`, and a
  WASM rebuild (`npm run build:wasm`, requires Emscripten — see "Getting started"). There's no
  scripting/plugin-loading mechanism for arbitrary
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
  fully offline. Documented here as the recommended path if the effects list keeps growing
  past what hand-writing comfortably supports; effect #3 (reverb), #4 (compressor), #5
  (saturation), and #6 (chorus/flanger) were all still small enough to hand-write (see next
  bullet), so Faust wasn't adopted for any of them.
- **RBJ Audio EQ Cookbook** biquad formulae — adopted directly (`dsp/biquad_filter.h`): this
  is the standard, well-documented technique, small enough to own and unit-test, and not
  meaningfully improved on by a general-purpose library for a single lowpass filter. Reverb
  (`dsp/reverb.h`, `docs/reverb-node.md`), the compressor (`dsp/compressor.h`,
  `docs/compressor-node.md`), the saturator (`dsp/saturation.h`,
  `docs/saturation-node.md`), and the chorus/flanger (`dsp/chorus_flanger.h`,
  `docs/chorus-flanger-node.md`) followed the same pattern one level up: each is a
  fully-specified published design (Dattorro's plate reverb; Giannoulis/Massberg/Reiss's
  feedforward log-domain compressor tutorial; a single memoryless tanh waveshaper for
  saturation; Dattorro's own companion paper on delay-line modulation for chorus/flanger)
  re-derived directly from its source rather than pulled from a library or ported from a
  third-party implementation — see `docs/effects-algorithm-survey.md` (ECS-14) for why these
  specific candidates were picked over their alternatives (Freeverb/FDN/Schroeder/Moorer for reverb;
  feedback/RMS topologies for the compressor; a general-purpose distortion framework with
  multiple selectable curves and built-in oversampling for saturation).

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
   The instrument's amplitude envelope (`setSampleEnvelope()`, see "Envelopes" and
   `docs/adsr-envelope.md`) shapes each note; `duration` is its note-off.
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

### A note on `AudioWorkletGlobalScope` API gaps (two real bugs found during verification)

`AudioWorkletGlobalScope` is not a full `Window`/`Worker` scope — it only gets a subset of
`WindowOrWorkerGlobalScope`, and which APIs are missing isn't consistently documented or
type-checked (the ambient `.d.ts` for the worklet scope doesn't necessarily declare what it
omits, so TypeScript has no way to catch a reference to a missing one). Two different globals
have now bitten `engine-processor.ts` this way; both were caught only by manual browser
verification, not by the type checker:

- **`performance`** — `engine-processor.ts` originally measured render time via
  `performance.now()` for a `cpuLoad` diagnostic. This throws (`performance` isn't part of the
  mixin worklets receive), and an uncaught exception in `process()` causes Chrome to silently
  stop calling it forever — sample loading + triggering never showed up in diagnostics again.
  Fixed by removing the measurement rather than guessing at a workaround, and by wrapping both
  `process()` and the command handler in `try`/`catch` that reports failures as `"error"`
  `WorkletEvent`s instead of letting an exception anywhere disable the node permanently — a
  malformed command should degrade gracefully, not take down the whole audio graph. `cpuLoad`
  and `underruns` were removed from `RuntimeDiagnostics` entirely rather than left as fields
  that always read a misleading `0`; see "Intentionally deferred" for how they could be added
  back.
- **`TextDecoder`** — added alongside the synthesized-instrument framework (see "Instruments"
  above), `readInstrumentMetadata()` used `new TextDecoder()` to decode each param's
  name/unit string out of WASM memory. `TextDecoder` isn't part of the worklet mixin either, so
  this threw on *every* `AudioRuntime.create()` — not just when an instrument was actually
  used — because it ran inside the module-ready callback, before the `"ready"` `WorkletEvent`
  (the one the `try`/`catch` fix above doesn't wrap, since it isn't `process()` or the command
  handler) ever posted. The practical effect for a consuming app: the whole runtime silently
  never finished initializing, so *no* audio worked — sample playback included, not only
  instruments. Fixed by replacing `TextDecoder` with a plain byte-for-char decode loop, exact
  for the short ASCII identifiers these tables actually contain (see `readInstrumentMetadata()`
  in `src/worklet/engine-processor.ts`). The broader lesson these two share: don't reach for a
  browser-global convenience API inside `engine-processor.ts` without checking it against a real
  `AudioWorkletGlobalScope` first — assume it's missing until verified in an actual browser, the
  same "can't fake it, so verify it for real" rule this doc's own testing section applies to
  everything realtime.

---

## Automated tests for the non-realtime parts

- **`native/test/*_test.cpp`** — pure C++ logic (`resampler.h`'s interpolation, `pan_test.cpp`'s pan law/smoothing on rendered buffers,
  `scheduler.h`'s ordering/boundary behavior, `adsr_test.cpp`'s envelope and voice lifecycle,
  and one file per DSP node), compiled natively with the system compiler
  (`clang++`, no `emcc` needed) for fast iteration. Run via `native/test/run.sh`.
- **`native/test/smoke.mjs`** — exercises the actual compiled WASM ABI under Node
  (`native/build/engine.node.mjs`): sample commit, polyphony, sample-accurate mid-block
  scheduling, release/reclaim, capture. This is the one test that touches the real compiled
  artifact rather than a native recompile of the same source, so it's what would catch an
  ABI-level regression (wrong argument order, a renamed export).
- **`test/*.test.ts`** (vitest) — `paramIds.test.ts` pins the `VoiceParam`/`NodeParam`/`SampleParam`
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

- **More instrument machines, polyphonic instrument instances, instrument-level FX.**
- **Dynamic DSP chains on voices.** Bus FX chains are dynamic (see "FX chains"). `Voice`'s
  one-node `DSPChain` is still fixed; per-voice effect stacks are not built.
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
- **Per-voice send buses, per-voice envelope overrides, pitch/filter envelopes,
  non-linear envelope curves.** Not requested. Voice/bus pan is implemented (see "Panning"). The amplitude ADSR is implemented (see
  "Envelopes"); it is per-instrument (`Sample`), and `AdsrEnvelope` is reusable for the others.
- **Capture duration cap (30s, `native/src/capture.h`, queryable as
  `RuntimeCapabilities.maxCaptureSeconds`).** Capacity is reserved up front so the realtime
  `appendBlock()` path never allocates; a streaming/backed-by-disk capture would remove the
  cap without changing the `Capture` class's public shape.
