# Bass instrument

`InstrumentKind.Bass` — a compact, sustained bass voice with its own filter and ADSR inside the
instrument. Client integration notes are in [`bass-client-guide.md`](./bass-client-guide.md). Design rationale and the rejected alternatives are in
[`bass-instrument-proposal.md`](./bass-instrument-proposal.md). Source: `native/src/instruments/bass.h`.

## Signal path

```
saw (PolyBLEP) --+
sine ------------+-- body crossfade -- + sub sine (-1 oct)
                          |
   asymmetric tanh  <- drive  = f(bite, timbre env, velocity)   (re-evaluated every 16 samples)
                          | DC block (5 Hz corner)
   BiquadFilter LP 24 dB <- cutoff = f(cutoff, keytrack, timbre env, velocity), floored at 0.9 f0
                          |       Q = f(resonance), level-compensated
   x amp env^2 x velocity x 0.8 -> soft clip (|y| <= 1) -> Panner -> bus
```

## Parameters (the whole public surface, 11)

| Param | Range | Default | Unit | Meaning |
|---|---|---|---|---|
| `tune` | 27.5–110 | 55 | Hz | Root. Note `pitch` is semitones on top, like the drums. |
| `body` | 0–1 | 0.5 | | Round/subby (sine + sub) to rich (saw). |
| `bite` | 0–1 | 0.4 | | Onset drive/brightness, and how much velocity changes timbre. 0 = clean; velocity is only level. |
| `cutoff` | 40–4000 | 400 | Hz | Resting low-pass cutoff (keytracked, 50%). |
| `resonance` | 0–1 | 0.25 | | Q 0.71 – ~9.7, level-compensated. |
| `attack` | 0.001–0.25 | 0.003 | s | |
| `decay` | 0.03–2 | 0.35 | s | |
| `sustain` | 0–1 | 0.6 | | The level the note actually holds at. |
| `release` | 0.01–1.5 | 0.1 | s | Read at note-off, so it applies to held notes. |
| `legato` | 0–1 | 0 | | >= 0.5 = on. A note landing on a held note continues the voice (see Legato and slide). Off = v1 behaviour. |
| `slide` | 0–0.5 | 0 | s | Legato pitch glide time. 0 = immediate. Read when the legato note arrives. |

Control ranges come from `getInstrumentParams(InstrumentKind.Bass)`; never hardcode them.

## What gives it character

| Mechanism | Audible contribution |
|---|---|
| Sine + sub under the saw (`body`) | The fundamental stays present as the filter closes. |
| Cutoff floored at 0.9 × the note's fundamental | The filter makes the note dark and round but can never delete it (a 24 dB LP set below f0 would attenuate it ~35 dB). |
| Shaper *before* the filter, asymmetric | Even harmonics (2nd) that carry on small speakers; the moving filter windows harmonics the note generates. |
| Drive and cutoff follow an internal timbre envelope | Note starts rich, settles round: timbre plucks, not only level. The envelope uses the public attack/release, decay x0.5, sustain x0.4. |
| Velocity -> drive and cutoff (scaled by `bite`) | Harder = brighter and grittier; not a scaled copy. |
| Keytrack (~50%) | Same relative brightness across the range. |
| Onset pitch transient (+0.5 st x bite, ~12 ms) | Small "thump". First thing to cut if it does not earn its keep by ear. |

The envelope -> cutoff mapping goes through the shared [filter modulation](./filter-modulation.md) path (signal = timbre envelope, depth = bite x 4 oct).

Internal (never exposed): sub level, shaper pre-gain range (−20..+30 dB), asymmetry, keytrack
amount, timbre-envelope derivation, envelope->cutoff depth (4 oct x bite), Q map and
compensation, 24 dB slope, sub-block size (16), start phase (sine peak), output level.

## Behaviour

- **Mono per instance** (engine contract). Chords/layers = more instances.
- **Fresh note:** phase reset to the sine's peak, filter and DC state cleared; identical inputs
  render identical samples.
- **Retrigger (note-on while sounding or releasing):** continuous. Phase carries; both
  envelopes re-attack from their *current level*; pitch jumps to the new note. This is also what
  every note does with `legato` off, and what a note on a *releasing* voice does with it on.
- **Note-off:** starts `release`; repeated note-off is a no-op. A note-off before the note's
  pre-delay elapses cancels it. The instrument is active (and keeps its bus awake) until the
  tail ends. `sustain = 0` makes a plucked note that ends itself after `decay`.
- **Velocity:** linear level, plus timbre in proportion to `bite`. Output is bounded for any
  velocity (soft clip), so `velocity > 1` cannot exceed full scale.
