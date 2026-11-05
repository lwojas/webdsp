# Noise

A synthesis engine whose primary sound source is noise. Not a drum voice with a noise oscillator, and not
"white noise into a filter": the source, its spectrum, its temporal structure and a bank of resonators are the
instrument. Source: `native/src/instruments/noise.h`; tests: `native/test/noise_test.cpp`. Generic
instrument contract: [instruments.md](./instruments.md).
Identity: `id "noise"`, name `Noise`, category `synth`, `InstrumentKind.Noise = 19`, params `NoiseParam`.

The question this first version answers: *can a noise-first architecture cover enough ground to justify being its
own engine?* Hats, metal, tuned noise, bells, icy and dark atmospheres, risers and impacts all come from the same
15 controls (see [Validation](#validation) for what was measured, and what could not be).

## Signal path

```
source  white | pink | brown | metal | crackle | burst
  --> colour    one-knob spectral tilt, dark rumble .. flat .. bright hat   (level-normalised per source)
  --> texture   stochastic grain gate: smooth .. grainy .. granular .. crackling; rate = density
  --+--> dry ------------------------------------------------+
    +--> six-band resonator bank (ratio table morphs          +--> equal-power mix --> drive (tanh, bounded
         unison / harmonic / bell / metallic cluster)  -------+     regenerative feedback into the bank)
  --> DC block --> tone tilt --> x amp env^2 x velocity --> soft clip (|x| <= 1) --> Panner --> bus

sweep: the whole bank moves with the amplitude envelope level (rise into a note, fall out of a hit).
```

Mono per instance (overlapping hits = several instances). A fresh note resets all state, so identical inputs
render identical samples. A retrigger of a sounding note is continuous: the amplitude fades out over ~2 ms, then
re-attacks, while sources, grains and resonators carry on. Not tempo-synced.

Reused, nothing new in the engine, bus or ABI: `AdsrEnvelope`, `PercNoise` (xorshift, deterministic),
`PercCore` (lifecycle, pre-note delay, pan, routing), `setTableParam`, and PercSynth's tanh drive and soft clip
forms. New and local to `noise.h`: the pink/brown/metal/crackle/burst generators, the tilt filter, the grain gate
and the resonator bank (TPT state-variable, so it can be retuned at control rate without clicks). A new kind is
the one registry entry plus a `NoiseParam` enum.

## Controls

Grouped the way a compact UI would lay them out (the catalogue is the UI contract; the demo app has no
per-instrument panels). `Sustain` and `Release` are long-form extras a compact UI can leave on a second page.

| Group | Param | Range | Default | Effect |
|---|---|---|---|---|
| SOURCE | `source` | step 0-5 | 0 | white, pink (-3 dB/oct), brown (-6 dB/oct), metal, crackle, burst. Different behaviour, not different colours (see below). |
| | `color` | 0-1 | 0.6 | Spectral tilt. 0 = rumble, 0.5 = untouched (exactly transparent), 1 = hat / air. Level-normalised per source, so it moves the spectrum, not the loudness. |
| | `texture` | 0-1 | 0 | 0 continuous; ~0.2 grainy; ~0.5 granular; 1 sparse, clicky crackle. |
| RESONANCE | `tune` | 40-12000 Hz | 900 | Resonator root. Note `pitch` is semitones on top. |
| | `resonance` | 0-1 | 0.4 | Ring: broad band (Q 0.7) to long bell-like decay (Q 2000). Level stays roughly constant as it rises. |
| | `spread` | 0-1 | 0.45 | Layout of the six bands: unison (0), harmonic (0.33), inharmonic bell (0.67), metallic cluster (1); continuous between. |
| | `sweep` | -4..4 oct | 0 | The bank moves by `sweep x envelope level`: positive rises with a swell and falls with a hit, negative the reverse. |
| SHAPE | `attack` / `decay` | 0.5 ms-8 s / 20 ms-8 s | 1 ms / 0.25 s | Amplitude ADSR (linear, applied squared). |
| | `density` | 2-4000 Hz | 40 | Events per second: grains (with `texture`), crackle pops, burst rate. No effect on smooth white/pink/brown/metal. |
| | `sustain` / `release` | 0-1 / 5 ms-8 s | 0 / 0.2 s | Sustain 0 = a self-ending one-shot. With sustain > 0 the note is held: pass `duration` or call `instrumentNoteOff`. |
| CHARACTER | `drive` | 0-1 | 0.15 | Clean, saturated, then (above ~0.25) regenerative ring. |
| | `tone` | 0-1 | 0.5 | Output tilt after the drive, +-6 dB, flat at 0.5. |
| | `mix` | 0-1 | 0.5 | Raw noise (0) to resonators (1), equal power. |

All controls except `source` are modulation targets (`density` and `tune` in the log domain); `source` is a
step. Ranges are enforced in `setParam`; non-finite values are ignored.

## The pieces

**Sources.** White is the xorshift generator. Pink is Kellet's filter; brown a leaky integrator (bounded). **Metal**
is six squares at the inharmonic 808-hat set (205-800 Hz, tracking note pitch), each phase-jittered by noise and
combined by products (XOR-like), giving dense inharmonic buzz. **Crackle** emits random-signed ticks at `density`
per second. **Burst** is white noise in jittered bursts (~35 % duty) at `density` per second. All are scaled to
about the same level (within ~1 dB) so switching source does not jump the volume.

**Colour.** A first-order tilt around 700 Hz (+-12 dB) plus, from the middle outwards, a 12 dB/oct low-pass
sweeping down to ~90 Hz or high-pass sweeping up to ~8.7 kHz. The makeup is computed from the *source's* spectrum
(analytic 1/f / brown models and the filter's exact digital response), not assumed white, so a dark colour on brown
noise is not boosted twice.

**Texture.** After colour, a gate `(1 - d) + d x grain`. Grains start at Poisson times (rate `density`), with random
height, and decay exponentially; `texture` raises the depth `d` over its first third and shrinks the grain from 60 ms
to ~0.25 ms (a click). Dense enough, grains merge back into continuous noise, which is what makes the control a
continuum rather than a mode switch. Level is made up for the gaps. The gate is *before* the resonators, so every
grain excites the bank: grains become pitched pings.

**Resonator bank.** Six parallel TPT band-passes. The ratio table is interpolated (in log) between four anchors:
unison with +-0.4..0.8 % detune, harmonic 1..6, free-bar bell `1, 2.756, 5.404, 8.933, 13.34, 18.64`, and a metallic
cluster `1, 1.483, 1.800, 2.546, 2.630, 3.897`. Higher bands are weighted down and damped faster (as in nature).
Each band is levelled for its excitation: output rms is independent of Q and frequency for white input, and
corrected against the source and colour spectrum at the band's own frequency, so `mix` is a crossfade and not a
loudness knob even for a 55 Hz resonator on brown noise. Bands approaching Nyquist fade out rather than fold.

**Drive and feedback.** Asymmetric tanh with a small bias (even harmonics), compensated at a reference peak,
exactly transparent at 0. Above `drive` ~0.25 a soft-saturated copy of the pre-drive mix feeds back into each band
at unit peak gain. The loop's worst-case small-signal gain is 0.5 x 0.8 x ~2.2 < 1 for any settings, so it
lengthens and thickens rings but cannot latch or run away; everything is also bounded by the final soft clip
(tested over 120 random parameter sets and a 4 s fully-resonant, fully-driven note). The DC blocker's corner is 18 Hz:
the drive's bias rectifies each click into a DC step, and a slower blocker (5 Hz, tried first) dragged it out as a
~30 ms thump after every crackle tick.

## Recipes (the seven validation sounds)

`source / color / texture / density / tune / resonance / spread / sweep / attack / decay / sustain / drive / tone / mix`

| Sound | Settings |
|---|---|
| Tight closed hat | metal / 0.85 / 0 / - / 5200 / 0.35 / 1.0 / 0 / 0.5 ms / 60 ms / 0 / 0.3 / 0.6 / 0.55 |
| Long metallic hat / cymbal | metal / 0.8 / 0 / - / 3500 / 0.55 / 0.9 / 0 / 0.5 ms / 1.6 s / 0 / 0.25 / 0.6 / 0.6 |
| Industrial percussion | burst / 0.45 / 0.5 / 14 / 180 / 0.7 / 0.75 / 0 / 1 ms / 0.5 s / 0 / 0.85 / 0.5 / 0.7 |
| Tuned resonant noise | pink / 0.55 / 0 / - / 220 / 0.85 / 0.33 / 0 / 1 ms / 0.5 s / 0.8 (held) / 0.1 / 0.5 / 1.0 |
| Icy HF texture | white / 1.0 / 0.55 / 500 / 5200 / 0.8 / 0.67 / 0 / 0.3 s / 2.5 s / 0.5 (held) / 0.15 / 0.8 / 0.8 |
| Dark LF drone | brown / 0.15 / 0 / - / 55 / 0.55 / 0.2 / 0 / 1.2 s / 1 s / 0.9 (held) / 0.4 / 0.3 / 0.55 |
| Evolving riser | white / 0.5 / 0.25 / 300 / 400 / 0.6 / 0.5 / +3 oct / 3.5 s / 0.3 s / 1 (held) / 0.3 / 0.5 / 0.8 |

Other directions the same controls reach: a crackle source at low density through a high-resonance harmonic bank
is a plucked/pinged string; `sweep` negative with a short decay is a zap or impact; `mix` 1 with `spread` 0.67 and
long `release` is a bell; `texture` 1 with `density` 2-20 is a Geiger-style crackle; `resonance` 1, `drive` 1 and
`spread` 0 is a sustained, self-thickening drone.

## Validation

`NOISE_WAV_DIR=<dir> ./native/build/noise_test` (after `bash native/test/run.sh` has built it) writes the seven
sounds and a control (white noise high-passed by `color`, nothing else) as 16-bit mono WAVs.

What was measured, on those renders (16384-sample window, 48 kHz; the control is plain filtered white noise):

| | rms | centroid | peak over median bin | spectral flatness |
|---|---|---|---|---|
| control: filtered white | 0.40 | 14.2 kHz | 16 dB | -4.7 dB |
| closed hat | 0.05 | 5.9 kHz | 24 dB | -6.5 dB |
| metallic cymbal | 0.27 | 4.2 kHz | 43 dB | -11.9 dB |
| industrial | 0.13 | 2.1 kHz | 43 dB | -13.8 dB |
| tuned noise | 0.26 | 0.7 kHz | 70 dB | -36.7 dB |
| icy texture | 0.26 | 9.6 kHz | 40 dB | -13.8 dB |
| dark drone | 0.42 | 85 Hz | (all energy at the tune) | -80 dB |
| riser | 0.29 | 6.4 kHz (3.4 s; rises ~2x+ over the note) | 39 dB | -9.7 dB |

Spectrograms of the same renders show what the numbers suggest: the control is flat broadband; the cymbal is
resonance lines over a decaying noise bed; the industrial hit is vertical grain events carrying harmonic lines;
tuned noise is a harmonic stack; the riser is a family of exponentially rising bands. None of them is a filtered
version of the control.

**Not done: listening.** These numbers and images were produced without an audition; they show the sounds are
structurally different from filtered noise, not that they sound good. The seven WAVs are the thing to listen to.
Places the author would listen first: the metal source's aliasing (deliberate, but is it pleasant?), the `icy`
sound's two dominant bell lines (it may want a lower `mix` or `spread` at 1), and the very high-resonance unison
(`spread` 0) beating, which comes from the detuned unison bands and is intended.

## Not in this version

Per-band controls or a band count (one `spread` morph instead), stereo/decorrelated bands, a second envelope, tempo
sync of grains, a `bandwidth` control on the noise (colour covers it), oversampling of the drive. Each is a local
change to `noise.h` if the engine earns expansion.
