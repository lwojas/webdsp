# Scheduled parameter events

Timestamped writes of a parameter's **base value**, with an optional linear ramp, on the audio clock:
the parameter counterpart of `schedule()` / `instrumentNoteOn({time})`. Built for automation
(a sequencer lane, a tracker's cell FX), where applying a change on the application's own timer
lands it up to a lookahead early and cannot ride a note-on.

```ts
runtime.scheduleParams([
  { time, target: { effect: { bus, effect } } | { instrument } | { sample }, param, value,
    from?, ramp? },
]);
```

* `time`: audio-clock seconds, as `ScheduledEvent.time`. A time in the past applies on the next block.
* `value`: the value reached: at `time` for a step, at `time + ramp` for a ramp.
* `ramp` (seconds, default 0): a linear ramp in the parameter's own units.
* `from` (optional): the ramp's start value. Omitted, the ramp starts from the parameter's current base.
  With `from`, one event is a self-contained lane segment: independent of what the engine last did
  (a seek, a cancel, a loop restart). Applications that play breakpoint lanes should send
  `{time: tᵢ, from: vᵢ, value: vᵢ₊₁, ramp: tᵢ₊₁ - tᵢ}` per segment and a plain step for a jump.
* `param` is the id the matching immediate setter takes: a `NodeParam` (effect), the synth's param id
  (instrument), a `SampleParam` (sample).

Not covered: bus parameters (`BusGain`/`BusPan`: the mixer is deliberately not automated) and
per-voice parameters.

## Semantics

| Rule | Behaviour |
|---|---|
| What is written | The **base**, through the same path as `setEffectParameter` / `setInstrumentParameter` / `setSampleParameter`. Engine LFOs keep riding on it: effective = base + LFOs throughout a ramp (tested). |
| When it lands | At the start of the render block containing `time` (below), **before** that block's notes. |
| Ramp shape | Linear in value space. |
| One ramp per parameter | A new event for a (target, param) replaces that parameter's running ramp. |
| Immediate setters | A direct write to a parameter cancels its running ramp (last writer wins), so an application's "restore the base on stop" is final. Removing the effect / instrument / sample cancels its ramps. |
| `cancelScheduled(from)` | Drops queued param events at or after `from`, like notes. A ramp that has **already started** keeps running until a direct write or its end: a stop is `cancelScheduled()` followed by writing the base back, which ends it. |
| Unknown / invalid | An event for a target that does not exist, or with a non-finite value, is dropped. A bad `from` falls back to the current base. |
| Table full | At most 64 ramps run at once (`kMaxParamRamps`). Past that, a new ramp is not run: its **end** value is applied directly and the drop is counted (`droppedParamRamps`). |

## Why linear

A lane is stored as breakpoints with linear interpolation (webseq's `AutomationLane`), and its view
draws exactly those breakpoints (`curveVertices`): the drawn line, the stored data and the engine's
ramp are the same straight segment, so the view stays O(points) and exact. A log (octave) ramp would
be nicer for a cutoff sweep but would make the engine disagree with the drawn line, and the lane view
would have to subsample a curve again. Where a log feel is wanted, the application densifies the
segment into several short linear ones (webseq's templates already do this for Hz and seconds
ranges). Linear costs a few float operations per active ramp per block.

## Engine design

| Piece | Where |
|---|---|
| Event type, ramp table | `native/src/param_events.h` (`ParamEvent`, `ParamRamps`, fixed array, no allocation) |
| Queue | `EventQueue<ParamEvent>` (`scheduler.h`), the third instantiation beside sample triggers and instrument notes |
| Drain + advance | `Engine::process`, right after the transport queue and before the notes' queue |
| Writes | `Engine::applyParam`: effect → `Bus::setEffectParam`, instrument → `ParamModulation::setBase` + `setParam`, sample → `SampleStore::setParam` |
| ABI | `webdsp_schedule_param`, plus `webdsp_pending_param_events / active_param_ramps / dropped_param_ramps` for diagnostics and tests |
| Runtime | `AudioRuntime.scheduleParams` (validates targets and numbers, one batched `schedule-params` message) |

Real-time safety is as for notes: the queue grows in the command handler, `process()` allocates
nothing, and the ramp table is fixed-size.

## Timing: why block granularity

Events apply at the **start of the 128-frame render block** that contains them, and a running ramp
is evaluated at the first frame of each block. That quantizes timing to about 2.7 ms at 48 kHz, which
is a ~50x improvement on the ~150 ms lookahead an application's timer gives, and well below what
anyone hears on a continuous parameter (a filter sweep, a volume curve, a mix).

Making effects sample-exact was considered and rejected:

* Effects process a whole block per call. To land a change mid-block, `FxChain::process` (and every
  bus around it) would have to take the block's event list and split its processing at each event
  frame, for every effect on every bus, every block. FX chains already split into 32-frame chunks when
  an LFO is active; adding event splitting would cost real work on the hot path.
* The gain is inaudible. A parameter on an effect is a control signal, not a note: a 2.7 ms error on a
  filter coefficient or a mix is far below the smoothing the effects already apply (the Utility smooths
  its gain over ~5 ms) and below the resolution a lane is drawn or authored at.
* Block granularity keeps the whole feature a fixed amount of work per block (one queue drain and at
  most 64 ramp evaluations), independent of how much automation is playing.

What this does cost is one specific case, below.

### The one case that cares: a per-note parameter next to its note

Some synth parameters are read **when a note starts** (an attack or release time, a drum's decay).
Two orderings within the same block:

* "set the parameter, then the note": exact. Param events are processed before the block's notes, so
  the note reads the new value. (Tested: a drum's decay set 50 frames before its note, in one block.)
* "note, then set the parameter, a few frames later in the same block": the parameter lands about
  2.7 ms **early**, so that note reads the new value although its time precedes the event. The error
  is at most one block, only for per-note parameters, and only when an event and a note are less than a
  block apart.

For automation this is almost never audible (a lane or a cell FX value changes on step boundaries,
the note and the value on the same step are meant to be heard together, and the "set, then note" order
is the one that matters). If user testing shows it matters, phase 2 below removes it.

## Phase 2 (not built): exact ordering for instruments

Decide after user testing. The change is small and local because instruments already render in
segments split at each retrigger and timed note-off (`Engine::process`'s `renderTo` loop and
`pending` notes):

1. Keep `paramQueue_` draining into a short per-block list instead of applying at block start, but
   only for **instrument** targets: `{offset = atFrame - blockStart, event}`, sorted.
2. In the instrument render loop, treat each such offset as one more split point beside the pending
   notes: `renderTo(offset)`, apply the event (and start its ramp), continue. Ordering between a note
   and an event at the same frame stays "event first".
3. Ramps on instruments are advanced per segment instead of per block (the loop already chunks to 32
   frames while an LFO is active).
4. Effects and samples keep block granularity.

Cost: a few branches in the instrument loop when an instrument has a due event this block, nothing
otherwise. Test: a note and a param event 1 frame apart in one block, both orders.

## Clients

* **webseq** (migrated): `audio/automation.ts` compiles each lane into events (a step, one ramp per
  segment) over the next lookahead window of the transport's step iterations, and refreshes a lane that
  was edited during playback from the current position (a ramp the old data had running is replaced,
  because the newest event per parameter wins). The model, UI, templates and LFOs were untouched. Its
  writers keep a `base` (the last project value) separate from the last *scheduled* value, so stopping
  can restore the project value (`restoreBase`) with direct writes, which also end any running ramp.
* **webtracker** compiles per step already (`compileRowFx`), which knows each step's time: a cell value is
  one step event, and a glide is one `from`/`ramp` event to the next set cell. Nothing is written into
  its model while playing; stop restores the authored base.

## Tests

`native/test/param_events_test.cpp`: a step lands in its block; a linear ramp is monotonic, linear and
ends exactly on `value`; a ramp with no `from` starts at the current base and `from` ignores prior
state; a late event catches up instead of restarting; replace-by-key and direct-write cancel;
`cancelScheduled` drops queued events but not a running ramp; a ramp under an active LFO moves the
base only; instrument and sample targets (the sample one audibly); unknown targets and malformed
events dropped; the full ramp table; a per-note parameter applied before its note in one block.
`test/modulation.test.ts`: command shape and validation. `native/test/smoke.mjs`: the ABI through the
compiled module (ramp, direct-write cancel, `cancelScheduled`).