- **Pitch range:** fundamentals clamped to 20 Hz – 1 kHz (and 0.2 x sample rate).

## Legato and slide

With `legato` on, a note-on that lands on a note that is **held** (sounding, not releasing) is a
legato transition. Anything else is the ordinary retrigger above.

- **Envelopes:** untouched. No re-attack, no decay restart, no thump, no filter/timbre reset: the
  amp and timbre envelopes keep their stage and level, so there is no dip and no click.
- **Velocity:** the held note's velocity is kept. A new velocity would step the level (and, via
  `bite`, the timbre) in the middle of a held note.
- **Pitch:** moves to the new note. `slide = 0` jumps; `slide > 0` glides **linearly in pitch
  (semitones) over `slide` seconds**, so C->C# and C->C2 take the same time, and lands exactly on
  the target. The rate is per-sample, so the result does not depend on block size (pitch is
  evaluated every 16-sample sub-block, as the rest of the control rate). A note arriving mid-glide
  starts from the pitch the glide has reached. Cutoff keytracking follows the gliding pitch.
- **Ownership:** last note wins; there is no held-note stack. `noteOff()` carries no note
  identity and the engine arms a single release per instrument, so "return to the previous held
  note" cannot be represented; adding it would need a pitch on note-off. The sequencer's
  per-note `duration` is the hand-off: the new note's `duration` replaces the previous note's
  armed release (a note without one cancels it), and a note-off / duration end on the last note
  releases the voice.
- **What makes two notes legato from a sequencer:** the previous note must still be held when the
  next begins. A note-off at exactly the next note's frame still counts (the engine applies the new
  note first), so `duration >= step` is legato and `duration < step` (e.g. `0.9 x step`) leaves a
  gap and re-attacks. Slide never applies across a gap.
- The new note's pan is applied (the panner smooths it); its pitch is the only other thing used.

## Timed note-off (`duration`)

Instruments gained an optional `duration` on notes (also used by any future sustained kind):

```ts
runtime.instrumentNoteOn(bass, { time, velocity: 0.8, pitch: 7, duration: 0.25 });
```

- Seconds, counted on the audio thread from the frame the note actually starts (so it is exact
  across render blocks and with a scheduled `time`). Omitted = held until `instrumentNoteOff`.
- Engine-side: `InstrumentSlot::releaseAtFrame`; the instrument's render block is split at that
  frame (render, `noteOff()`, render). The `Instrument` interface and the drums are unchanged;
  drums should simply not pass `duration`.
- A new note without a `duration` cancels a previously armed release. Releases belong to notes
  that already started, so `cancelScheduled()` does not drop them (it would leave a note hanging);
  queued notes it drops never arm one. `removeInstrument` drops everything.
- ABI: `webdsp_instrument_note_on` and `webdsp_schedule_instrument_note` gained a trailing
  `durationFrames` (-1 = none). Rebuild the WASM and `lib/` together; a stale caller passing
  nothing would read as 0 (immediate release).

## Cost

Per sample: PolyBLEP saw, 2 sines, 1 tanh, a 24 dB biquad, 2 linear ADSRs. Roughly 1.5–2x a Kick
(estimate, not a benchmark). Filter coefficients and drive update every 16 samples. No allocation
in `render` (counted in `bass_test.cpp`). No oversampling or ADAA: at bass pitches the shaper's
alias products land far below audibility and a low-pass follows it.

## Tests

`native/test/bass_test.cpp` (run by `npm run test:native`; `BASS_WAV_DIR=<dir>` dumps WAVs):
bounded/finite/deterministic; pitch accuracy (DFT peak, ±0.3%); ADSR timing and true sustain
level; filter closing, 24 dB slope and keytracking; resonance fuzz and live-parameter slamming;
param validation and metadata; velocity (pure level at bite 0, timbre at bite 1); continuous
retrigger; filter modulation (bite 0 = no timbre movement, monotone in bite, amp-ADSR lifetime identical at every bite, finite/bounded at extremes, same relative depth across registers, same-pitch legato bit-matches one held note); release/cleanup; sample-accurate timed note-off; bus routing; zero allocation; and
guards that it is not "saw -> LP -> ADSR" (timbre moves while level is flat, even harmonics,
fundamental retention, velocity nonlinearity, spectral distance from a naive reference).

## Tuning status

All numeric constants (drive range, timbre-envelope ratios, thump, Q map) are first-pass
values set by measurement, **not yet by ear**. Dump WAVs with `BASS_WAV_DIR` and tune. If it
sounds generic, the escalation path is an instrument-private nonlinear ladder filter (see the
proposal, Candidate 3), not an extension of `BiquadFilter`.
