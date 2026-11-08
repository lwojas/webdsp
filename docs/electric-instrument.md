# Electric

A digital electric-keyboard voice: struck, bell-like, warm, metallic, expressive. Not a Rhodes, Wurlitzer or DX
emulation, not a second VA synth and not an FM workstation: one fixed, purpose-built signal path, eight musical
macros plus Decay, and a default patch that is already a finished sound (a minor 7th, a dub-techno stab, a house
chord, an ambient pad all work). Source: `native/src/instruments/electric.h`; tests:
`native/test/electric_test.cpp`. Generic instrument contract: [instruments.md](./instruments.md).
Identity: `id "electric"`, name `Electric`, category `synth`, `InstrumentKind.Electric = 21`, params `ElectricParam`.

## Signal path

```
per note (a slot; 3 slots so a new note never cuts the previous one's tail)

 body   A: sin(f0 + tine)  + 2nd partial          tine = ratio-1 phase modulator, index = struck burst (fast fall)
        B: sin(f0 x (1 + 2.5..8 cents)) + tine         + sustained part that follows the decay
        sub: sin(f0 / 2)                          mid = A/B mix; side = (A - B) x width  -> the A/B beat is the width
 bell   sin(4 f0 + I x sin(5.6 f0)), two modulator phases (L/R)   I = bell index x its own fast-falling envelope
        + a ring partial at 7.09 f0 (the attack tick)
 strike band-passed noise tick, 6-16 ms
        |
        sum -> crush: sample-and-hold + quantise (the two modulators are quantised too)
            -> warmth: biased tanh, gain-compensated, DC-blocked; bell path softened with a 1-pole
            -> ZDF ladder low-pass 24 dB per channel (cutoff key-tracked 0.35, opened briefly by the strike)
            -> x gate^2 x velocity x loudness trim
 slots summed -> soft clip (|x| <= 1) -> balance Panner -> bus
```

**What makes it an electric keyboard and not an FM patch.** The sound is built from the three things a struck tine
instrument has: a clean fundamental that carries the note, a short-lived bark (the tine index falls in 35-150 ms,
so the spectrum is bright for a moment and then settles into the fundamental), and an upper partial that rings
shorter than the body (the bell, with its own faster envelope, so the sound darkens as it rings). Everything
decays with a two-stage curve (a quick fall off the strike, then the long ring).

**Bell is inharmonic, not an EQ.** The bell pair runs at 4 : 5.6, so the sidebands fall at 1.6x, 9.6x, 7.2x ... f0
(measured: 1.6x f0 is 9 dB below the fundamental at bell 1 and 26 dB below at 0.4; at bell 0 only harmonics
1, 2 and 4 remain). It is audible with the filter shut at 2.2 kHz (+15 dB at 1.6x), which no shelf could do. The
index is capped so the highest sideband stays under 0.42 sr, so high notes lose bell instead of aliasing.

**Crush is part of the synthesis.** Sample-and-hold (hold 1 to 9 samples, fractional) and quantisation (15 to ~6
bits) act on the summed voice *before* warmth and the filter, and the two FM modulators are quantised as well
(256 to ~6 levels), so the sidebands are stepped. Because the filter follows, Cutoff tames it (9-22 kHz drops
25 dB at 1.5 kHz). It is applied before the velocity gain, so a soft note is as crunchy as a hard one.

**Overlap.** One instance is one voice (like every kind): a new note moves the previous one into its release and
starts in a free slot, so stabs ring out under the next stab. More than three overlapping notes steal the
quietest tail with a 2 ms restart fade. Play chords with one instance per note.

Reused, nothing new in the engine, bus or ABI: `AdsrEnvelope` (the gate: attack, release, restart fade),
`MatrixFilter` (ladder), `PercNoise` / `PercBandpass` (the tick), `PercCore` (lifecycle, pan), `Panner`,
`setTableParam`, the LFO sample-and-hold hash (per-note variation). New and local: the oscillator stage, the
curves, the trims. A new kind is one registry entry plus an `ElectricParam` enum.

## Controls

Front panel: `body`, `strike`, `bell`, `warmth`, `age`, `crush`, `cutoff`, `resonance` (+ `decay`). `tune`,
`release`, `width` are the long-form extras. All macros are 0..1.

| Param | Range | Default | Effect |
|---|---|---|---|
| `body` | 0-1 | 0.55 | Thin and delicate to full: fundamental weight 0.3-1.0, the 2nd partial falls, a sub octave comes in above 0.5, the sustained tine brightness falls. Not a gain: at 0 the 2nd partial is 6+ dB closer to the fundamental. |
| `strike` | 0-1 | 0.5 | Soft and rounded to struck. Attack 41 ms to 1.5 ms, tine burst index 0.15 to 2.5, noise tick, larger fast-decay share. Band 3-12 kHz in the first 40 ms rises ~10 dB. |
| `bell` | 0-1 | 0.4 | Pure to metallic: bell index 0.2-3.2, level 0.04-0.54, ring partial; the bell rings 0.1-1.8 s, always shorter than the body. |
| `warmth` | 0-1 | 0.4 | Clean to dense: biased tanh drive (gain 1-5.2, bias 0.03-0.19), a detuned second oscillator (2.5-8 cents) beating against the first, bell path softened (16 kHz to 4.5 kHz). Harmonics 3-8 rise ~7 dB at 1; level compensated. |
| `age` | 0-1 | 0.25 | Precision to imperfection: per-note tuning offset (+-4 cents), slow pitch drift (~+-5 cents over seconds), tine index wander (+-18 %), bell ratio wander, modulator phase, bell tint. 0 is exactly repeatable. Measured over a 5 s note: 9 cents total range at 1, 0.2 at 0. |
| `crush` | 0-1 | 0.1 | Clean to degraded: see above. At the default the modulator steps are inaudible and the hold is ~1.01 samples. |
| `cutoff` | 80-18000 Hz | 7000 | Ladder cutoff at C4; key-tracked at 0.35 (an octave up opens it 0.35 octave), opened up to 1.1 octave by a strike and by velocity. |
| `resonance` | 0-1 | 0.15 | Emphasis 0.04-0.9 of ladder loop gain: never self-oscillates (a held max-resonance note rings out, peak < 0.01 after 2 s). |
| `decay` | 0-1 | 0.5 | Short to sustained: ring time constant 0.12 s (0), 0.25 s (.25), 1 s (.5), 6 s (.75), 50 s (1). Time to fall 20 dB (measured): 0.28 s at 0, 0.54 s at .25, 2.0 s at .5, and more than 6 s from .75 up. Also scales the release (x(0.5 + decay)). |
| `tune` | 32.7-1046.5 Hz | 261.63 | Root (C4); a note's `pitch` is semitones on top. |
| `release` | 20 ms-4 s | 0.35 | Key-up time at decay 0.5; scaled by decay. The attack is derived from `strike`, sustain is the decay curve: hold with `duration` or `instrumentNoteOff`. |
| `width` | 0-1 | 0.6 | Stereo spread of the oscillator pair and the bell. 0 is exactly mono. Default L/R correlation ~0.73, width 1 ~0.39; the mono sum does not change with width. |

