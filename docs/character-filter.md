# Character Filter

A resonant filter built to have a personality, for any webdsp source. It is our own design, not an emulation of any commercial filter. The question it answers: can a filter feel like an instrument rather than a transparent utility? Source: `native/src/dsp/character_filter.h` (the primitive), `native/src/dsp/character_filter_fx.h` (the FX-chain node), tests in `native/test/character_filter_test.cpp`.

**Status.** The DSP is measured, not yet auditioned by the author of this document. Everything below under "Measured" is a number from the test suite; whether it sounds good is for ears. `CHARACTER_FILTER_WAV_DIR=<dir> native/build/character_filter_test` renders the validation matrix (8 sources x 4 corners x LP/HP, plus two modulation sweeps each) as WAVs.

## Using it

**As an effect** on any bus (drums, samples, synths, the Noise instrument): `EffectKind.CharacterFilter` (13), parameters `NodeParam.CharFilter*` (93..98), self-described in `capabilities.effects` like every kind, so a UI can build its controls from the table and every continuous parameter takes the generic FX LFO stack.

| Param | Range | What it does |
|---|---|---|
| `CharFilterMode` | 0..3 (step) | 0 LP 24 dB (the main voice), 1 LP 12 dB, 2 BP, 3 HP 24 dB. Crossfaded over ~6 ms. |
| `CharFilterCutoff` | 30..18000 Hz | Smoothed in octaves (about 0.4 ms), so envelopes, LFOs and MIDI can sweep it fast. |
| `CharFilterResonance` | 0..1 | Compressive. Top ~12 % is self-oscillation: a tone tuned to the cutoff. |
| `CharFilterDrive` | 0..1 | 0..+24 dB into the saturating stages inside the filter. Output level is compensated. |
| `CharFilterCharacter` | 0..1 | Voicing: 0 Clean, 1/3 Warm, 2/3 Punchy, 1 Aggressive (interpolated). |
| `CharFilterEnvAmount` | -1..1 | An envelope follower on the input moves the cutoff up to 4 octaves (negative closes it). 0 = off. |

Defaults are cutoff 18 kHz, resonance 0, drive 0, character 0.4, env 0: a light colouring, not a bypass. Disable or remove the instance to bypass it. There is no dry/wet mix (the filter has ~0.3 ms of latency from its oversampler, and a parallel blend should be routed with a bus, not mixed out of phase).

**Inside an instrument**: use the primitive directly, one `CharacterFilter` per voice.

```cpp
CharacterFilter f;  f.configure(sampleRate);
f.setMode(0); f.setResonance(r); f.setDrive(d); f.setCharacter(c);   // 0..1, smoothed
float hz = CharacterFilter::trackedCutoff(baseHz, noteHz, 0.6f);      // keyboard tracking
hz = FilterModulation().cutoffHz(hz, depthOctaves, envelope);         // your own envelope
f.process(buf, n, hz);                                                // call with short blocks to modulate faster
```

