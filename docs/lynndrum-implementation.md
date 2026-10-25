# Implementation plan: issues from the LynnDrum review

Source: [`lynndrum-review.md`](./lynndrum-review.md). Each item is independent and shippable alone. Native tests are
picked up automatically by `native/test/run.sh` (any `native/test/*_test.cpp`, `clang++ -std=c++17 -O1 -Inative/src`).
Every change that touches `native/` ends with `npm run build:wasm` and committing the regenerated
`src/worklet/generated/engine.js` and `lib/`. Clients (webseq, webtracker, webvst) need a lockfile bump only for the
engine-behaviour changes (P1-1, P1-3); see the "bump = integrate" rule: nothing here adds client UI.

Suggested order: P1-3 first (the net that proves the others), then P1-1, then P1-2.

---

## P1-1. `Delay` time slew

Problem: `Delay` jumps its read pointer on every time change (11x sample step on a 100 ms ramp). See review finding A.

Files: `native/src/dsp/delay.h`, `native/src/fx_params.h` (comment only), new `native/test/delay_time_test.cpp`,
`docs/fx-chain.md`.

Design: copy the mechanism from `dub_delay.h`, not a new one.
1. Replace the per-block `delayFrames` int with `double delayCur_` and `double targetFrames_`.
   `setParam(DelayTime)` sets `targetFrames_ = clamp(seconds * sampleRate, 1, capacity - 3)`; nothing else.
2. In the sample loop: `d += clamp(target - d, -kMaxSlew, kMaxSlew)` with `kMaxSlew = 0.4` (same constant and rationale as
   DubDelay: pitch moves at most +-40% during a change), then a linearly interpolated read at `w - d`
   (copy the `r0/r1/frac` code from DubDelay).
