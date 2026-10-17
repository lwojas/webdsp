# FM synth: design proposal

Status: decided 2026-10-17, implemented as `InstrumentKind.Fm`. Shipped behaviour is in
[`fm-instrument.md`](./fm-instrument.md). This file keeps the decision trail.

Inputs: [`synth-dsp/techniques.md`](./synth-dsp/techniques.md) #4 (FM/PM with decaying modulator,
Chowning), #5 (loopback FM), #3 (phase distortion), [`synth-dsp/instruments.md`](./synth-dsp/instruments.md)
("PD/FM Bass", "#4 FM/PM with index envelope: bells, e-piano, FM bass"),
[`bass-instrument-proposal.md`](./bass-instrument-proposal.md) (Candidate 2, "Warp Bass", was the FM-ish
road not taken), [`filter-modulation.md`](./filter-modulation.md), [`adsr-envelope.md`](./adsr-envelope.md),
`instrument.h`, `instruments/bass.h`, `instruments/pulse.h`, `instruments/pad.h`.

## A. What the research says

- Technique #4: `mod osc(ratio*f) x index*env -> carrier phase -> sin -> amp env`. Cheap, "punchy FM
  bass", controls: tune, brightness (index), ratio, decay, snap (index decay). Phase modulation is the
  numerically friendly form (no integrated-frequency drift, exact in the phase domain).
- Technique #5: a self-modulating oscillator gives a sawtooth-ish, denser spectrum from one sine;
  noise-like at high feedback.
- The Bass proposal rejected phase-warp/FM as the *first* bass because "the filter becomes a minor tone
  control" and it is weaker for analogue-style lines. That is the argument *for* FM as a separate
  instrument: it is the one that does what the subtractive Bass cannot.
- Engine constraints: mono per instance, no allocation in `render`, deterministic fresh note,
  continuous retrigger, no oversampling infrastructure. Reusable: `AdsrEnvelope`, `BiquadFilter`,
  `FilterModulation` (gated cutoff updates), `PercCore`, legato/glide approach from Bass.

## B. What FM adds that Bass, Pulse, Pad and the drums cannot do

| Existing | Mechanism | What it cannot do |
|---|---|---|
| Bass | saw+sine+sub, shaper, **filter** | Brightness is *removal* from a fixed rich source: a closing low-pass. Cannot create new, non-harmonic or hollow (odd-only) spectra, or a sine-pure sub that suddenly speaks with a bright, glassy edge. |
| Pulse | detuned saws, filter, pump | Same: subtractive, rhythmic. |
| Pad | saw ensemble, filter | Subtractive, slow. |
| Drums | one-shots | not sustained. |

FM generates a spectrum **additively, per pitch, from one control** (modulation index). Consequences
that are the instrument's identity:

1. **Brightness without a level change.** PM does not add energy as the index rises, it redistributes
   it (measured: RMS within about +-3 dB over the whole timbre range, versus a low-pass, which can
   only remove). Timbre and loudness are close to independent.
2. **Hollow / odd-harmonic and inharmonic colours** (ratio 2, 3, 1.41, 3.5): clarinet-ish, glassy,
   metallic, bell-like. A low-pass cannot make these.
3. **Index envelope = the DX attack.** A bright, aggressive, sideband-rich onset that settles into a
   clean tone. Bass does this with a filter; FM does it with new partials.
4. **Velocity-brightness** is natural: harder = more sidebands.
5. **Digital-clean sub bass** with a controllable "slap" and lead tones with glassy sparkle.

## C. Candidates

### A. 2-operator PM, sine/sine, no filter
`mod -> carrier -> amp env`. 2 sines/sample, state = 2 phases + 2 envelopes. Controls: ratio, index,
snap, ADSR, tune. Easy, cheapest, very clean. Spectrum follows Bessel functions with the known weakness
that at integer ratio the carrier fundamental nulls at index ~2.4/5.5/8.7 (a bass "disappears" mid-sweep).
Bass: good, plucky. Lead: thin, "plasticky"; no way to tame fizz at high index.

### B. 3-operator stack `mod -> mod -> carrier` (or 2 mods parallel)
3 sines/sample. Much richer, more DX-like; two ratios + two indices means 4+ musical parameters that
need operator knowledge (what is ratio 2 *of*?). Hard to give one-knob timbre. Rejected: this is the
step toward the "operator workstation" the brief forbids and one extra sine buys little over feedback.

### C. 2-operator PM + modulator self-feedback + static tone filter (chosen)
`modulator(+internal feedback) -> carrier -> tone LP -> amp env`. 2 sines + 1 biquad per sample.
- Feedback on the modulator turns it into a smooth sawtooth-like phase source, so the spectrum is denser
  and smoother. This is the DX "feedback" trick, used internally, not exposed. (Measured: it does
  **not** cure the fundamental nulls; see "Findings" below for what does.)
- A 12 dB low-pass, keytracked to the *fundamental*, tames the top sidebands. It is a static tone
  control, not an envelope target: the index envelope already does what a filter envelope does for
  subtractive synths, so `filterMod` is deliberately not adopted (see Decisions).
