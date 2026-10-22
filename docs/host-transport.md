# Host transport

Some hosts have a clock of their own: a DAW plugin (the host's timeline), Ableton Link, MIDI clock. The
runtime carries that clock to the application without deciding what to do with it. In a plain browser there
is none: `getHostTransport()` returns `null` and `onHostTransport` never fires.

```ts
runtime.getHostTransport(): HostTransport | null
runtime.onHostTransport(fn: (t: HostTransport) => void): () => void

interface HostTransport {
  playing: boolean;
  bpm: number;                 // quarter notes per minute
  beat: number;                // host position (quarter notes) ...
  time: number;                // ... heard at this runtime time (same clock as getCurrentTime())
  loop: { start: number; end: number } | null;
  timeSignature: { numerator: number; denominator: number } | null;
  epoch: number;               // bumped by start / stop / locate / loop wrap / signature change, not by tempo
}
```

The mapping is the whole contract: while `playing`, host position at runtime time `t` is
`beat + (t - time) * bpm / 60` (`hostBeatAt`), and a host beat `b` is heard at `time + (b - beat) * 60 / bpm`
(`hostTimeAt`). Both helpers are exported.

## Following the host

An application that wants to follow keeps its own scheduler and anchors it to the mapping:

- On `playing` going true, or a new `epoch` while playing, pick a start time `t >= now + a small margin`
  (never earlier than the event can still be acted on), compute `b = hostBeatAt(t)`, and start its sequence
  at its own position for `b`, sounding at `t`. Cancel what was scheduled before (`cancelScheduled`).
- On `playing` going false, stop.
- On a tempo-only change (same epoch), re-anchor at the new bpm from the current position.

`time` already includes any latency compensation the host applied, so a follower never sees a start that is
already in the past by more than the host's own delivery delay.

## Realtime

The host reads its position on its audio thread and passes the mapping to the page through a lock-free
queue; the audio thread never waits on JavaScript. The page is told after the fact, which is why `time`
may be ahead of `now` by the host's declared latency: that headroom is what lets the first note of a start
sound on time. See the webvst repo for the native side.
