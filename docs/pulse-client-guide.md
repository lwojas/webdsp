# Pulse instrument: guide for consuming clients

For applications (webtracker, other clients) that play Pulse. What it is and how it sounds:
[`pulse-instrument.md`](./pulse-instrument.md). Why it is built this way:
[`pulse-instrument-proposal.md`](./pulse-instrument-proposal.md). Same conventions as
[`bass-client-guide.md`](./bass-client-guide.md).

## What changed in webdsp

1. **New instrument kind** `InstrumentKind.Pulse` (id `"pulse"`, category `"synth"`), 9 controls in
   `PulseParam`. It is created, played and edited exactly like the Bass and the drums.
2. **A tempo/beat grid in the engine.** Pulse's two signature behaviours (pump and repeat) lock to
   it, so a client that plays Pulse must tell the engine its tempo: `runtime.setTempo(...)`.
   This is the only new thing a client has to learn; see "Telling the engine your tempo".
3. **ABI** (only matters if you call the WASM exports directly): one new export,
   `webdsp_set_transport(atFrame, anchorFrame, bpm)`. Nothing existing changes signature. Use the
   rebuilt `lib/`; do not mix old and new worklet/WASM.

Adopting Pulse is additive: no existing call changes meaning, and the kind list reaches the UI
through the engine's capabilities (`getInstrumentParams`), not a hardcoded table.

## Using it

```ts
import { InstrumentKind, PulseParam } from "webdsp";

const pulse = runtime.createInstrument(InstrumentKind.Pulse, { bus });
runtime.getInstrumentParams(InstrumentKind.Pulse);   // ranges/defaults from the engine

runtime.setInstrumentParameter(pulse, PulseParam.Cutoff, 800);
runtime.setInstrumentParameter(pulse, PulseParam.Pump, 0.7);
runtime.setInstrumentParameter(pulse, PulseParam.Rate, 3);       // 1/8

// Before scheduling notes against a grid (see below):
runtime.setTempo(bpm, { anchorTime: step0Time });

// A sequenced note: pitch = semitones above `tune`, duration = note length.
runtime.instrumentNoteOn(pulse, { time, velocity: 0.8, pitch: 7, duration: stepSeconds * 0.9 });
```

| Control | Unit / range | What a UI should call it |
|---|---|---|
| `Tune` | 27.5-440 Hz | Root |
| `Cutoff` | 60-6000 Hz | Tone / cutoff |
| `Resonance` | 0-1 | Resonance |
| `Pump` | 0-1 | Pump (duck depth) |
| `Repeat` | 0-1 | Repeat (drive/grit per cycle) |
| `FilterMod` | 0-1 | Filter Mod (how far each repeat opens the filter; 0 = static) |
| `Rate` | 0-5, stepped | Rate: 1/1, 1/2, 1/4, 1/8, 1/16, 1/32 |
| `Diffuse` | 0-1 | Diffuse / spread |
| `Attack`, `Release` | seconds | A / R |

Build knobs from `getInstrumentParams`, not from this table; ranges and defaults may be retuned.
`Rate` is a discrete step (non-integers round): render it as a stepped control with the labels
above, not a continuous knob.

## Telling the engine your tempo

Pump and repeat follow the **beat grid, not the note**. The engine keeps one grid: a tempo and the
absolute time of one beat on it. Pulse derives its rhythmic phase from it, so it keeps ducking on
the beat through held notes, retriggers and silence, and several Pulse instances pump together.

```ts
runtime.setTempo(bpm, { time?, anchorTime? });
```

- `bpm`: quarter notes per minute. Ignored if not finite and > 0.
- `anchorTime`: engine time (same domain as `getCurrentTime()` and note `time`) of a beat. The
  grid then has beats at `anchorTime + n * 60 / bpm`. Defaults to `time`, or now.
- `time`: when the change takes effect. Omitted = as soon as possible (next audio block). Pass the
  time of the step where a tempo change lands, so it arrives together with the notes scheduled
  for that step.

The engine does not know about steps or patterns. Translate yours into "BPM and where a beat is":

| Your sequencer | `bpm` | `anchorTime` |
|---|---|---|
| Steps of `stepSeconds`, 4 steps per beat (16ths) | `60 / (stepSeconds * 4)` | Time of step 0 |
| Steps of `stepSeconds`, 2 steps per beat (8ths) | `60 / (stepSeconds * 2)` | Time of step 0 |
| Explicit BPM | the BPM | Time of the pattern/song start |

### When to call it

| Event | What to do |
|---|---|
| Playback start (or restart) | `setTempo(bpm, { anchorTime: startTime })` **before** scheduling the first notes. Message order is preserved, so calling it first is enough. |
| Stop, then start again | Call it again with the new start time. The grid does not follow your transport; if the new start is not a whole number of beats after the old anchor, pump will be off-beat. |
| Pause/resume | Treat as a restart: re-anchor to the time the pattern's step 0 would now fall on (resume time minus elapsed pattern time). |
| Tempo change during playback | `setTempo(newBpm, { time: changeTime, anchorTime: changeTime })` with `changeTime` the time of a **beat boundary**. A change mid-beat makes the next hit land early or late. |
| Tempo automation (many changes) | One call per change at the beat where it takes effect. |
| Project load | Same as playback start. |

