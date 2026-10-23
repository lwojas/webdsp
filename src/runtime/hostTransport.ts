// Host transport: the clock of whatever is hosting the engine, when that host has one of its own.
//
// In a browser there is no such thing, so `AudioRuntime.getHostTransport()` is null and
// `onHostTransport` never fires. When the runtime is hosted by something with a timeline (a DAW plugin,
// Ableton Link, MIDI clock...) the host reports a mapping between ITS musical position and THIS
// runtime's clock, and an application that wants to follow it schedules against that mapping. The
// runtime never decides what following means; it only carries the mapping (see docs/host-transport.md).

export interface HostTransport {
  /** True while the host's own transport is running. */
  playing: boolean;
  /** Host tempo in quarter notes per minute. */
  bpm: number;
  /** Host position in quarter notes (PPQ) at `time`. */
  beat: number;
  /** Runtime time (seconds, the same clock as `AudioRuntime.getCurrentTime()`) at which the host position equals `beat` is heard. */
  time: number;
  /** The host's loop range in quarter notes, or null when it is not looping. */
  loop: { start: number; end: number } | null;
  /** The host's time signature, if it reports one. */
  timeSignature: { numerator: number; denominator: number } | null;
  /** Bumped by every discontinuity (start, stop, locate, loop wrap, time signature change). Followers
   * resync when it changes; a tempo-only change keeps it. */
  epoch: number;
}

/** Host position in quarter notes at runtime time `time`, extrapolated from the mapping (valid while playing). */
export function hostBeatAt(transport: HostTransport, time: number): number {
  return transport.beat + (time - transport.time) * (transport.bpm / 60);
}

/** Runtime time at which host position `beat` is heard, extrapolated from the mapping (valid while playing). */
export function hostTimeAt(transport: HostTransport, beat: number): number {
  return transport.time + (beat - transport.beat) * (60 / transport.bpm);
}

/** True when `next` is a state a follower must resync to rather than just retime: the transport was
 * started or stopped, or the position jumped (a different epoch). */
export function isHostDiscontinuity(prev: HostTransport | null, next: HostTransport): boolean {
  return !prev || prev.epoch !== next.epoch || prev.playing !== next.playing;
}

/** True when the environment supplies its own audio backend and clock (a plugin host) rather than leaving
 * the runtime to a browser AudioContext. Such a backend needs no user gesture, so an application that wants
 * to follow a host transport can bring its runtime up at startup instead of waiting for a click. Always
 * false in a plain browser. The host marks itself with a global before the page's scripts run. */
export function hasEmbeddedHost(): boolean {
  return Boolean((globalThis as { __webdspHost?: unknown }).__webdspHost);
}
