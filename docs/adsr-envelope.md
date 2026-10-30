# ADSR amplitude envelope

Every voice has an ADSR amplitude envelope. This doc has two audiences: **application and
agent authors** consuming it (start at "Using it"), and **engine/instrument authors** reusing
the DSP (see "Internals").

## Layers

| Layer | What | Who touches it | Where |
| --- | --- | --- | --- |
| Public control model | `EnvelopeParams { attack, decay, sustain, release }` | applications (tracker, sequencer, agents) | `src/runtime/types.ts`, `src/runtime/index.ts` |
| Instrument abstraction | the envelope is a persistent property of the instrument (today: the `Sample`, via `SampleParam.EnvAttack..EnvRelease`) | the runtime / future instruments | `native/src/params.h`, `sample_store.h`, `voice.h` |
| DSP | `AdsrEnvelope` + `AdsrParams` | engine authors | `native/src/dsp/adsr_envelope.h` |

Applications never see stages, frame counts or coefficients, and should not use the
`SampleParam.Env*` ids directly.

## Using it

```ts
// At load time...
const kick = await runtime.loadSample(bytes, {
  name: "kick",
  envelope: { attack: 0.005, decay: 0.15, sustain: 0.7, release: 0.25 },
});

// ...or any time later. Only the fields present change.
runtime.setSampleEnvelope(kick.id, { release: 0.4 });

// Note-on / note-off:
const v = runtime.trigger({ sampleId: kick.id });       // note-on
runtime.release(v);                                       // note-off -> release stage
runtime.trigger({ sampleId: kick.id, duration: 0.5 });   // note-off after 0.5 s, on the audio thread
runtime.schedule([{ time: t, sampleId: kick.id, duration: 0.25 }]); // sequencer notes
```

### Parameters

| Field | Unit | Range | Default | Meaning |
| --- | --- | --- | --- | --- |
| `attack` | seconds | 0..30 | 0 | silence -> peak |
| `decay` | seconds | 0..30 | 0 | peak -> sustain level |
| `sustain` | level | 0..1 | 1 | held level (fraction of peak) |
| `release` | seconds | 0..30 | 0.01 | level at note-off -> silence |

Out-of-range, negative, NaN or infinite values are clamped (NaN/Inf become 0). The defaults
leave the sound unchanged apart from a 10 ms click-free release, which is exactly the engine's
behaviour before envelopes existed — so existing consumers are unaffected.

### Semantics an application can rely on

- **Lifecycle:** note-on -> attack -> decay -> sustain (held) -> note-off -> release -> voice
  ended. The voice stays alive for the whole release; `onVoiceEnded` fires once, only after the
  release has reached silence. Don't free resources or reuse the handle on note-off.
- **Note-off sources:** `release(handle)`, or `duration` on `trigger()`/`schedule()`. Prefer
  `duration` for sequenced notes (computed sample-accurately on the audio thread).
- **Live edits (deterministic):** `attack`, `decay`, `sustain` are captured at note-on, so
  editing them affects notes triggered afterward only. `release` is read at note-off, so a
  changed release applies to notes still held. Edits are per-instrument, not per-note.
- **Retrigger / voice steal:** if a note-on lands on a still-sounding voice (steal, or during
  release) the attack ramps from the current level over the full attack time; the envelope
  never jumps. (The sample itself still changes, as with any steal.)
- **Instant stages:** a time of 0 is an instant stage, `release: 0` is a hard stop.
- **Sample end wins:** a non-looping sample that runs out ends the voice immediately, whatever
  the envelope; loop the sample or use a long enough sample for long releases.
- **Shape:** linear in amplitude. A stage of N frames reaches its target on frame N; release
  renders N non-zero frames then silence.
- **Missing sample:** `setSampleEnvelope` on an unknown/removed sample id is a silent no-op.
  Re-apply the envelope after replacing a sample's PCM (a re-commit is a new sample, same as gain).

### Guidance for agents / new consumers

- Use `EnvelopeParams`, `setSampleEnvelope()` and `loadSample({ envelope })`. Do not call
  `setSampleParameter(id, SampleParam.EnvAttack, ...)` or reach into `native/`.
- Express note length with `duration`, not a second timed `release()`.
- Keep UI/control code in terms of the four named controls and their units; don't expose raw
  frame counts or curve coefficients.
