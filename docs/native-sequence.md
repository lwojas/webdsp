# Native sequence

`runtime.setSequence(sequence)` hands a native host the song, so the host plays it itself.

## Why

An application that sequences from the page (a timer scheduling a short window ahead) stops when the page does. In a plugin the page lives in a WebView that is destroyed when the editor window closes, and it cannot keep pace with a DAW's offline bounce. A `Sequence` moves the *playing* to the host while the page stays the editor.

## Contract

- Available when `runtime.capabilities.nativeSequence` is true (protocol version 3). Elsewhere `setSequence` is a no-op and the application sequences from the page as it always did.
- A `Sequence` is one cycle: `lengthBeats` (quarter notes) and `events` at `beat` in `[0, lengthBeats)`. The host repeats it, so at DAW position `p` it plays what the cycle has at `p mod lengthBeats`. Tempo, loops, locates and stop/start are the DAW's.
- Instrument events may carry `tie: true` (a 303-style slide): the note glides into the note still held on that instrument with no re-attack, whatever its Legato switch says (Bass and FM; other instruments retrigger as usual). Give the held note a `durationBeats` that reaches past the tied note's start. Additive: a host that predates the field plays the note as an ordinary retrigger.
- Event kinds: `sample` (a sample voice), `instrument` (a note on a synth or drum, released after `durationBeats` if given), `param` (a base-value write or ramp on an effect, instrument or sample parameter, as `scheduleParams`).
- `mono` + `track` on sample events: a new event of the same track releases that track's previous voice. `chokeGroup` works as for `trigger`.
- Every call replaces the previous sequence (swapped in at a block boundary); `null` clears it. Events naming a sample, instrument or effect the runtime does not know are dropped, so send the sequence after creating what it uses, and again when those change.
- Hosts cap a sequence at `MAX_SEQUENCE_EVENTS`.
- The host releases the sequence's sounding voices when the transport stops or locates.
- Application contract: while `nativeSequence` is true and a sequence is set, the page must not also schedule the same notes itself, or they sound twice.
