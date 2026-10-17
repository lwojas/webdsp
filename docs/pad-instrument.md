# Pad instrument ("Drifting Ensemble")

A sustained, spacious pad: slow swell, long overlapping release, and a sound that keeps moving
while a note is held. Source: `native/src/instruments/pad.h`; tests: `native/test/pad_test.cpp`.
Generic instrument contract: [instruments.md](./instruments.md).

## Research input and honesty

The repo holds no Helsinki- or pad-specific research; the only Gadget text is the Brussels write-up in
[pulse-instrument-proposal.md](./pulse-instrument-proposal.md), plus the sustained-voice entries in
[synth-dsp/techniques.md](./synth-dsp/techniques.md) (#21 supersaw, #19 drift, #24, #25). Korg's manual
(detune.co.jp, *KORG Gadget* user manual) describes Helsinki as a polyphonic ambient/chillwave pad:
a waveform with LOW / HI / HIGHER octave levels and noise, a filter, ADSR, then LO-FI and REVERB
output sections. It gives no DSP detail. This is **not** a Helsinki clone: we borrow the
"pad = layered generator, gentle ADSR, space added at the output" idea and add our own internal
movement and overlap behaviour.

## Candidates considered

| | A. Detuned saw ensemble | B. Evolving filtered oscillator | C. Resonant chamber (comb / resonator bank) |
|---|---|---|---|
| Flow | N saws -> LP -> ADSR | 1-2 osc -> shaping -> slow-moving filter -> ADSR | noise/saw -> tuned delay/resonators with feedback |
| Why it sounds like a pad | Beating between detuned saws; width | Timbre drifts | Room/body resonance |
| Cost | 4 saws + 1 stereo biquad / note | ~1-2 osc + biquad | delay lines + tuning + stability per note |
| Risk | Static if nothing moves (it is Pulse minus pump) | Mono, narrow, no ensemble | Pitch-tracked fractional delays, feedback stability, hard to tune without ears |
| Distinctiveness | Medium | Low-medium | High but unproven |

**Chosen: A + B's movement.** A 4-saw stereo ensemble where *every* detune, pan position and the
filter cutoff drift on slow, unrelated cycles. It reuses everything that exists (PolyBLEP saw
pattern, `BiquadFilter`, `FilterModulation`, `AdsrEnvelope`, `Panner`), adds ~330 lines, and the
movement is what C would have offered (a changing, living spectrum) without a stability risk.

## Signal flow

```
per note (slot, 3 per instance):
  4x PolyBLEP saw, detune offsets {-1,-.37,+.43,+1} x detune, each swinging +-35% x movement
      -> pan positions {-1,-.35,+.35,+1} x space, each drifting +-0.25 x movement
      -> 12 dB LP (stereo), cutoff = tone(250 Hz..8 kHz, keytracked) x 2^(3 oct x signal)
           signal = -0.6 x (1 - envelope) + 0.4 x movement x (drift1 + drift2)/2     (>= f0)
      -> x smoothstep(envelope) x velocity
slots summed -> two allpass diffusers per channel, mixed in by `space` -> soft clip (|x| <= 1)
      -> balance Panner (note pan) -> bus
```

## Public controls

| Param | Range | Default | Effect |
|---|---|---|---|
| `tune` | 27.5-440 Hz | 110 | Root; note `pitch` is semitones on top. |
| `tone` | 0-1 | 0.5 | Brightness (resting cutoff, 250 Hz-8 kHz, keytracked). |
| `movement` | 0-1 | 0.5 | How far the slow drift moves detune, stereo image and filter. 0 = static ensemble. |
| `detune` | 0-1 | 0.4 | Ensemble thickness (outer saws up to +-22 cents). |
| `space` | 0-1 | 0.6 | Stereo width and diffusion. 0 = exactly centred, undiffused. |
| `attack` | 0.02-8 s | 0.8 | Swell time. |
| `release` | 0.1-12 s | 3 | Fade time after note-off (see below). |

Internal: saw count/offsets, drift rates (0.07-0.23 Hz, unrelated), envelope darkening (0.6),
modulation depth (3 oct), filter Q 0.8 and 12 dB slope, diffuser lengths (4-8 ms) and gain, level.

## Behaviour

- **Overlap (the engine contract is unchanged).** One instance still plays one *line*, but it
  keeps up to three notes alive: a new note-on moves the held note into its release and starts a
  fresh voice, so old and new fade across each other. Chords = several instances, as for every
  instrument. If all three slots are busy the quietest tail is taken over; its envelope attacks
  from its current level so there is no restart click.
- **Note-off** releases the held note only; tails keep fading. Timed `duration` works unchanged.
  A note-off before a delayed note starts cancels it.
- **Envelope.** Linear ADSR ramps (decay 0, sustain 1, private) shaped by smoothstep at both ends: gentle
  start, gentle beginning of the fade, soft end. The audible fade is therefore about 0.8 x `release`.
  The instance stays active (and its bus awake) until the last tail ends plus a 40 ms diffuser tail.
- **Filter follows the envelope.** A note blooms from darker to its resting tone as it swells and
  darkens as it fades; drift keeps moving through the release.
- **Determinism.** Saw phases are fixed; drift phases derive from the note's pitch only, so repeated
  notes render identically and chord notes drift out of step with each other.
- **Not tempo-synced.** The pad ignores `syncTransport`; its movement is free-running.
- **Ensemble beating is the character, and the main thing to tune by ear.** Four near-unison saws
  beat slowly (seconds at low pitches); in a mono sum the level swings noticeably. Lower `detune`
  (or the `kMaxDetuneCents` constant) if it is too pulsing. Dump WAVs with `PAD_WAV_DIR=<dir>`.

## Inside vs. outside

Inside: oscillator interaction, detune and pan drift, envelope-following filter, stereo placement
and a short diffusion stage (what makes the *note* wide and dense). Outside: the room. Put
`EffectKind.Reverb` (and chorus, delay, lo-fi/bitcrusher for a Helsinki-like finish) on the bus;
`space` and the bus reverb are meant to be used together. No reverb is built into the instrument.

## Cost

Per sounding note: 4 PolyBLEP saws, 2 biquad channels (12 dB), 4 sines per 32 samples, one
`exp2`/filter update per 32 samples. At most 3 notes per instance, ~4 sample-rate allpass taps per
channel per instance. No allocation in `render()` (delay lines are sized in `configure()`; checked by test).
