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
```

Details and caveats (worklet URL resolution across bundlers, the `webdsp/sequencing`
subpath, etc.) are in [`ARCHITECTURE.md`](./ARCHITECTURE.md#using-this-as-a-package).
