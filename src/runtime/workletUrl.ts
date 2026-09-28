// Deliberately its own module, not re-exported from ./index.ts or imported by anything in
// src/app: the `new URL('./x', import.meta.url)` pattern below is a well-supported
// bundler-friendly asset reference (Vite, webpack 5+, native ESM all resolve it correctly
// relative to *this file's own* published location), but it only resolves correctly when
// this file actually ships with a sibling engine-processor.js next to it — true for the
// published package (tsup bundles this to lib/worklet-url.js, right next to
// lib/engine-processor.js) but not true for this repo's own src/ tree, where no such
// sibling exists. Keeping it out of index.ts's module graph means the demo app's own Vite
// build (which does transitively import ./index.ts) never has to resolve this reference.
// See ARCHITECTURE.md, "Using this as a package".
export const defaultWorkletUrl = new URL("./engine-processor.js", import.meta.url);
