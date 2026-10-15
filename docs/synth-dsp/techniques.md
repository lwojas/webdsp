# Techniques

Each entry: **Sound** · **Shape** (signal sketch) · **Cost** · **Reuse** (existing webdsp
primitives) · **Controls** (musical, suggested) · **Source**. Cost: cheap / moderate / expensive,
all per instance; "W" = good fit for AudioWorklet+WASM monophonic instances.
Sources are the originals where located. Controls and fit notes are recommendations.

Sections: [A Oscillators & anti-aliasing](#a-oscillators--anti-aliasing) ·
[B Modulation / phase](#b-modulation--phase-techniques) ·
[C Nonlinear / waveshaping](#c-nonlinear--waveshaping) ·
[D Resonators & physical models](#d-resonators--physical-models) ·
[E Analogue-inspired percussion](#e-analogue-inspired-percussion) ·
[F Other](#f-other) · [G Sustained & melodic](#g-sustained--melodic-voices)

---

## A. Oscillators & anti-aliasing

### 1. Polynomial BLEP / PolyBLEP (band-limited steps)
- **What:** Correct the discontinuities of naive saw/square/pulse with a short polynomial residual
  around each edge. Standard way to get cheap, clean classic waveforms.
- **Sound:** Bright, clean analogue-style saw/pulse; PWM and hard sync stay usable.
- **Shape:** `phase → naive wave + blep(phase, dt) at each discontinuity → out`.
- **Cost:** cheap. W, polyphony-safe. No oversampling needed.
- **Reuse:** `BiquadFilter` after it; `Saturation`. New: the oscillator (a few lines).
- **Controls:** Tune, Pulse width, Sub level, Sync/Detune.
- **Source:** Välimäki & Huovilainen, *Antialiasing oscillators in subtractive synthesis*,
  IEEE SPM 2007; overview in Pekonen/Välimäki, *Oscillator and filter algorithms for virtual
  analog synthesis*, CMJ 2006 (both via [CCRMA/Aalto pubs](https://ccrma.stanford.edu/~juhan/pubs/Pekonen2010ICGCS.pdf) index).

### 2. Differentiated polynomial waveforms (DPW)
- **What:** Generate a parabola/higher-order polynomial from phase, differentiate once or more in
  the digital domain: aliasing is attenuated without edge detection.
- **Sound:** Same as BLEP saw; very smooth at low pitches, mild brightness loss/noise at high.
- **Shape:** `phase → x² or cubic → differentiate (1–3×) → scale by 1/(4·f/fs)`.
- **Cost:** cheap. W. Per-sample branchless; handy if pitch is modulated fast (kicks, FM).
- **Reuse:** none needed. Pairs with sweeps (Kick's pitch envelope).
- **Controls:** Tune, Sweep, Detune.
- **Source:** Välimäki, Nam, Smith, Abel, *Alias-suppressed oscillators based on differentiated
  polynomial waveforms*, IEEE TASLP 2010 ([PDF](https://mac.kaist.ac.kr/pubs/jnam-taslp2010.pdf)).

### 3. Phase distortion (Casio CZ style)
- **What:** A phase accumulator is warped by a non-linear map before a sine/cos lookup; one "DCW"
  control morphs sine → saw/square/pulse-ish shapes and, with a resonant window, formant-like peaks.
- **Sound:** Digital-analogue hybrid, bright plucky bass and bell-ish leads, distinct from FM.
- **Shape:** `phase → warp(phase, amount) → sin → × amp env`; amount follows its own envelope.
- **Cost:** cheap. W.
- **Reuse:** `AdsrEnvelope` for amount; `Saturation` for weight.
- **Controls:** Tune, Brightness (amount), Pluck (amount decay), Decay.
- **Source:** Casio CZ, 1984 ([overview](https://en.wikipedia.org/wiki/Phase_distortion_synthesis));
  no key academic paper: treat as folklore + your ears. *Recommendation:* strong candidate for a bass.

---

## B. Modulation / phase techniques

### 4. FM / PM with decaying modulator (Chowning, DX-style)
- **What:** Modulate the carrier phase by another oscillator; the modulation index envelope
  shapes spectral richness over time. Phase modulation is the numerically friendlier form.
- **Sound:** Bell, electric piano, metallic hits, punchy FM bass, inharmonic clang at non-integer ratios.
- **Shape:** `mod osc(ratio·f) × index·env → carrier phase → sin → amp env`.
- **Cost:** cheap (2–3 sines). W.
- **Reuse:** `AdsrEnvelope`, `Saturation`.
- **Controls:** Tune, Brightness (index), Ratio/Inharmonicity, Decay, Snap (index decay).
- **Source:** Chowning, *The Synthesis of Complex Audio Spectra by Means of Frequency Modulation*,
  JAES 1973. Aliasing at high index: see #9.

### 5. Loopback (feedback) FM
- **What:** The oscillator phase-modulates itself with its previous output; a time-varying
  feedback coefficient yields frequency trajectories resembling nonlinear acoustic percussion
  (tension modulation of gongs, toms).
- **Sound:** Rich, evolving, slightly unstable metallic/drum tone from a single oscillator;
  noise-like at high feedback.
- **Shape:** `y[n] = sin(phase + β(t)·y[n-1])`, β decays; sum of several tuned to modes.
- **Cost:** cheap (one sine per mode). W. Few modes = few controls.
- **Reuse:** `Saturation`, `BiquadFilter` (band-limit), envelopes.
- **Controls:** Tune, Strike (β start), Decay, Tension (pitch glide), Metal.
- **Source:** Hsu & Smyth, *Percussion Synthesis using Loopback FM Oscillators*, SMC 2019
  ([PDF](https://www.smc2019.uma.es/articles/S5/S5_05_SMC2019_paper.pdf),
  [sound examples](https://trsmyth.ucsd.edu/other/percussionSynthesisLoopbackFM.html)).

---

## C. Nonlinear / waveshaping

### 6. Waveshaping (memoryless) with dynamic drive
- **What:** Pass a simple signal through a nonlinear function (tanh, polynomial, Chebyshev).
  Input level controls harmonic content; Chebyshev polynomials give selected harmonics.
- **Sound:** Everything from warm saturation to buzzy/aggressive; level-dependent brightness.
- **Shape:** `sine·drive_env → shaper → DC block → trim`.
- **Cost:** cheap. W. Aliases when drive is high (see #9).
- **Reuse:** `Saturation` (already tanh + asymmetry + DC blocker); Kick uses it.
- **Controls:** Drive, Bite (envelope on drive), Asymmetry/Warmth.
- **Source:** Standard; see Pekonen/Välimäki CMJ 2006 above and Arfib, *Digital synthesis of
  complex spectra by means of multiplication of non-linear distorted sine waves*, JAES 1979.

### 7. Wavefolding (Buchla / Serge style)
- **What:** Fold the waveform back on itself above a threshold; folds multiply harmonics with a
  vowel-ish, non-monotonic timbre as the input grows.
- **Sound:** Glassy, metallic, "west-coast"; sub-bass with sparkling top.
- **Shape:** `sine/tri → gain env → fold stages (parallel) → out`.
- **Cost:** cheap–moderate with antialiasing (#9). W.
- **Reuse:** `Saturation` after; envelopes drive fold depth.
- **Controls:** Tune, Fold, Fold decay (strike), Symmetry.
- **Source:** Esqueda, Pöntynen, Välimäki, Parker, *Virtual Analog Buchla 259 Wavefolder*, DAFx-17
  ([DAFx](https://dafx.de/paper-archive/details/zmNQ8s5sZVhM_ObMB_D8tg)); Lockhart/Serge in
  [Esqueda et al. 2017](https://acris.aalto.fi/ws/portalfiles/portal/16820117/esqueda_et_al_applsci_07_01328_v2_1.pdf).

### 8. Feedback with nonlinearity / self-oscillation
- **What:** A delay or filter loop containing a saturator; at high gain it self-oscillates and
  settles at a level-dependent pitch/timbre (analogue-filter-ping, Karplus-with-drive).
- **Sound:** Unstable, living, aggressive or "singing"; strongly dependent on excitation.
- **Shape:** `impulse/noise → [filter or delay] → tanh → feedback gain → loop`.
- **Cost:** cheap. W. Requires care to stay bounded (tanh in loop; output limit).
- **Reuse:** `BiquadFilter`, `Delay`, `Saturation`.
- **Controls:** Pitch, Ring (feedback), Grit, Excite.
- **Source:** Huovilainen, *Non-linear digital implementation of the Moog ladder filter*,
  DAFx-04 (for nonlinear loops); general waveguide theory in Smith, PASP (see #11).
  *Recommendation:* safe starting point for a resonant "ping" bass or zap.

### 9. Anti-aliased nonlinearities (ADAA / BLAMP)
- **What:** Instead of oversampling, evaluate the nonlinearity's *antiderivative* across each
  sample pair (first-order ADAA) or add BLAMP residuals for folders/clippers.
- **Sound:** Same waveshaping, with far less inharmonic fizz at high drive/pitch.
- **Shape:** `y[n] = (F(x[n]) − F(x[n−1])) / (x[n] − x[n−1])`, with fallback when Δx≈0.
- **Cost:** cheap (≈ 1 extra function eval). W. **Not present in webdsp's `Saturation`.**
- **Reuse:** would be new code; instrument-private first.
- **Controls:** none (it is quality, not a control).
- **Source:** Parker, Zavalishin, Le Bivic, DAFx-16
  ([DAFx](https://dafx.de/paper-archive/details/vem_XXF5qBbfiWOH2RVVAA)); practical note in
  [Vicanek, "Anti-aliased distortion"](https://www.vicanek.de/articles/AADistortion.pdf).

---

## D. Resonators & physical models

### 10. Modal synthesis (resonator bank)
- **What:** Model an object as N damped sinusoids (modes): each with frequency, decay and
  gain. An excitation (click, noise burst) rings them. Implement as parallel 2-pole resonators.
- **Sound:** Struck wood, metal bars, bells, marimba, membrane toms; highly "physical".
- **Shape:** `excitation (impulse·velocity) → N × resonator(f_i, T60_i, g_i) → Σ`.
- **Cost:** cheap per mode (~5 flops); 8–32 modes fine. W. Pre-allocate the bank.
- **Reuse:** `BiquadFilter` (band-pass) as mode, or new 2-pole sine oscillator (cheaper, exact).
- **Controls:** Tune, Material (decay vs frequency tilt), Size/Stretch (inharmonicity), Strike (position/hardness), Damping.
- **Source:** Bilbao, *A Modular Percussion Synthesis Environment*, DAFx-09
  ([DAFx](https://dafx.de/paper-archive/details/EYnht3o7b7CqKfo491X2Vg)); Skare & Abel, *Real-Time Modal Synthesis of Crash Cymbals…*, DAFx-19
  ([DAFx](https://dafx.de/paper-archive/details/qe8SzTmE_uIgihtLswWFFA)) for the nonlinear idea.
  Mode tables for bars/plates: any acoustics text (e.g. Fletcher & Rossing).

### 11. Karplus-Strong / digital waveguide string
- **What:** A delay line of length fs/f with a lowpass in the feedback loop, excited by noise
  burst; the loop filter makes high partials decay faster, like a real string.
- **Sound:** Plucked string, pluck-bass, harp, muted pluck; with drive: nasty and fat.
- **Shape:** `noise burst → delay(N) → loop lowpass (damping) → ×g → delay`; fractional-delay
  allpass for tuning.
- **Cost:** cheap. W. Memory = fs/f_min samples, allocate once.
- **Reuse:** `Delay` (check fractional-delay support), `BiquadFilter`, `Saturation`.
- **Controls:** Tune, Pluck position/brightness, Decay, Damping, Drive.
- **Source:** Karplus & Strong, *Digital synthesis of plucked-string and drum timbres*, CMJ 1983;
  Jaffe & Smith, *Extensions of the Karplus-Strong plucked-string algorithm*, CMJ 1983;
  [Smith, PASP §Karplus-Strong](https://ccrma.stanford.edu/~jos/pasp/Karplus_Strong_Algorithms.html).

### 12. Commuted synthesis / excitation impulse
- **What:** Fold a (long) body resonance into the excitation signal so the instrument is
  "excitation sample → string model". For webdsp: a *synthesized* short excitation.
- **Sound:** Realistic body resonance for guitar-like/pianos without a large filter bank.
- **Cost:** moderate; needs a stored/generated excitation. **Not currently a fit** (no data assets in instruments).
- **Source:** Smith, PASP "Commuted Synthesis" (CCRMA). *Recommendation:* skip until needed.

### 13. Nonlinear tension modulation (gong/tom pitch glide)
- **What:** Make a mode's frequency depend on its current amplitude: loud = sharp, drops as it
  decays. Cheap approximation of the nonlinear plate/membrane behaviour.
- **Sound:** Tom/gong "bwoom", kick-tone drop with natural character, thunder-like crash.
- **Shape:** `f_i(t) = f_i·(1 + k·env_i(t))`; env_i is the mode's own amplitude estimate.
- **Cost:** cheap. W.
- **Reuse:** the Kick pitch-envelope approach, now per mode.
- **Controls:** Tune, Tension (k), Decay.
- **Source:** concept in Skare & Abel (#10) and Hsu & Smyth (#5); Bilbao's nonlinear plate work
  explains why. *Recommendation:* implement as a single parameter on a modal bank.

---

## E. Analogue-inspired percussion

### 14. Bridged-T / ringing-filter kick (TR-808 style)
- **What:** The 808 kick is a trigger pulse exciting a high-Q band-pass-like resonator with
  nonlinear shaping and a tiny pitch drift, not a swept oscillator.
- **Sound:** Round, long, rubbery sub with natural "settling" and a defined click.
- **Shape:** `trigger pulse → (shaper) → resonant 2-pole @ f0 → soft clipper → amp`.
- **Cost:** cheap. W.
- **Reuse:** `BiquadFilter` (high Q), `Saturation`; the existing Kick is the swept-oscillator variant.
- **Controls:** Tune, Decay, Click, Warmth.
- **Source:** Werner et al., *A Physically-Informed, Circuit-Bendable, Digital Model of the Roland
  TR-808 Bass Drum Circuit*, DAFx-14
  ([DAFx](https://www.dafx.de/paper-archive/details/hwkd1Lfe8vmtOxCEjsNY-g)). Simplify: behavioural resonator + shaper, skip the circuit solver.

### 15. Metallic square-wave cluster (808 hat / cymbal)
- **What:** Six square oscillators at fixed inharmonic frequencies (~200–800 Hz base),
  summed, band-passed then high-passed, short or long envelope.
- **Sound:** Classic closed/open hat, cowbell (two squares), clap-adjacent metal.
- **Shape:** `6× square → Σ (or XOR/ring) → BP → HP → env → out`.
- **Cost:** cheap. W (aliasing at 6 squares is audible but characteristic; PolyBLEP optional).
- **Reuse:** `BiquadFilter` (2×), `AdsrEnvelope`, `Bitcrusher`.
- **Controls:** Tune, Decay, Tone, Choke/open.
- **Source:** Folklore/service-manual level (TR-808 schematics);
  summarised in [Sound On Sound drum synthesis](https://www.soundonsound.com/techniques/practical-snare-drum-synthesis).
  Treat the frequency set as a starting point, then tune by ear.

### 16. Snare = two tone modes + filtered noise
- **What:** Two decaying pitched modes (~1:2 ratio) with a pitch drop, plus band-passed noise
  with its own envelope; a short "snap" at the start.
- **Sound:** Snappy, punchy; from tight electronic to fuller acoustic by noise/tone balance.
- **Shape:** `tone1,tone2 (pitch env) + noise→BP→env → mix → Saturation`.
- **Cost:** cheap. W.
- **Reuse:** Kick internals (sine+pitch env), `BiquadFilter`, `Saturation`.
- **Controls:** Tune, Snap, Tone/Noise balance, Decay, Drive.
- **Source:** TR-808 descriptions (SoS above); Karplus-Strong's original paper also covers drum timbres.

---

## F. Other

### 17. Noise-excited resonant bandpass (blown/scraped/rattle)
- **What:** Noise or random impulses into a few high-Q resonators; excitation envelope and
  random rate shape "breath", "shaker", "rain" textures.
- **Sound:** Shakers, maracas, chimes, wind-ish pads.
- **Shape:** `noise × burst env → N BP resonators → Σ`.
- **Cost:** cheap. W.
- **Reuse:** `BiquadFilter`, deterministic xorshift noise from Kick.
- **Controls:** Pitch, Shake, Decay, Brightness.
- **Source:** Standard modal/subtractive; see #10.

### 18. Friction / bowed excitation
- **What:** A nonlinear stick–slip function between a bow velocity and a resonator (string or
  mode bank) sustains oscillation.
- **Sound:** Bowed, singing, scratchy; "friction drum"-like when applied to membrane modes.
- **Shape:** `bow velocity → friction curve(v_rel) → resonator → feedback v_rel`.
- **Cost:** moderate; stability tuning needed. W if kept to string/bank.
- **Reuse:** #10/#11 resonators.
- **Controls:** Pressure, Speed, Pitch.
- **Source:** Smith, PASP (bowed strings, CCRMA); friction-drum finite-difference model at
  DAFx (see [DAFx20in21 paper 32](https://vbn.aau.dk/files/445885379/DAFx20in21_paper_32.pdf)),
  too heavy as-is. *Recommendation:* secondary; only as a simplified loop.

### 19. Virtual-analogue drift and instability
- **What:** Slow, bounded random modulation of pitch and amp (and filter) per note; per-note
  random detune on stacked oscillators.
- **Sound:** "Alive", less sterile; reduces machine-gun repetition.
- **Cost:** negligible. W. Must be deterministic: seed from a per-instance counter, not a clock.
- **Reuse:** the Kick xorshift noise; one-pole smoothing.
- **Controls:** Age/Drift (single knob).
- **Source:** folklore; see Pekonen/Välimäki CMJ 2006 for oscillator drift modelling.

### 20. Spectral/analysis-resynthesis (RDFT-encoded drum)
- **What:** Encode a drum's character in one real spectrum and synthesise by manipulating it;
  morph between drum characters.
- **Sound:** Realistic drums from few parameters; morphing between kits.
- **Cost:** expensive / data-dependent. Poor fit today (needs analysis data and FFT).
- **Source:** Kirby & Sandler, *Advanced Fourier Decomposition for Realistic Drum Synthesis*,
  DAFx-20 ([DAFx](https://dafx.de/paper-archive/details/bIYhP4OjdM919GayyFkU-A)).
  *Recommendation:* academically interesting, not worth pursuing now.

---

## G. Sustained & melodic voices

Added after the first pass, which leaned percussive. These suit note-held instruments. Engine
reminders: one instance is monophonic; chords = several instances; `instrumentNoteOff` should
trigger a short click-free release (the contract already says so); retriggers must be continuous.
Sustained instruments also need **slow-modulation determinism** (LFO phase reset on note-on).

### 21. Detuned unison / supersaw
- **What:** 5–9 band-limited saws with a nonlinear detune spread (small near centre, larger outside),
  random start phases, centre oscillator louder; HP-filtered. Szabo reverse-engineered the
  JP-8000's detune/mix curves.
- **Sound:** Wide, shimmering trance/pad/lead stack; "huge" from one note.
- **Shape:** `N × PolyBLEP saw (detune_i, phase_i) → mix(centre vs side) → HP → LP → amp env`.
- **Cost:** moderate (N × #1). W; fine for one instance, avoid >7 voices per instance.
- **Reuse:** #1 oscillator, `BiquadFilter`, `AdsrEnvelope`, `Phaser`/chorus downstream.
- **Controls:** Tune, Spread (detune), Mix (centre vs sides), Brightness, Attack, Release.
- **Source:** Szabo, *How to Emulate the Super Saw* (MSc thesis, 2010; not located online here:
  search the title). Phases must be seeded deterministically (not random per run).

### 22. Hard sync / sync sweep
- **What:** A slave oscillator is reset by a master; sweeping the slave's pitch gives the
  vocal, ripping "sync lead" sweep. Needs BLEP-corrected resets to avoid heavy aliasing.
- **Sound:** Aggressive, harmonically sweeping lead/bass; classic mono-synth.
- **Shape:** `master phase wrap → reset slave phase (+BLEP) ; slave saw/pulse → amp env`.
- **Cost:** cheap–moderate. W.
- **Reuse:** #1; envelope on slave pitch (like Kick's pitch env).
- **Controls:** Tune, Sync (ratio/sweep), Sweep decay, Pulse.
- **Source:** Brandt, *Hard Sync Without Aliasing*, ICMC 2001 (cited by title; not linked);
  caveat: BLEP only approximates sync of non-saw shapes.

### 23. Bandlimited wavetable oscillator (mipmapped)
- **What:** Store single-cycle waves per octave (harmonics cut at Nyquist for that octave),
  read with interpolation, crossfade between tables for morphing.
- **Sound:** Evolving, digital-organic timbres; morph between shapes.
- **Shape:** `phase → table[mip(f)][morph] (interp) → amp env`.
- **Cost:** cheap. W. Tables generated at `configure()` from formulas (additive build), so no
  data assets.
- **Reuse:** none for the oscillator; envelopes/filters as usual.
- **Controls:** Tune, Morph, Motion (morph LFO/env), Brightness.
- **Source:** practical writeup in [musicdsp "bandlimited" notes](https://musicdsp.readthedocs.io/en/latest/_downloads/91daec88cb0d79aeb1c21d8afd4e3199/bandlimited.pdf); theory in
  Pekonen/Välimäki CMJ 2006. *Recommendation:* build tables from harmonic recipes at init.

### 24. Additive / drawbar organ
- **What:** Sum 6–16 sine partials at harmonic ratios with per-partial level; organ drawbars
  are 9 fixed ratios. Per-partial envelopes enable bell/pad/vocal-ish shapes.
- **Sound:** Organ, glassy pads, evolving harmonics; very stable pitch.
- **Shape:** `Σ_k a_k·sin(k·ω·t) → click/key env → leakage/chorus → out`.
- **Cost:** cheap–moderate (N sines; use recurrence oscillators). W.
- **Reuse:** `Saturation` (tonewheel/tube grit), `Phaser`/chorus downstream.
- **Controls:** Tune, Drawbars (a small set: Sub/Body/Bright), Click, Vibrato, Drive.
- **Source:** classic additive theory; analysis-based variant in Serra & Smith, *Spectral Modeling
  Synthesis*, CMJ 1990 (cited by title; analysis not needed here).

### 25. Formant / vocal synthesis (FOF)
- **What:** An impulse train drives formant-wave functions: short exponentially damped sinusoids
  per formant, retriggered each pitch period. Vowels = a table of 3–5 formant centre/bandwidth/level.
- **Sound:** Choir "ahh", vowel pads, talky leads/basses, vowel-morph.
- **Shape:** `pitch → impulse train → formant filters or FOF grains (F1..F4) → Σ → amp`.
- **Cost:** moderate. W. Compare cheaper alternative: saw → 3 parallel `BiquadFilter` band-passes.
- **Reuse:** `BiquadFilter` band-passes, #1 saw as source, `Phaser`.
- **Controls:** Tune, Vowel (morph), Breath (noise), Vibrato, Brightness.
- **Source:** Rodet, *Time-domain formant-wave-function synthesis*, CMJ 1984 (cited by title);
  reference implementation overview in the [Csound FOF manual](https://csound.com/manual/fof.html).
  *Recommendation:* start with the 3-bandpass-over-saw version; FOF only if you need grain-clean timbre.

### 26. Bowed string (bow-table waveguide)
- **What:** A waveguide string (#11) with a nonlinear *bow junction*: a table maps
  velocity difference to reflection coefficient. Bow speed/pressure sustain and shape the tone.
- **Sound:** Cello/violin-like sustained, scratchy-to-smooth, with natural onset transients.
- **Shape:** `bow velocity → bow table(v_bow − v_string) → two delay lines (string halves) →
  reflection ← body/loop filter → out`.
- **Cost:** moderate. W. Tuning for stability is fiddly; the simplified piecewise-linear table
  is enough.
- **Reuse:** `Delay`, `BiquadFilter` loop filter, `Saturation`.
- **Controls:** Pitch, Pressure, Bow speed, Brightness, Attack.
- **Source:** Smith, *Digital Waveguide Modeling of Bowed Strings*
  ([PDF](https://ccrma.stanford.edu/~jos/BowedStrings/BowedStrings.pdf)); real-time efficiency in
  Jaffe & Smith 1995 ([PDF](https://ccrma.stanford.edu/%7Ejos/pdf/JaffeSmith95.pdf)).

### 27. Subtractive voice with nonlinear (Moog-style) filter
- **What:** Saw/pulse into a resonant 4-pole low-pass with tanh saturation per stage; filter
  envelope + keytracking; at high resonance it self-oscillates (#8).
- **Sound:** Fat classic mono bass/lead; level-dependent resonance squash.
- **Shape:** `osc(#1) → ladder LP (res, drive) → amp env`.
- **Cost:** moderate (4 stages + tanh; zero-delay-feedback form is more robust). W.
- **Reuse:** `Saturation`, `AdsrEnvelope`; existing `BiquadFilter` is a linear 2-pole, so a
  ladder is new code (instrument-private first).
- **Controls:** Tune, Cutoff, Resonance, Env amount, Drive, Decay.
- **Source:** Huovilainen, *Non-linear digital implementation of the Moog ladder filter*,
  DAFx-04 (cited by title). *Recommendation:* keep the filter inside the instrument.

### 28. Slow-modulation "life" (vibrato, PWM, ensemble)
- **What:** Per-note LFO with delayed onset on pitch (vibrato) and/or pulse width; optional
  stereo detuned doubling.
- **Sound:** Warmth, motion; makes sustained notes feel played.
- **Cost:** negligible. W. LFO phases reset on note-on for determinism.
- **Reuse:** per-instrument; `Phaser`/chorus downstream if wanted.
- **Controls:** Vibrato depth, Vibrato delay, Width.
- **Source:** folklore; see Pekonen/Välimäki CMJ 2006.
