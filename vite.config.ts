import { fileURLToPath, URL } from "node:url";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The AudioWorkletProcessor (src/worklet/engine-processor.ts) is loaded via
// audioContext.audioWorklet.addModule(url) — a runtime fetch, not a JS `import`, so Vite's
// built-in `new Worker(...)` bundling magic doesn't apply to it. It's registered as a
// second Rollup entry with a fixed (unhashed) output name so production code can reference
// it by a known path instead of needing Vite's asset-URL resolution to see through
// addModule(). In dev, Vite's dev server transforms any matching .ts request on the fly, so
// the raw source path is used directly — see src/runtime/index.ts, AudioRuntime.create().
export default defineConfig({
  plugins: [react()],
  server: {
    headers: {
      "Cross-Origin-Opener-Policy": "same-origin",
      "Cross-Origin-Embedder-Policy": "require-corp",
    },
  },
  build: {
    rollupOptions: {
      input: {
        main: fileURLToPath(new URL("./index.html", import.meta.url)),
        "engine-processor": fileURLToPath(
          new URL("./src/worklet/engine-processor.ts", import.meta.url),
        ),
      },
      output: {
        entryFileNames: (chunk) =>
          chunk.name === "engine-processor" ? "engine-processor.js" : "assets/[name]-[hash].js",
      },
    },
  },
  worker: {
    format: "es",
  },
  test: {
    // Without this, vitest's default glob also picks up emsdk's own internal test
    // fixtures under .emsdk/ (that directory is gitignored, checked out by native/build.sh
    // — see ARCHITECTURE.md — and is not part of this project's source).
    include: ["test/**/*.test.ts"],
  },
});