- Controls: tune, ratio (stepped), timbre, snap, tone, ADSR, legato, slide.

| | A | B | C (chosen) |
|---|---|---|---|
| Ops / sines per sample | 2 | 3 | 2 (+1 biquad) |
| Voice state | 2 phases, 2 env | 3 phases, 3 env | 2 phases, 2 env, 1 filter, 1 feedback sample |
| Bass | good, fundamental dips | rich, hard to control | strong fundamental, aggressive without noise |
| Lead | thin | excellent but complex | glassy to sawish; tone tames fizz |
| Controls a user must understand | ratio, index | 2 ratios, 2 indices, routing | ratio (a colour switch), timbre, snap |
| Complexity vs Kick (~150 lines) | S | L | M |
| WASM suitability | excellent | good | excellent |

## D. Decisions (recommendation: C)

- **Frequency ratio:** `modulator = carrier x ratio`, always derived from the live carrier pitch (so it
  stays correct across the keyboard, tune, `pitch`, legato and glide). Public control: a **stepped
  selector of 8 musical ratios**, not a continuous knob: `0.5, 1, 2, 3, 4, 5, 1.414, 3.5`.
  Integer ratios keep the sound harmonic (every partial on a multiple of the note); a continuous
  ratio is mostly out-of-tune clang. Sidebands sit at `f(1 +- k r)`, so: **1** all harmonics, the
  saw-like bass/lead workhorse; **2** and **4** odd harmonics only (hollow, clarinet/square-ish);
  **3** all but every third harmonic; **5** all but every fifth; **0.5** a sub-octave growl;
  **1.414 / 3.5** inharmonic (metallic, bell).
- **Modulation index:** never exposed raw. `timbre` (resting index, 0..1, curved so the low end is
  fine-grained) and `snap` (extra index at note start that decays). Index is in radians of peak phase
  deviation; max about 5 resting plus 7 snap. **Bandwidth guard:** the effective index is limited so
  `(index + 1) x modulatorHz < 0.4 fs` (Carson), which makes high notes and high ratios alias-safe at
  any sample rate without oversampling. At 0 index the output is exactly the carrier sine.
- **Envelope-controlled FM:** a second, internal `AdsrEnvelope` (no extra public ADSR): attack = amp
  attack, decay = 0.35 x public decay (clamped), sustain 0, release = amp release. `snap` scales how
  much of it is added to the resting `timbre`. Amplitude uses the existing Bass shape (squared, sqrt
  sustain). Velocity scales both index terms (always on: harder = brighter).
- **Feedback:** fixed, internal, on the modulator only, proportional to the effective index
  (0 at index 0, so zero-index stays a pure sine). Averaged over two samples (DX style) for stability.
- **Waveforms:** sine only. Sidebands stay predictable and a "waveform" param would turn this into a
  general oscillator.
- **Filter:** 12 dB biquad LP after FM, cutoff = fundamental x 2^(1 + 7 tone): always relative to the
  note so brightness is pitch-invariant; `tone` = 0 leaves essentially the fundamental. It is a static
  shaper: no envelope depth (that is the index envelope's job) and therefore no `filterMod`.
- **Legato/slide:** same semantics as Bass (`legato` >= 0.5: a note on a held note continues the voice,
  no re-attack, held velocity, optional linear-in-semitone `slide`).
- **Public set (11):** `tune, ratio, timbre, snap, tone, attack, decay, sustain, release, legato, slide`.
- **Category:** `synth`.

## E. Not built (and why)

3rd operator, operator envelopes/levels, algorithm select, waveform select, fixed-frequency modulator,
LFO, `filterMod`, unison/detune, a shared FM "operator" class. Each can be added later by a real need;
none is needed for the two target sounds.

## F. Findings during implementation (measured, and what changed)

1. **The fundamental really does vanish.** At most ratios the carrier fundamental follows `J0(index)`
   and sits at -30 dB around index 2.4 and 5.5; the snap envelope sweeps through that on every note.
   Modulator feedback does not fix it. Fix: at low pitch a fixed share of the clean carrier is blended
   in (0.5 at/below 80 Hz, fading to 0 at 320 Hz, normalised so index 0 is still an exact sine). Ratio 1
   then never falls more than ~6 dB under the plain carrier at 55 Hz. Other ratios still thin at
   particular timbre values (that is FM, and part of its colour); the default ratio is 1 for this reason.
2. **Pitch jumps click through a keytracked filter.** A note landing on a sounding or releasing voice
   moves the pitch a fifth in one step; the biquad's coefficient jump produced a +-0.1 spike on bright
   FM content (caught by the retrigger test). The tone filter therefore follows the pitch at 1 semitone
   per 16-sample sub-block, and the bass-weight share is ramped per sample like the index.
3. **Loudness is only roughly constant.** For integer ratios the phase relationship of carrier and
   modulator is locked, so RMS varies by a few dB with the index (measured -4..+2 dB). Accepted.
4. **"Activity" metrics are not monotone in the index** (energy re-concentrates near the fundamental at
   some indices); tests use targeted measures instead of "more is brighter".
