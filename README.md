# webdsp

A standalone, application-agnostic browser audio engine — WASM (C++/Emscripten) DSP core
running inside an AudioWorklet, with a small deliberate TypeScript API (`AudioRuntime`) in
front of it. Built as the foundation for a future sampler/tracker/sequencer app; the engine
itself has no idea what a "pad" or "pattern" is. A small React test harness is included as a
disposable client of the API, not part of the engine.

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the full design: runtime boundaries, how
samples/voices/scheduling/DSP work, and how to use this package from another project.

## Quick start

```bash
npm install       # also builds lib/ from src/ via tsup (see ARCHITECTURE.md)
npm run dev        # demo app at http://localhost:5173

npm run test:all   # native tests + wasm ABI smoke test + vitest
```

## Using it as a dependency

```ts
import { AudioRuntime } from "webdsp";
import { defaultWorkletUrl } from "webdsp/worklet-url";

const runtime = await AudioRuntime.create({ workletModuleUrl: defaultWorkletUrl });
await runtime.resume(); // must follow a user gesture
const sample = await runtime.loadSample(arrayBuffer, { name: "kick" });
runtime.trigger({ sampleId: sample.id, gain: 1.0 });

// Optional per-instrument ADSR amplitude envelope (seconds / 0..1):
runtime.setSampleEnvelope(sample.id, { attack: 0.005, decay: 0.15, sustain: 0.7, release: 0.25 });
```

Stackable FX chains — any number of effects per bus, including several of the same kind:

```ts
const bus = runtime.createBus();            // or MASTER_BUS
const lo = runtime.addEffect(bus, EffectKind.Filter);
const crush = runtime.addEffect(bus, EffectKind.Bitcrusher);
runtime.setEffectParameter(bus, lo, NodeParam.FilterCutoff, 3000);
runtime.moveEffect(bus, crush, 0);
```

Reference: [`docs/fx-chain.md`](./docs/fx-chain.md).

Envelope reference and consumer guide: [`docs/adsr-envelope.md`](./docs/adsr-envelope.md).

Choke groups (open/closed hi-hat style cut-offs): [`docs/choke-groups.md`](./docs/choke-groups.md).

Synthesized instruments: [`docs/instruments.md`](./docs/instruments.md) (framework and client integration); Kick: [`docs/kick-instrument.md`](./docs/kick-instrument.md); Snare/Clap/Tom/Hi-Hat: [`docs/percussion-instruments.md`](./docs/percussion-instruments.md). Ideas for new instruments (techniques, concepts, ranking): [`docs/synth-dsp/`](./docs/synth-dsp/README.md).

Details and caveats (worklet URL resolution across bundlers, the `webdsp/sequencing`
subpath, etc.) are in [`ARCHITECTURE.md`](./ARCHITECTURE.md#using-this-as-a-package).

## In a native plugin

The engine (`native/src/engine.h`) also runs inside a VST3 plugin ([webvst](https://github.com/lwojas/webvst)), hosting an
unmodified client in a WebView. webdsp offers a few host seams for that (host transport, host state, host files, a protocol
version), all inert in a browser: see `docs/native-host.md` for the contract a change here must keep (additive protocol) and
`ARCHITECTURE.md`, "Hosting in a native plugin".
