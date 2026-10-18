# Sample cue regions and chop playback

Code: `native/src/sample_store.h` (`SampleRegion`, `Sample::regions`, `regionBounds`),
`native/src/engine.h` (`trigger` / `scheduleEvent` / scheduler drain), `native/src/api.cpp`
(ABI), `src/runtime/` (`TriggerParams.region`, `setSampleRegions`, `regionForNote`). Tests:
`native/test/sample_regions_test.cpp`, `test/sampleRegions.test.ts`.

## What it does

A sample can carry a list of **cue regions** (`[start, end)` in frames). A trigger can then
play one region instead of the whole sample ("chop" playback). Nothing is copied or cut: a
region is just a pair of offsets into the existing PCM.

| | |
| --- | --- |
| Regions belong to | the **sample** (they describe the audio). |
| Playback mode belongs to | the **trigger** (how this note interprets the sample). |
| Chromatic (default) | no `region` on the trigger: whole sample / `start`..`end`, pitched by `rate`/`pitch`. Unchanged from before. |
| Chop | `region: i` on the trigger: plays region `i` (0-based). |

So the same sample can be played chromatically by one track and chopped by another; there is no
mode flag stored on the sample.

## How it works

* **Setting regions** is a message to the worklet (`set-sample-regions`), which clears the
  sample's list and adds each region. Bounds are clamped to `[0, length]` (`end >= start`); at
  most `kMaxSampleRegions` (256) are kept. An unknown sample id is a silent no-op.
* **Resolving** happens when a voice starts: `Engine::trigger` for immediate triggers, and when a
  scheduled event comes due for scheduled ones. The region's `start`/`end` replace the trigger's
  `start`/`end`, then the unchanged `Voice` plays it. Because it resolves at fire time, regions
  edited after an event was scheduled are honoured.
* **A region that doesn't exist** (no regions, index out of range, or an empty region) plays
  nothing, like an unknown sample id. No voice is stolen.
* **Inside a region** `rate`/`pitch`, `loop` (loops within the region), `reverse`, `duration`,
  gain, pan, the filter and the amplitude envelope all work as usual.
* **Region end** is a hard stop (like any `end`); playback never runs into the next slice. For a
  click-free tail use `duration` together with the sample envelope's release.
* **Real-time safety:** regions are only edited on the message handler; the render thread only
  reads them. No allocation at trigger time.
* **Lifetime:** regions are freed with the sample. Re-committing a sample id (including
  `removeSample` + reload) creates a fresh sample with **no regions**, like its gain; re-apply them.

## Consuming it from an app

```ts
// 1. Load with regions (frames at the sample's rate), or set later.
const s = await runtime.loadSample(buf, { regions: [{ start: 0, end: 24000 }, { start: 24000, end: 48000 }] });
runtime.setSampleRegions(s.id, newRegions);        // replaces the whole list
s.regions;                                          // SampleMetadata.regions mirrors what was sent

// 2. Chop trigger: pick the region, don't send pitch.
runtime.trigger({ sampleId: s.id, region: 3, time, duration });
runtime.schedule([{ sampleId: s.id, region: 3, time }]);   // same field on scheduled events

// 3. Note -> region (consecutive notes = consecutive regions).
const r = regionForNote(note, rootNote, s.regions?.length ?? 0);
if (r !== undefined) runtime.trigger({ sampleId: s.id, region: r, time });
```

Suggested app-side model (not part of webdsp):

```
Sample    { audio, regions[] }                    // saved with the sample
Track     { sampleId, mode: "chromatic" | "chop", rootNote }
note event -> mode === "chop" ? { region: regionForNote(...) } : { pitch: note - root }
```

Notes for apps:

* `regionForNote` maps consecutive semitones (C, C#, D, ...), not white keys only. An app that
  wants a different layout computes the index itself; `region` is just an index.
* Persist regions in the project next to the sample and call `setSampleRegions` after every
  load/reload of that sample. Existing projects have no regions and behave exactly as before.
* Use `duration` (note length) with a short release to avoid clicks at region ends.
* Notes outside the region range should be skipped (`regionForNote` returns `undefined`).

## Cue-point helpers (`src/runtime/chop.ts`)

Apps edit cue points as **fractions** (sorted interior boundaries, 0..1 of the sample) kept with
the sample in their own project (webdsp stores no project state), and hand the engine frame
regions. The pure maths for that is exported so apps don't each re-implement its invariants:

`normalizeCues`, `evenCues`, `addCue`, `moveCue` (clamped between neighbours), `removeCue`,
`sliceCount`, `cueRegions(cues, lengthFrames)` -> `setSampleRegions` input, `MIN_SLICE`,
`MAX_SLICES` (256, the engine cap; `normalizeCues`/`addCue`/`evenCues` take an optional smaller
cap), and `computePeaks(channels, bins)` for a waveform overview (decoding stays with the app).

Not in webdsp, deliberately: playback mode, note -> slice mapping beyond `regionForNote`,
persistence and the editor UI.

## Raw ABI (for non-TS hosts)

`webdsp_clear_sample_regions(sampleId)`, `webdsp_add_sample_region(sampleId, start, end)`.
`webdsp_trigger` / `webdsp_schedule_event` take a trailing `regionPlusOne`: `0` = whole sample,
`n >= 1` = region `n-1`. (Offset by one so an omitted trailing argument, which JS passes as 0,
keeps the old behaviour.) The C++ `Engine` API is 0-based with `-1` = none.
