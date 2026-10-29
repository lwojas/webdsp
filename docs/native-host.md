# Native hosts (plugins) and the protocol contract

Besides the browser AudioWorklet, the runtime can be driven by a native host: a plugin (the `webvst` repo) runs this
engine (`native/src/engine.h`, header-only) on a host's audio thread and speaks the same command/event protocol
(`src/runtime/commandProtocol.ts`: `HostCommand` in, `WorkletEvent` out) to the page. The host injects a small shim
that stands in for `AudioContext`/`AudioWorkletNode`, so applications are unchanged. Related: `docs/host-transport.md`
(the host's clock).

## What a change to webdsp must keep true

- **The protocol is additive.** Add commands and events; do not change or remove one. A plugin is built against a
  specific webdsp commit and updated separately from the web app that loads in it, so the app may be newer than the
  plugin.
- **When an addition is one an older host would not understand, bump `HOST_PROTOCOL_VERSION`.** A host reports the
  version it implements in its `ready` event (`protocolVersion`); a runtime that finds a lower one logs a warning
  (`hostProtocolWarning`) and exposes `capabilities.hostProtocolVersion`. A browser reports none.
- **Engine parameters, instrument kinds and effect kinds are self-describing** (`webdsp_instrument_param_*`,
  `webdsp_fx_param_*`), so new ones reach a plugin without code there, but a new *command* needs a case in the
  plugin's dispatcher. The plugin's tests check that it handles every `HostCommand` type in this file.
- **`engine.h` stays free of platform and emscripten dependencies**, and keeps its audio-thread rules
  (`docs/realtime-audit.md`): the plugin counts allocations per command on the audio thread.

## Protocol versions

1. The command/event set up to host files and host state.
2. `set-midi-route` and `ready.midiRouting` (MIDI routing, `docs/midi-routing.md`).
3. `set-sequence` and `ready.nativeSequence` (native sequence, `docs/native-sequence.md`).
4. Bus targets in scheduled params and sequence param events (`{ bus }`, `docs/scheduled-params.md`): the mixer (gain, pan, send amount, main gain) on the audio clock.
