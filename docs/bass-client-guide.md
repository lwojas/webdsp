# Bass instrument: guide for consuming clients

For applications (webtracker, other clients) that play the Bass. Implementation detail and
parameter mapping: [`bass-instrument.md`](./bass-instrument.md). Why it is built this way:
[`bass-instrument-proposal.md`](./bass-instrument-proposal.md).

## What changed in webdsp

1. **New instrument kind** `InstrumentKind.Bass` (id `"bass"`, category `"bass"`), 11 controls in
   `BassParam` (`Legato`, `Slide`, `FilterMod` are the latest). It is consumed exactly like the drums.
2. **Timed note-off for instruments.** `InstrumentNoteParams.duration` (seconds) is new and
   optional. Existing code that does not pass it behaves as before.
3. **ABI change** (only matters if you call the WASM exports directly):
   `webdsp_instrument_note_on(id, velocity, pan, pitch, durationFrames)` and
   `webdsp_schedule_instrument_note(atFrame, id, velocity, pan, pitch, durationFrames)`, with
   `-1` meaning "no duration". Use the rebuilt `lib/`; do not mix old and new worklet/WASM.

No client refactor is required: nothing existing changes meaning. Adopting the bass is additive.

## Using it

```ts
import { InstrumentKind, BassParam } from "webdsp";

const params = runtime.getInstrumentParams(InstrumentKind.Bass);   // ranges/defaults from the engine
const bass = runtime.createInstrument(InstrumentKind.Bass, { bus });

runtime.setInstrumentParameter(bass, BassParam.Cutoff, 300);
runtime.setInstrumentParameter(bass, BassParam.Bite, 0.6);

// A sequenced note: pitch = semitones above `tune` (default 55 Hz = A1), duration = note length.
runtime.instrumentNoteOn(bass, { time, velocity: 0.8, pitch: 7, duration: stepSeconds * 0.9 });
```

| Control | Unit / range | What a UI should call it |
|---|---|---|
| `Tune` | 27.5–110 Hz | Root |
| `Body` | 0–1 | Body / waveform (sub-ish to rich) |
| `Bite` | 0–1 | Bite / dynamics (onset drive; velocity -> timbre) |
| `FilterMod` | 0–1 | Filter Mod (how far the note's envelope moves the cutoff; 0 = static filter) |
| `Cutoff` | 40–4000 Hz | Cutoff |
| `Resonance` | 0–1 | Resonance |
| `Attack`, `Decay`, `Release` | seconds | A / D / R |
| `Sustain` | 0–1 | S (the held level) |
| `Legato` | 0/1 | Legato (mono-glide on overlapping notes); default off |
| `Slide` | 0–0.5 s | Slide time (only between legato notes) |

Build knobs from `getInstrumentParams`, not from this table; ranges may be retuned.

## Rules of thumb

- **Always pass `duration` for bass notes** when driven from a sequencer. Without it the note is
  held until the next `instrumentNoteOn` (which retriggers) or `instrumentNoteOff`. Duration is
  counted on the audio thread from the frame the note starts, so it is sample-accurate; do not
  time note-offs with JS timers.
- **Do not pass `duration` for drums**; it would choke the one-shot with a short release.
- **Mono per instance.** A new note while one sounds is a continuous retrigger (no click, pitch
  jumps, envelopes re-attack from their current level). For chords or layered lines, create
  several instances.
- **`pitch` is relative to `tune`.** To play MIDI note `n`, send `pitch = n - tuneMidi`
  (A1 = MIDI 33 at the default `tune` 55 Hz). Keep `Tune` for the instrument's root, not per note.
- **Velocity changes timbre as well as level** in proportion to `Bite`. `Bite = 0` makes velocity
  a pure level. Output is bounded for any velocity.
- **Live edits:** all controls can be changed any time. `Release` is read at note-off, so it
  applies to notes already held; others take effect on the running note smoothly.
- **Cancel / cleanup:** `cancelScheduled()` drops queued notes but lets already-started notes
  finish their timed release. `removeInstrument` drops everything for that instance.
- **Legato / slide:** set `Legato` on and give notes `duration >= step` (or no gap) so the next
  note lands while the previous is held: same voice, no re-attack, pitch moves (glides over
  `Slide` seconds). A gap (`duration < step`, e.g. 0.9 x step) re-attacks normally and never
  glides, so one `Legato`+`Slide` setting gives 303-style lines where you choose per note by
  duration. The held note's velocity is kept through a legato transition. Last note wins; there is
  no held-note stack.
- **Not available yet:** per-note accent/slide flags, presets, unison/chords within one instance.

## Upgrade checklist

1. Bump the webdsp dependency to the version containing this change.
2. If you call exports directly, add the trailing `durationFrames` argument (`-1` for none).
3. Pass `duration` from your sequencer for bass notes.
4. Add `InstrumentKind.Bass` to any hardcoded instrument lists / persisted-project mappings
   (refer to the kind by its stable `id` `"bass"`, never its display name).
