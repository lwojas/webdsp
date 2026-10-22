# Choke groups

A **choke group** makes sources cut each other off. The classic case is an open and a closed
hi-hat: playing the closed hat must silence a ringing open hat.

```ts
runtime.schedule([
  { sampleId: openHat,   time: 1.00, bus: openBus,   chokeGroup: 1, duration: 0.5 },
  { sampleId: closedHat, time: 1.25, bus: closedBus, chokeGroup: 1, duration: 0.1 },  // cuts the open hat at 1.25 s
]);
```

## Rules

- `chokeGroup` is a positive integer on `TriggerParams` / `ScheduledEvent`. Omitted (or `< 1`) = none; a voice
  with no group neither chokes nor is choked.
- A trigger into group *g* stops every **active** voice of group *g* (still playing, or in its release tail) that
  plays into a **different `bus`**, then starts normally. Every member of a group can choke every other member.
- Voices on the **same bus** are one source and do not choke each other; their overlap is governed by the usual
  voice behaviour. (A host that gives each of its tracks its own bus gets "track A chokes track B" for free.)
- The cut happens on the audio thread at the new voice's exact start frame, as part of the trigger. A scheduled
  event is therefore choked sample-accurately, with no extra message. Two triggers on the same frame resolve in
  queue order: the later one wins.
- The choked voice fades out over ~2 ms (no click) and ignores its envelope release, so a long tail is cut, not
  decayed. It is then reported through `onVoiceEnded` and reclaimed like any voice that ended on its own.

## Not voice stealing

| | Choke | Voice stealing |
|---|---|---|
| Cause | An explicit group relationship | The pool (`maxVoices`) is full |
| Victim | Active voices of the same group, other sources | The quietest active voice |
| When | Every trigger into a group | Only when no voice is idle |

Choking does not consult or change the pool: it happens with plenty of free voices, and `maxVoices` / steal
behaviour is exactly as before. Choke groups are not implemented as a voice limit.

## Scope

Sample voices only. Synthesized instruments (`createInstrument`) are not choked. Send/receive choke modes,
per-note choke and choke timing are not implemented; `VoiceManager::chokeOthers` is the single place to extend.

Tests: `native/test/choke_test.cpp`.
