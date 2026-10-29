# MIDI routing (playing incoming MIDI without the page in the path)

A host that receives MIDI itself (a plugin: the DAW sends it notes) can hand it to the page as Web MIDI, but then the page must turn each
note into an engine call and send that back: a round trip through a WebView (about 10-30 ms plus a block), and nothing plays while the
plugin's window is closed. A **route** is the page telling the host, ahead of time, what a key should play. The host then plays incoming
notes itself, sample-accurately on its audio thread, with no page involved.

```ts
if (runtime.getCapabilities().midiRouting) {
  runtime.setMidiRoute({ kind: "instrument", id: bassId, unityNote: 45, holdUntilKeyUp: true });   // keys play this synth
  // ...and do NOT also play incoming MIDI notes from the page: the host does
} else {
  // a browser (or an older host): play notes from the page, as before
}
runtime.setMidiRoute(null);   // clear
```

- **`capabilities.midiRouting`** is true only when the host can route (false in a browser, and for a host older than protocol 2).
  `setMidiRoute` is a no-op without it, so calling it is always safe; the capability decides who plays, so a note is never played twice.
- **The route** (`MidiRoute`, `src/runtime/midiRoute.ts`): a sample (`id` = SampleId) or an instrument (`id` = InstrumentId); `unityNote` (the
  key that plays it at its own pitch; others transpose by the difference); for samples `bus`, `chop` (the key picks a cue region), `loop`, `mono`;
  `fixedPitch` (instruments: every key plays at the instrument's own pitch instead of transposing, for a drum pad; protocol 4); `holdUntilKeyUp` (a looping sample's voice and a synth's note end on key-up, otherwise a note rings out like a pad hit); `gain` (0..1, e.g. a
  track's volume); `chokeGroup`; `channel` (omit for any). Velocity is the note's level.
- **Re-declare when what a key should play changes** (a different track or instrument selected, a sample reassigned, its mode changed): an
  unchanged route should not be re-sent (`sameMidiRoute`). The route holds until replaced; it is dropped when the engine is reset (a page
  reload), so a page declares it again on load.
- **The page still receives the MIDI** (note entry in a tracker, CCs, learn), so it can keep reacting to it; it just must not make the
  sound. Not covered: pitch bend, CCs and sustain (they are delivered to the page only).
- Unusable routes (an unknown target, non-finite numbers) are treated as `null` (`normalizeMidiRoute`).
- Protocol: command `set-midi-route`, `ready.midiRouting`; added in `HOST_PROTOCOL_VERSION` 2 (docs/native-host.md).
