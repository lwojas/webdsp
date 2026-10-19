# Digital Machines

Five FM instruments (category `digital`) built on one shared kernel: **Thump**, **Metal**, **Tone**,
**Crack**, **Chaos**. Source: `native/src/dsp/fm_kernel.h`, `native/src/instruments/digital_machine.h`,
`native/src/instruments/dm_*.h`; tests: `native/test/dm_test.cpp`.

## Idea

Elektron-style *Machines*, not presets: each Machine is a deliberately designed topology plus a small
set of semantic controls, built from the same FM parts, and each is **particularly good at** one
territory while being able to leave it. All of it is digital/FM — no analogue modelling.

| Machine | Kind id (`InstrumentKind`) | Territory | Can also be |
|---|---|---|---|
| **Thump** | `dm_thump` (11) | kick, tom, tuned percussion, FM bass hit | zap / laser, impact, long bass tone |
| **Metal** | `dm_metal` (12) | hat, cymbal, clang, bar, bell | pure inharmonic bell (noise 0), metallic percussion |
| **Tone** | `dm_tone` (13) | 80s pluck, FM bass, keys, bell, lead, chord stab | tom-like hit, organ (`hold` 1) |
| **Crack** | `dm_crack` (14) | snare, clap, rim, noisy percussion | pitched noise tone (noise only through FM) |
| **Chaos** | `dm_chaos` (15) | unstable digital sounds, metallic lead, strange hits | clean slightly-inharmonic tone (chaos 0) |

The existing `Fm` (kind 8) stays the bass/lead voice (two-operator, stepped ratios, low-pass `tone`,
legato/slide). The Machines deliberately do not do legato/slide, have no in-voice filter, use
continuous "magnetic" ratios (below), up to four operators, exponential percussive envelopes and a
pitch envelope, and can put noise inside the FM path. Bus FX (filter, saturation, bitcrusher, delay,
reverb) already exist per track, so the voice has none of its own.

## Shared kernel (`fm_kernel.h`)

- `SineTable` — one read-only 2048-point table, linear interpolation (error ~1.2e-6 = -118 dB), built
  once in `configure()` (message thread). `FmOp` = phase accumulator read through it.
- `ExpEnv` — short linear attack, one-pole decay toward a sustain floor (0 = pure decay that ends itself),
  one-pole release. `trigger()` climbs from the current level (continuous retrigger); `start(level)` jumps
  (pitch envelopes). Used for amplitude, index, pitch and click envelopes.
- `magneticRatio` / `magneticPos` — **magnetic ratios**: a 0..1 control over a table of curated ratios,
  with a plateau around each entry (60% of the travel is exactly the curated ratio) and a geometric
  glide in between. Sweet spots to rest on, out-of-tune clang to sweep through (an LFO on `ratio` is
  musical). The existing `Fm` uses stepped ratios; this is a deliberate difference.
- `maxIndex` / `nyquistGain` — the **bandwidth guard**, generalised from `fm.h`: a modulator at `f` with
  index `I` is limited so `(I + 1) f < 0.4 fs`; an operator above ~0.4 fs fades out instead of aliasing.
  Feedback and cascaded modulators count toward a link's bandwidth.
- `Svf` (trapezoidal state-variable filter), `Drive` (gain + asymmetry, level compensated, bit-exact
  bypass at 0), `softClip` (output bound of exactly 1), `velocityBrightness`.
- `DigitalMachine<Policy>` (`digital_machine.h`) owns the identical lifecycle: parameter values,
  `PercCore` (velocity, pitch offset, pan, pre-note delay), the 16-sample control-rate sub-block loop,
  soft-clip and bus routing. The Machine policy supplies its table, operators and per-sample wiring and
  is inlined into `render()` — no per-sample virtual call. Adding a Machine = a policy header, a kind
  value, and the registry cases.

Every Machine puts `tune`, `decay` and its depth control (`fm`, or `chaos`) at ids 0, 1, 2, so a client
can swap Machines on a track and keep those values.

## Machines

All velocity handling: linear level plus brightness (index x 0.55..1). Fresh notes are deterministic
(phases, noise seed, feedback cleared); a retrigger carries phases and restarts envelopes from their
current level. `noteOff` is a short release (a hat choke, a drum cut); one-shot defaults end by
themselves in exact silence. Tone and Chaos with `hold` > 0 are sustained: pass `duration` on
`instrumentNoteOn` (or call `instrumentNoteOff`).

### Thump — `DmThumpParam`: tune, decay, fm, sweep, punch, ratio, body, drive

```
pitch env (exp, sweep oct) -> f;  modulator (ratio x f, feedback `body`) --index--> carrier (f)
                                                          `--0.5 index--> overtone (1.59 f)
