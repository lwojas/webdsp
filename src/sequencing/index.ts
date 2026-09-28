// Public entry for the "webdsp/sequencing" subpath export — the example event-compilation
// helpers (project brief section 9), reusable by a real sequencer app, but never part of
// the core runtime's own public surface. See ARCHITECTURE.md, "How future sequencers can
// integrate".
export * from "./pattern";
export * from "./lookaheadPlayer";