3. A fresh or reset delay snaps `delayCur_ = targetFrames_` on its first processed block (DubDelay's `started_` flag), so
   a loaded project does not glide in from 0.
4. Keep `dirty_`/bypass-clear behaviour from `bypass_state_test`. `reset()` clears `started_`.
5. Settled state must be exact: when `delayCur_ == targetFrames_` and the target is a whole number of frames, interpolation
   with `frac = 0` returns `ring[r0]` unchanged. Non-integer targets are rounded (`std::round`) at set time so the
   settled output stays bit-identical to today for the common case.
6. Leave `time` as `kNoMod` in `fx_params.h` for now; update its comment to say clicks are fixed and that LFO modulation is
   deliberately still off pending a listening test. Making it modulatable is a separate, later decision.

Tests (`delay_time_test.cpp`), reusing the harness in `test_main.h`:
- 440 Hz sine, mix 0.5, feedback 0.4: ramp `time` 0.25 to 0.35 s in 128-frame steps; assert the max adjacent-sample step
  during the ramp is under 1.5x the steady-state max step (current code: 11x).
- Single 0.25 to 0.30 jump: max step under 1.5x input slope (current: 2.4x).
- Settled output is bit-identical to a golden captured from the pre-change code for fixed time (capture the golden hash
  before editing `delay.h`).
- `bypass_state_test` and `fx_chain_test` still pass.

Risks: a brief pitch glide on time changes (intended; same as DubDelay). Memory unchanged. Audition a knob drag in
webseq before shipping.

Done when: tests pass, WASM rebuilt, `docs/fx-chain.md` Delay section mentions the slew.

---

## P1-2. Retrigger continuity: conformance test, then fix `dm_thump`

Problem: retrigger policy varies per instrument and nothing checks it; `dm_thump` steps ~0.36 in two samples when
retriggered while ringing. See review finding B.

Step 1: the test (`native/test/retrigger_test.cpp`).
- Table-driven over all 18 kinds via `makeInstrument` / `instrumentKindMeta`.
- For each kind, and retrigger at 2, 8 and 40 blocks: `noteOn`, render, `noteOn` again, render. Metric: low-pass the output
  (one-pole, ~2 kHz) before measuring, so noise and metal content is judged by its low-frequency waveform, then compare
  `max |y[at..at+3] - y[at-1..]|` against the max step in the preceding 240 samples.
- Fail when the boundary step exceeds both 8x the preceding max and 0.05 absolute.
- Known failures go in an explicit `kKnownOpen[]` list with a comment, so the test is green on day one and each fix removes
  an entry. Expected initial list: `dm_thump`, and any other DM kind the new metric flags. Do not add kinds to the list
  without measuring.
- Probe values from the review (unfiltered metric): dm_thump 0.2497 vs 0.0114 at 21 ms; kick, bass, fm continuous.

Step 2: fix the digital machines. Reference fix: commit `571a2fe` (Perc Synth), which defers the new note's sweeps,
velocity and pitch until the `noteOnRestart` fade reaches silence.
1. In `dm_thump.h` `noteOn(..., fresh)`: when `!fresh` (retrigger on a live voice), do not call `index_.trigger`,
   `pitch_.start`, `click_.trigger` or `noise_.reset()` immediately. Store the new parameters as pending and start a short
   fade (reuse the 2 ms `kRestartFadeSeconds` idea) on `amp_`; apply the pending triggers when the fade reaches zero.
2. Implement the deferral once in the shared `DigitalMachine`/`fm_kernel.h` layer (e.g. a `RestartGate` helper beside
   `ExpEnv`) so the other four machines (metal, tone, crack, chaos) inherit it. Do not copy it into five files.
3. Fresh notes must be unchanged: keep the `fresh` path bit-identical (golden-hash `dm_test` output before editing).
4. Re-run the new test; remove fixed kinds from `kKnownOpen[]`. If a kind is flagged only by the metric (a legitimate
   transient), adjust the metric, not the instrument, and say so in the test comment.

Risks: attack of back-to-back thumps changes by up to ~2 ms. Tune by ear (the Digital Machines are still ear-tuning
pending). Update `docs/digital-machines.md` with the retrigger rule.

Done when: test green with an empty or fully commented `kKnownOpen[]`, `dm_test` passes, WASM rebuilt.

Also deliver (cheap, same PR): a one-table "retrigger policy per instrument" section in `docs/instruments.md`
(`noteOnRestart` / carry / deferred), which is P2-6 in the review.

---

## P1-3. Permanent parameter-catalogue and zero-allocation tests

Problem: robustness is currently verified by one-off probes; a future instrument or FX could regress it unnoticed.

Files: new `native/test/param_sweep_test.cpp`, new `native/test/engine_alloc_test.cpp`.

`param_sweep_test.cpp` (about 2.5 s runtime):
- For every instrument kind (`instrumentParamCount`/`instrumentParamInfo`): default, all-min, all-max, all-mid at velocities
  0, 0.2, 1, 2; each parameter alone at min/max/mid; 12 random vectors per kind from a fixed-seed `std::mt19937`.
- Each case: `noteOn`, render 2 s in 128-frame blocks with `noteOff` at 1 s, then up to 20 s more until inactive.
- Assert: every sample finite, peak under 3.0, and the instrument reaches inactive after `noteOff`.
- FX half: for every `FxKind` and `fxParamInfo`, the same combinations (default, all-min/max/mid, each param min/max, 20
  random) through `FxChain` with 300 blocks of noise then 300 blocks of silence. Assert finite and peak under 20.
- Baseline when measured on 2026-10-25: 1029 instrument cases and 320 FX cases, zero failures. If a new kind legitimately
  exceeds a bound, raise that single bound with a comment rather than weakening the test.

`engine_alloc_test.cpp`:
- Override global `operator new`/`new[]` with a counter armed only around `Engine::process` (pattern already used in
  `adsr_test.cpp` and `bass_test.cpp`; copy it).
- Scenario: one instance of each of the 18 kinds on buses 1-8, one of each of the 10 FX kinds on each bus, sends and a bus
  route, choke groups, scheduled notes with durations, scheduled param ramps, a sample voice. Warm 10 blocks, then 3000
  armed blocks. Assert zero allocations.
- Note: `commitSample(id, ch, len, sr, float* const*)` takes ownership and `free()`s the buffers, so pass `malloc`ed
  memory, never a stack array (this aborted the first probe).

Done when: both tests run in `run.sh`, pass, and are mentioned in `ARCHITECTURE.md` under automated tests.

---

## P2 items (after P1)

### P2-4. Master non-finite containment
- Where: end of `Engine::process`, after `limiter_.process`, or inside `Limiter::process`.
- Behaviour: per sample, if `!std::isfinite(x)` write 0 and set a block flag; on a flagged block call `reset()` on the
  offending bus chains if cheap to identify, otherwise just count. Surface the count through the existing meters block
  (append one float; additive only, keep the host protocol rules in `docs/native-host.md`) or the diagnostics.
- Cost: one compare per sample per channel. Do not add this before P1-3 exists; P1-3 is what would tell you it is needed.
- Test: inject a NaN into a bus buffer through a test-only hook (or a custom `DSPNode`) and assert the output is finite and
  the next block is clean.
- Why only P2: no NaN source found in 1349 sweep cases; this is insurance. LXR's int16 path structurally cannot emit NaN.

### P2-5. Enum mirror check
- Add `test/paramMirror.test.ts` (vitest) that reads `native/src/params.h` and `src/runtime/types.ts`, extracts
  `enum class X { ... }` / `export enum X { ... }` bodies, strips comments, assigns implicit values, and compares by name.
  Also compare `kMaxTrackBuses` with `MAX_TRACK_BUSES`, `kFxKindCount` with the number of `EffectKind` members, and
  `kMaxFxPerBus` with `MAX_EFFECTS_PER_BUS`. Fail on any mismatch or any one-sided member.
- On 2026-10-25 the same check run ad hoc found 24 C++ enums and 0 mismatches, so it should pass on day one.
- `paramIds.test.ts` stays; it pins literal values, this adds the cross-language check it says does not exist.

---

## P3 experiments (each as its own branch, A/B by ear and by measurement)

1. Slope curve: add `slopeCurve(x, k) = (1+k)x/(1+k|x|)` to `dsp/utility.h`; apply as an optional shaper on `PercEnv` /
   `AdsrEnvelope` output; compare against the current squared envelope in a spectrogram and by ear.
2. Kick start-phase offset: expose `startPhase` (0 = zero crossing, 0.25 = current peak start); measure the first-5-ms click
   energy.
3. FM depth coupled to pitch envelope for kick/tom/percsynth: a single coupling constant, default 0 (no behaviour change).

---

## Do not change
RBJ biquad (measured equal to TPT SVF under modulation), `ParamModulation`, instance-owned engine state, sample-accurate
events, the bus graph and send/gain semantics, the master limiter, per-sample envelopes.

## Definition of done for the whole plan
P1-1 to P1-3 merged with tests green in `npm run test:all`; WASM and `lib/` rebuilt and committed; webseq/webtracker/webvst
lockfiles bumped (no UI work needed); `docs/fx-chain.md`, `docs/digital-machines.md`, `docs/instruments.md` updated; the
memory note `lynndrum-comparative-review` updated with what shipped.