y = carrier + body x overtone;  x amp env;  + band-passed noise click (`punch`);  -> Drive
```
`ratio` is magnetic over 0.5, 1, 1.59 (membrane mode), 2, 2.76, 3.5, 5.4. The index envelope is faster
than the amplitude (shorter with `punch`), so the hit is bright for a few ms and settles to a sine.

### Metal — `DmMetalParam`: tune, decay, fm, spread, tone, noise, chick, drive

Four operators at a ratio set that `spread` glides through (harmonic `1 2 3 4`, free-free bar
`1 2.76 5.40 8.93`, 808-style `1 1.483 1.800 2.546`, scattered `1 2.236 3.606 5.385`): two parallel
pairs (B->A, D->C), each also cross-modulated by the other pair, noise added and as phase modulation,
x (body envelope + `chick` x a 6 ms envelope), then a high-pass (`tone`, 150 Hz..6 kHz) and Drive.

### Tone — `DmToneParam`: tune, decay, fm, structure, ratio, damp, attack, hold, drive

Four operators, a constant modulation-weight table per `structure` (op i is modulated only by ops above
it): 0 **Stack** `3->2->1->0`, 1 **Twin** `(1->0)+(3->2)` with a fast 7:1 "tine", 2 **Fan** `1,2,3->0` at
bar-like ratios, 3/4/5 **Major / Minor / Sus** — one shared modulator `3 -> 0,1,2` with carriers at the
chord's intervals. Each modulator has its own envelope (decay x a per-structure time scale x 0.08..1 by
`damp`), the DX trick: bright strike, settling tone. `hold` is the carrier sustain; the top modulator
has self-feedback; op 0 gets a clean-sine share at low pitch (0.8 at/below 80 Hz) so the fundamental
survives the Bessel nulls. No filter.

### Crack — `DmCrackParam`: tune, decay, fm, body, noise, burst, tone, drive

A feedback 2-op body (modulator 1.59 x, 0.6 octave strike drop) plus band-passed noise. The noise also
**phase-modulates the carrier** (`fm`), so the body becomes a pitched hiss. `burst` 0..1 = 1..5 noise
bursts 7..13 ms apart (earlier bursts decay in ~12 ms, the last carries `decay`) and fades the body: snare
-> clap.

### Chaos — `DmChaosParam`: tune, decay, chaos, ratio, fold, noise, sweep, hold

A three-operator ring `A -> B -> C -> (one sample later) -> A`, loop gain `chaos^2 x 9 rad` riding a
decaying envelope, noise into A's phase, then a wavefold and a DC blocker. Every operator is a sine, so
the output is bounded for any setting; chaos only changes how far the spectrum scatters, and the result
is deterministic (a fresh note clears everything).

## Decisions

- **One kind per Machine**, not one kind with a `machine` parameter: parameter tables (names, ranges,
  defaults, modulation domains) are static per kind in the engine contract; per-kind tables keep every
  Machine self-describing and LFO-correct with no contract change. Switching Machines on a track = create
  another instance.
- **No internal multi-timbrality.** The only shareable resource is the read-only sine table, which every
  instance already shares; a Machine pool would add the voice allocator the engine deliberately lacks.
- **Aliasing policy.** Thump, Metal and Tone limit the index from the live bandwidth (the tests fail if the
  guard is removed). Chaos does not: its folded-over brightness is the point; its operators still fade near
  Nyquist. Metal runs without oversampling; decide after listening whether its top pair needs 2x.
- **Percussive envelopes**: exponential decays with a fast release on note-off (as the drums), not ADSR-squared.

## Cost

Measured with `DM_BENCH=1 native/build/dm_bench` (clang -O3, native, 16 sounding instances, ns per sample
per instance; informational): Thump 18.5, Metal 22.1, Tone 29.0, Crack 17.2, Chaos 39.4, existing Fm 31.1.
The table sine is not a dramatic win over `std::sin` on its own; the cost is dominated by operators and
envelopes. WASM numbers differ. No allocation in `render` (counted in `dm_test.cpp`).

## Tests

`native/test/dm_test.cpp` (`npm run test:native`; `DM_WAV_DIR=<dir>` dumps demo WAVs; `DM_BENCH=1` times
them): the shared contract for every Machine (metadata incl. ids 0/1/2, clamping, finite/bounded/ends in
exact silence, deterministic, velocity, pitch offset, note-off choke, continuous retrigger, scheduling and
duration, bus routing, polyphony equals the sum, a random-parameter fuzz at 22.05/48/96 kHz, zero
allocation); kernel (table error, magnetic plateaus, envelope, guards, drive); and per Machine the
behaviours in the validation table below. The near-Nyquist test is a 16384-point FFT power share.

## Validation sounds

| Sound | Machine | Behaviour asserted |
|---|---|---|
| 80s FM pluck | Tone Twin, hold 0 | centroid falls while the carrier still rings; `damp` 1 keeps the timbre |
| DX bass | Tone Stack, 55 Hz | fundamental never more than ~14 dB below the plain carrier across `fm` |
| Synthetic kick | Thump | pitch starts >2.5x Tune and settles on Tune; `sweep` 0 is flat; `punch` adds a click |
| Tuned tom | Thump (ratio 1.59) / Tone | overtone at 1.59 x; Tone can do a tom-like hit too |
| Metallic hat open/closed | Metal | `tone` brightens, `decay` sets length, note-off chokes, `noise` adds energy |
| Bell | Metal bar set / Tone Fan | partials at 2.76 / 5.40 x; FM sidebands decay away; Fan sidebands inharmonic, Stack harmonic |
| FM chord stab | Tone Major / Minor | peaks at the chord intervals, no minor third in major; shared-modulator sidebands |
| Snare / clap | Crack | exactly 1..5 noise bursts for `burst` 0..1; clap has no body; `tone` moves the noise |
| Strange percussion | Chaos | bounded and deterministic, sensitive to `chaos` (0.5% changes the waveform) |
| Aggressive metallic lead | Chaos hold 1 | pitch tracks, sustains, releases; never blows up over 3 s at full chaos/fold/noise |

## Tuning status

All constants (index scales, feedback, level trims, ratio tables, burst timing, Chaos loop gain) are
first-pass values set by measurement, **not yet by ear**. Level trims target roughly Bass-class loudness
(default-patch RMS over the first 100 ms: Thump 0.24, Metal 0.09, Tone 0.16, Crack 0.13, Chaos 0.18).
Dump WAVs with `DM_WAV_DIR` and tune; the first candidates are Metal's `kIndexScale`/high-pass range,
Crack's burst spacing and noise gain, Tone's per-structure index/time scales, and Chaos's `kLoopGain`.
Client integration (webseq, webtracker) is not done yet.