- An envelope is per-instrument. For per-note level use `gain` (velocity), not the envelope.
- Not available yet: per-voice envelope overrides, curve shapes, pitch/filter envelopes.

## Internals

`AdsrEnvelope` (`native/src/dsp/adsr_envelope.h`) is a plain, non-virtual class with no
knowledge of voices or samples:

- `noteOn(AdsrParams)` / `noteOff(AdsrParams)` start the attack / release; `next()` returns this
  frame's level then advances; `isActive()` is the lifetime signal; `reset()` clears it.
- Stage lengths are counted in frames (`framesLeft_`), rounded to the nearest frame with a
  minimum of one for any positive time, and the stage snaps to its exact target when the count
  expires — no float-accumulation drift.
- `Voice` owns one envelope and reads `Sample::envelope` (an `AdsrParams`) at note-on and
  note-off. Release finishing is what ends a releasing voice; `stop()` and finishing reset it.
- `VoiceManager` is unchanged: a releasing voice is not idle, so it is only taken by stealing,
  which uses the live envelope level (`amplitudeEstimate`).
- No allocation or virtual dispatch in `process()` (`native/test/adsr_test.cpp` counts heap
  allocations across a full lifecycle).

### Reusing it in a new instrument

Own an `AdsrEnvelope` (one per envelope: amp, pitch, filter), keep your own `AdsrParams`, call
`noteOn`/`noteOff` from your note lifecycle and multiply/modulate by `next()` per frame. Map
the instrument's public controls onto `AdsrParams`; expose only what is meaningful for that
instrument. Don't add a generic modulation layer around it.

### ABI / ids

| `SampleParam` | id | unit |
| --- | --- | --- |
| `EnvAttack` | 1 | seconds |
| `EnvDecay` | 2 | seconds |
| `EnvSustain` | 3 | 0..1 |
| `EnvRelease` | 4 | seconds |

Mirrored in `native/src/params.h` and `src/runtime/types.ts`; pinned by `test/paramIds.test.ts`.
No new WASM exports: it rides `webdsp_set_sample_param`.

## Tests

`native/test/adsr_test.cpp`: envelope in isolation (stage shape, frame convention, extreme
times), voice integration through `Engine` (lifecycle, live-edit semantics, retrigger in every
stage, steal/stop/ended-event regressions, duration auto-release, legacy defaults, no
allocations). Run with `npm run test:native`.

## Retrigger declick (monophonic synths)

Bass, Edge, FM and PWM Beast re-attack from silence on a retrigger (`noteOnRestart`): the previous level
is faded out over `kRestartFadeSeconds` (2 ms) instead of carried into the new attack, which would hide
the attack of back-to-back notes. The fade also masks the step from the new pitch/velocity.

For a **scheduled** note the engine knows the frame ahead of time, so it ducks the sounding note *before*
that frame (`Instrument::duck`, `AdsrEnvelope::duck`): the old note reaches silence on the new note's own
frame and the attack starts there, on time. Applies when the whole 2 ms fits before the note (one block of
lookahead is peeked) and nothing else starts on the instrument in between; legato notes are never ducked.
Otherwise, and for immediate notes (`instrumentNoteOn`), the fade runs after the note starts as before.
A ducked instrument whose note is cancelled holds silence for at most `kHeldMaxFrames`, then ends.
Percsynth defers its pitch/velocity to the end of its fade and keeps the post-note fade.

## Per-trigger override (`TriggerParams.envelope`)

The envelope belongs to the sample, so every voice of one sample shares it. When several callers share one sample but
want their own decay or release (for example tracks of a groovebox pointing at the same loaded kick), pass `envelope`
on the trigger or scheduled event:

```ts
runtime.trigger({ sampleId, duration: 0.05, envelope: { release: 0.3 } });   // this voice only
runtime.schedule([{ sampleId, time, envelope: { decay: 0.2, sustain: 0 } }]);
```

Each field set replaces the sample's value for that voice; omitted fields inherit (and a live edit of an inherited field
behaves as before). Timing rules are unchanged: attack, decay and sustain are read at note-on, release at note-off. On the
wire an omitted field is NaN (`webdsp_trigger` / `webdsp_schedule_event` take four trailing floats), so existing callers
are unaffected. Native `Sequence` sample events do not carry the override yet.