Modulation targets: all macros and `cutoff` / `tune` (log). Velocity scales level (soft notes quieter, 0 silent),
the tine burst, bell index, tick and the cutoff opening, so a harder note is brighter as well as louder (band
2.5-16 kHz vs 150-700 Hz +3 dB or more between velocity 0.4 and 1); the effect is deliberately moderate so
sequenced parts stay predictable.

## Starting sounds

`ELECTRIC_PRESETS` in `src/runtime/types.ts` (set each entry with `setInstrumentParameter`): `electric` (the
default), `bright` (strike and bell up, open), `warm` (bell down, warmth up, dark), `digital` (bell and crush up),
`keys` (soft strike, long decay), `glass` (bell 1, thin body, open). Not a library: the macros are the instrument.

## Gain staging

A macro must not change loudness. A held default note peaks at 0.34 (-9 dBFS) and its first-400 ms rms is about
-14.5 dB. Body, strike, bell, warmth and resonance each add energy, so each has a five-point trim in dB measured
by sweeping it alone and held at zero at the default (`trimDb`). Result: every macro swept 0 to 1 stays within
+-0.4 dB of the default (the test asserts +-1.5 dB); all macros at their end stops stay below 1.0 peak. Width is
deliberately *not* trimmed: that would trade the mono sum for the stereo sum. Decay is not trimmed either: it
changes the energy by design. The output is bounded by a soft clip (|x| <= 1 by construction) and the ladder,
saturator and quantiser are bounded internally. Macro changes are smoothed at control rate (12 ms); envelopes and
the strike opening are interpolated per sample. CPU: ~0.7 % of a core per held voice natively (-O2), ~1.9 % for a
four-voice chord.

## Using it

```ts
const keys = rt.createInstrument(InstrumentKind.Electric, { bus });
for (const [p, v] of Object.entries(ELECTRIC_PRESETS.warm)) rt.setInstrumentParameter(keys, Number(p), v);
rt.instrumentNoteOn(keys, { velocity: 0.9, pitch: 3, duration: 0.4 });
```

One instance per chord note. There is no per-instrument panel in the demo app (as for every other kind): the
catalogue (`getInstrumentParams`) is the UI contract.

## Validation

Native tests (`native/test/electric_test.cpp`, 287 checks): registration, stable names, ranges and defaults; every
parameter changes the sound; clamping and non-finite input; lifecycle, release, timed notes, velocity, pre-note
delay; determinism and pitch (+12 doubles, Tune moves the root); strike (attack ratio, rise time, 3-12 kHz) and
decay (time-to-fall ordering and bounds); body (fundamental / 2nd partial / sub), bell (inharmonic partials at 1.6x
and 9.6x, present with the filter shut, shorter-lived than the body), warmth (harmonics, level); age (pitch drift
bounded, notes differ, age 0 repeats); crush (digital band, level, tamed by the cutoff); cutoff, resonance (peak at
the cutoff, no self-oscillation) and velocity brightness; stereo (width 0 mono, correlation falls, mono sum
steady, pan); a minor 7th chord, overlapping tails, click-free retrigger, slot stealing, slots freeing; loudness
within +-1.5 dB for every macro; 40 random end-stop patches finite and |x| <= 1; all-min and all-max at pitches
-48 to +48 and velocity 4; 44.1 and 96 kHz; mid-note parameter jumps do not click; no allocation in the audio
path; the six starting sounds finite, level-matched and ordered by brightness.

`ELECTRIC_WAV_DIR=<dir> native/build/electric_test` writes stereo listening renders: a Cm7 chord, a dub-techno
stab and a default stab, a sustained chord, a melody, the six starting sounds, and a five-step sweep of each of
body, strike, bell, warmth, age, crush and resonance.

**Not done: critical listening.** The tuning above is from spectral, envelope and level measurements; it was not
auditioned. Whether the default reads as "a really nice electric synth" is for ears. The constants most likely to
want a nudge are the bell ratio pair (4 : 5.6, `bellCar` / `bellMod`), the ring partial (`kRingRatio`), the tine
index range in `tinePeak` / `tineSus`, the warmth drive (`g`, `bias`) and `kKeyTrack`. The shared `modulation_test`
has one failing check (`plain` vs `zero` LFO render) that exists without this instrument.
