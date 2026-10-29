// MIDI routing: letting a host that receives MIDI play it without the page in the path.
//
// A host that gets MIDI itself (a plugin, whose DAW sends it notes) can deliver it to the page as Web MIDI, but the page then has to
// turn each note into an engine call and send that back: a round trip through a WebView, ~10-30 ms plus a block, and nothing plays
// while the plugin's window is closed. A MidiRoute is the page telling the host, ahead of time, what a key should play. The host
// then plays incoming notes itself, sample-accurately on its audio thread, and keeps doing so with the window closed. The page keeps
// receiving the MIDI too (for note entry, CCs), but must not also play it: it checks `capabilities.midiRouting` and, when the host
// can route, declares a route instead of auditioning notes itself. In a browser there is no host that routes, so the capability is
// false and an application plays notes as it always did. See docs/midi-routing.md.

export interface MidiRoute {
  /** What a key plays: a sample (`id` is its SampleId) or a synthesized instrument (`id` is its InstrumentId). */
  kind: "sample" | "instrument";
  id: number;
  /** The MIDI note (0-127) at which the target plays at its own pitch: a key plays `note - unityNote` semitones from it. */
  unityNote: number;
  /** Sample only: the bus it plays into (omit: master). */
  bus?: number;
  /** Sample only: the note picks a cue region (`note - unityNote`, 0 = the first) and never transposes. */
  chop?: boolean;
  /** Sample only: loops while the key is held. */
  loop?: boolean;
  /** Sample only: a new note releases the one before it on this route (monophonic). */
  mono?: boolean;
  /** Instrument only: every key plays at the instrument's own pitch, whatever note it is (a drum pad: a snare on any pad, not a snare
   * transposed by the pad's note). Without it a key plays `note - unityNote` semitones from the instrument's pitch. */
  fixedPitch?: boolean;
  /** Release on key-up: a looping sample's voice, a synthesized instrument's note. Otherwise a note is left to ring out (a pad hit). */
  holdUntilKeyUp?: boolean;
  /** Scales every note's level, 0..1 (e.g. a track's volume). Default 1. */
  gain?: number;
  /** Choke group, as in TriggerParams. */
  chokeGroup?: number;
  /** The MIDI channel to respond to, 0-15; omit for any. */
  channel?: number;
}

const finite = (n: unknown): n is number => typeof n === "number" && Number.isFinite(n);

/** The route cleaned up for the wire, or null when it is not usable (unknown target, non-finite numbers). `known` says whether a target
 * exists; values are clamped to their ranges and only the fields that apply to the kind are kept. */
export function normalizeMidiRoute(route: MidiRoute | null | undefined, known: { sample(id: number): boolean; instrument(id: number): boolean }): MidiRoute | null {
  if (!route) return null;
  if (route.kind !== "sample" && route.kind !== "instrument") return null;
  if (!finite(route.id) || !finite(route.unityNote)) return null;
  if (!(route.kind === "sample" ? known.sample(route.id) : known.instrument(route.id))) return null;

  const out: MidiRoute = { kind: route.kind, id: route.id, unityNote: Math.min(127, Math.max(0, Math.round(route.unityNote))) };
  if (route.holdUntilKeyUp) out.holdUntilKeyUp = true;
  if (finite(route.gain)) out.gain = Math.min(1, Math.max(0, route.gain));
  if (finite(route.chokeGroup) && route.chokeGroup >= 1) out.chokeGroup = Math.floor(route.chokeGroup);
  if (finite(route.channel) && route.channel >= 0 && route.channel <= 15) out.channel = Math.floor(route.channel);
  if (route.kind === "instrument" && route.fixedPitch) out.fixedPitch = true;
  if (route.kind === "sample") {
    if (finite(route.bus) && route.bus >= 0) out.bus = Math.floor(route.bus);
    if (route.chop) out.chop = true;
    if (route.loop) out.loop = true;
    if (route.mono) out.mono = true;
  }
  return out;
}

/** Whether two routes are the same, so an application re-declaring an unchanged route sends nothing. */
export function sameMidiRoute(a: MidiRoute | null, b: MidiRoute | null): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}