The filter only filters. Envelopes, tracking and LFOs are computed by the caller with the mechanisms webdsp already has (`FilterModulation`, the instrument's ADSR, `ParamModulation`) and arrive as the cutoff. That is why the FX node's Env is an input follower (an effect has no note), and why Tracking is a helper and not an FX parameter (an effect has no pitch).

## What it is

Four trapezoidal (zero-delay) one-pole stages in a feedback loop, the ladder family, so the 24 dB low-pass is the natural voice and the other modes are pole mixes of the same stages (LP12 = pole 2, BP = y2 - 2 y3 + y4, HP = u - 4 y1 + 6 y2 - 4 y3 + y4). The loop runs 2x oversampled (31-tap half-band, Kaiser, ~75 dB stopband, flat to 17 kHz). Four things make it more than a clean filter with a distortion on top:

1. **The nonlinearity is inside the filter.** Every stage soft-saturates its own input (the repo's rational tanh, `MatrixFilter::soft`). Lows hit stage 1 hard; highs are already filtered out by stage 3. So how much it colours depends on level *and* on where the energy sits relative to the cutoff, which a post-filter distortion cannot do. Drive is the gain into those stages, not a separate effect.
2. **Resonance is compressive.** The feedback is soft-limited: a loud input pulls the resonant peak down, a quiet one lets it ring. At the top of the knob the limit sets the amplitude of a stable self-oscillation (a sine-ish tone, shaped rather than clipped).
3. **Asymmetry.** Character biases stage 1, adding even harmonics (warmth). It is re-centred so zero in is zero out, and the output DC blocker removes the rest.
4. **Bass keeping.** Character moves a high-pass in the feedback path. Low, resonance eats the low end like a classic ladder; high, the low end stays and only the peak is added. The high-pass's gain and phase at the cutoff are compensated analytically, so the self-oscillation stays on pitch and keeps its margin.

### The four voicings

| | stage hardness | even harmonics | feedback limit | bass kept | character |
|---|---|---|---|---|---|
| Clean | 0.22 | none | very loose | none | a very good linear-ish ladder: -50 dB THD at -6 dBFS |
| Warm | 0.75 | strong | soft | little | rounded, H2 about -28 dB at -8 dBFS |
| Punchy | 1.15 | some | tight | most | low end holds under resonance, H3 forward |
| Aggressive | 1.90 | slight | hard | some | odd harmonics, buzzy self-oscillation |

Per-voicing trims (`Voice` in the header) keep level within ~2 dB across drive and voicing, and set the self-oscillation level to a similar peak (~0.35 to 0.4 peak).

## Stability

Bounded by construction, not by hope: stage inputs are saturated, each stage update is a convex mix (|1 - 2G| < 1), the feedback is limited, and a final soft limit (transparent to 0.7, exactly +-1 beyond) bounds the output. Non-finite input is zeroed, non-finite parameters are ignored, and a non-finite state resets the filter. Fresh filter and silent input give exact zeros; a burst decays below 1e-4 within 1.5 s outside the self-oscillation zone (a 1e-8 float-rounding floor remains). A self-oscillating filter keeps ringing until the resonance is lowered; it starts from any input (a tick is enough), there is no noise seed, so an idle bus stays silent.

Cutoff, resonance, drive, character and mode are smoothed at a ~0.17 ms control rate; the cutoff coefficient is interpolated per sample inside each step. No allocation, no branching on parameters in the hot loop. About 0.7 % of one core per channel natively; stereo is two instances.

## Measured (test suite, 49k checks)

- Extremes: 4 modes x 3 resonances x 2 drives x 3 voicings x 4 cutoffs (20 Hz .. 1e9 Hz clamped) with full-scale noise and a square burst: always finite, |y| <= 1.
- Rapid cutoff modulation: cutoff alternating 30 Hz <-> 16 kHz every 8, 16, 64, 256 samples at resonance 1, every mode and three voicings: finite, bounded. Smooth full-range sweeps at resonance 0.8 leave no step above 0.25 on a 100 Hz sine.
- Response: LP 24 about -12 dB at the cutoff and -49 dB two octaves up; LP 12 half the slope; HP and BP mirror them. Passband stays within about 5 dB as resonance rises while the peak climbs to +13 dB. Cutoff is right from 250 Hz to 12 kHz.
- Self-oscillation: starts from a tick, sustains at 0.15..0.9 peak, **within 25 cents of the cutoff** for all voicings from 100 Hz to 6 kHz (worst case about 8 cents up to 2 kHz, about 23 at 8 kHz, on Punchy), at 22.05, 44.1, 48 and 96 kHz. Below ~0.86..0.90 it rings and dies.
- Character: a -6 dBFS 200 Hz sine through the open filter at the default voicing is -32 dB THD (audible colour) where the biquad is below -90 dB; 30 dB quieter the same filter is near-linear (the colour follows the level). Resonance compression: loud input lowers the peak by more than 3 dB against a quiet one, in every voicing from 0.4 up. Warm adds a second harmonic 40+ dB above Clean's; DC stays below 2e-3 in every voicing.
- Drive: THD rises monotonically; output level stays within 4.5 dB across the whole drive range in every voicing.
- Aliasing: a hard-driven 5 kHz sine at full Aggressive/drive shows its 7th-harmonic alias (13 kHz) more than 45 dB under the fundamental.
- Sources (the engine's own Bass, Kick, Snare, Hi-hat, FM, Noise, plus a saw and a plucked-string stand-in for a sample) at low/high resonance x low/high drive, LP and HP: finite, bounded, level-sane (printed table in the test output).

## Not done / open

- **Not auditioned.** Voicing tables, trims and the Character mapping are numerically sane and were tuned from measurements; they have not been tuned by ear. Expect to adjust the `Voice` table.
- The Env follower has fixed times (2 ms attack, 140 ms release). A decay control is the obvious next parameter if auto-wah use matters.
- Keyboard tracking exists as `CharacterFilter::trackedCutoff()` for instruments; no shipped instrument uses the Character Filter inside its voice yet. Bass, Matrix and Wobble keep their own filters unchanged.
- Aliasing is reduced (2x), not eliminated; extreme drive at high pitch still folds a little. 4x would double the cost for little gain on the intended material.
- The existing modulation test `modulation_test.cpp:138` (bit-identical render with zero-amount LFOs) fails on `main` without this change too; unrelated.