### Things that follow from "grid, not note"

- **Not calling `setTempo` is not an error**, but the default grid is 120 BPM anchored at frame 0,
  so pump will be steady yet unrelated to your steps. If Pulse "feels off", this is the first
  thing to check.
- **One grid per engine.** Every tempo-aware instrument shares it. Two parts at different tempos
  in one engine are not supported.
- **The grid is straight.** Swing or humanised step times do not move the pump; notes will sit
  off the grid while the pump stays on it. That is usually what you want (a ducked sidechain feel).
- **Pattern length vs rate.** If a loop is not a whole number of `Rate` periods long (a 3/4
  pattern at 1/1, or 1/1 over a 2-beat loop), the pump phase at the loop point will not repeat.
  Keep loops a whole number of bars for slow rates.
- **Notes on a beat start inside a duck.** That is the intended sidechain sound; it is the main
  thing to judge by ear. If transients feel dull, tell webdsp: the fix is an internal lead, not a
  client change.
- **`cancelScheduled()` does not touch the grid.** Tempo is state, not an event. A future-dated
  `setTempo` queued before a stop will still apply; re-anchor on restart.
- **Timing resolution.** A change applies at audio-block granularity (about 2.7 ms); notes remain
  sample-accurate.

## Rules of thumb

- **Always pass `duration`** from a sequencer (Pulse is a sustained kind, like Bass). Without it
  a note is held until the next `instrumentNoteOn` (continuous retrigger) or
  `instrumentNoteOff`. Duration is counted on the audio thread; do not time note-offs with JS.
- **Mono per instance.** A new note while one sounds retriggers continuously. For chords create
  several instances: they pump identically because they share the grid.
- **`pitch` is relative to `tune`.** At the default `tune` 110 Hz (A2 = MIDI 45), play MIDI note
  `n` with `pitch = n - 45`. If you change `Tune`, adjust the root you subtract.
- **Velocity is musical:** it scales level and also how far each repeat opens the filter
  (half to full), so per-step velocities give per-step rhythmic accents.
- **Live edits:** every control can change at any time, including mid-note. `Release` is read at
  note-off; `Rate` and `Pump` changes are smoothed (about 1.5 ms) so there are no clicks.
- **`Pump = 0`, `Repeat = 0` and `FilterMod = 0`** is a plain ADSR synth, independent of tempo. (`Repeat = 0` alone still opens the filter on the grid if `FilterMod > 0`.)
- **Cleanup:** `cancelScheduled()` drops queued notes but lets started notes release;
  `removeInstrument` drops the instance.
- **Not available:** glide, per-note accent flags, separate pump/repeat rates, dotted/triplet
  rates, tempo maps inside the engine.

## Wiring it into a lookahead scheduler

The runtime already supports this shape (see `src/sequencing/lookaheadPlayer.ts`): the JS timer
tops up a window of notes, but time comes from absolute engine timestamps.

```ts
function startPlayback(startAt: number) {
  runtime.setTempo(bpm, { anchorTime: startAt });   // once, before the first top-up
  player.start(startAt);                            // schedules notes from startAt
}

function changeTempo(newBpm: number, atStepIndex: number) {
  const t = stepTime(atStepIndex);                  // a beat boundary
  runtime.setTempo(newBpm, { time: t, anchorTime: t });
  // ...and compute subsequent step times with the new step duration.
}
```

## Upgrade checklist

1. Bump the webdsp dependency to the version containing this change (`399cf4b` or later).
2. If you call exports directly, add `_webdsp_set_transport`; nothing else changes.
3. Call `setTempo` at playback start, restart, resume and on tempo changes (table above).
4. Pass `duration` from your sequencer for Pulse notes.
5. Register the kind by its stable `id` `"pulse"` in any persisted-project mapping, never by its
   display name; add a stepped `Rate` control to your UI.
6. Add Pulse to any hardcoded instrument lists (`InstrumentKind.Pulse = 6`).

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Pump is steady but unrelated to the steps | `setTempo` never called, or anchored to the wrong time |
| Pump drifts out of time after stop/start | Not re-anchored on restart |
| Pump jumps after a tempo change | Change not on a beat boundary, or `anchorTime` not equal to the change time |
| Everything is quiet or "choked" | Pump too high on short notes at a slow rate; lower `Pump` or raise `Rate` |
| Too sharp or whistling | `Resonance` high with `Repeat` high; resonance 0.15-0.3 is a good range |
| Note never ends | No `duration` passed and no `instrumentNoteOff` |
