import { defineConfig } from "tsup";

// Builds the *library* (the runtime + worklet + sequencing helpers) into lib/, separate
// from Vite's demo-app build (src/app -> dist/). See ARCHITECTURE.md, "Using this as a
// package".
//
// "engine-processor" must be bundled fully self-contained: it's loaded via
// audioContext.audioWorklet.addModule(url), a runtime fetch, never a JS `import` — so it
// can't rely on any other lib/ file being resolvable relative to it at that point.
export default defineConfig({
  entry: {
    index: "src/runtime/index.ts",
    "worklet-url": "src/runtime/workletUrl.ts",
    sequencing: "src/sequencing/index.ts",
    "engine-processor": "src/worklet/engine-processor.ts",
  },
  format: ["esm"],
  dts: true,
  outDir: "lib",
  clean: true,
  target: "es2020",
  platform: "browser",
  splitting: false,
  sourcemap: true,
});
