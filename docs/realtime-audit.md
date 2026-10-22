# Real-time / DSP architecture audit

Audit of the engine as it stands (commit `20cb4f2` plus the changes listed under "Implemented").
Scope: audio path, buses and routing, voices and instruments, effects, parameter propagation,
the AudioWorklet boundary, allocation and lifetime. It replaces nothing: the earlier
[`dsp-audit.md`](./dsp-audit.md) (ECS-13) is a historical capability inventory.

Method: read the docs and the whole render path, then **measured** rather than guessed. The
benchmark was a native `clang++ -O3` harness (not committed; WASM numbers are higher, and libm-heavy
code is hit hardest, so read the numbers as a ranking), plus three probes that reproduce a suspected
defect before it was called one. Everything marked *fixed* has a failing-then-passing test.

## 1. How the engine is put together

```
main thread                       audio thread (AudioWorklet, one thread, no locks)
-----------                       ------------------------------------------------
AudioRuntime --postMessage-->  handleCommand (between blocks, may allocate)
                                   |  SampleStore / VoiceManager / FxChain / instruments_ / queues
                               process(): Engine::process(blockStart, 128)
```

Per block, `Engine::process`: drain scheduled notes, transport and parameter events → advance
parameter ramps → render sample voices and sounding instruments into their bus accumulators →
for each track bus in topological order: FX chain → `BusGain` → `BusPan` → mix into its output
(`× BusMainGain`) and its send (`× BusSendAmount`) → master bus chain → capture tap → limiter.

Ownership: the engine owns everything. Voices and instruments are sources (they know a `busId`,
nothing about tracks). A `Bus` owns an `FxChain` (instances heap-allocated on the command side,
`process()` walks resolved pointers), a panner and a gain. Routing state (`busOutput_`, send
target/amount, `busOrder_`) lives only in `Engine`; effect state only in the node; parameter *base*
values for effects and instruments live in `ParamModulation`. There is no duplicated routing state.

**The graph matches the concept.** `source → voice gain/filter/pan → bus chain → fader (BusGain) →
pan → {main × BusMainGain, send × BusSendAmount}`. The send is post-fader and post-pan; mute
(`BusGain = 0`) silences both edges, solo gates only the main edge (`BusMainGain`), so a soloed
track's FX return stays live. That is exactly how webseq's `mixer.ts` uses it. Output and send
edges share one cycle guard and one topological order; a bus that is mixed into counts as fed.

### How often each thing happens

| Work | Frequency |
| --- | --- |
| Effect/instrument construction, buffer allocation, route rebuild, sample load | command (never in `process()`) |
| Parameter writes, coefficient redesign (`BiquadFilter::recompute`, `Compressor::recomputeCoeffs`) | command or LFO chunk (32 frames, only while an LFO targets it) |
| `pow`/`tan` hoisted per block (Saturation, Limiter ceiling) | render quantum |
| Bus gain/pan/mix/peak passes, ramp advance, scheduler drain | render quantum, per active bus |
| Voice/instrument oscillators, filters, envelopes, interpolation | per sample, per active voice |
| Phaser `sin`/`exp2`/`tan`, Compressor `log10`/`pow`, Saturation `tanh`, Reverb `sin`/`cos` | per sample |

`process()` allocates nothing: buffers, pointer tables, `busOrder_`, pending-note arrays and the
ended-voice vectors are sized in `init()`. The one growth hazard is noted in finding 9.

## 2. Measurements (native, ns per frame, stereo)

| Item | ns/frame | % of one core @ 48 kHz |
| --- | --- | --- |
| Instruments, one sounding instance (18 kinds) | 10 – 57 | 0.05 – 0.27 |
| Phaser | 44 | 0.21 |
| Reverb | 30 | 0.14 |
| Saturation / Compressor / Chorus / Delay / Filter | 13 / 9 / 10 / 6 / 5 | 0.03 – 0.06 |
| Sample voice (looped, no filter / with LP) | 6.2 / 11 per voice | 32 voices ≈ 1% |
| **16 configured buses, silent** (filter+comp+reverb+delay+sat each) | **1050** | **5.1** |

Nothing is individually expensive. The one number that does not scale with what you hear is the
last row (finding 3).

## 3. Findings

### Implemented

**1. `BusGain` applied instantly — click on mute, zipper on fader moves. P0 · fixed.**
*Location* `bus.h` (`Bus::process`, `setParam`). *Frequency* event. *Why* `BusGain` is the fader and
the mute, and every other bus level already glided (`BusMainGain`/`BusSendAmount` across the block,
`BusPan` smoothed). A sounding bus muted at a nonzero sample stepped the whole instantaneous value:
measured adjacent-sample jump 0.377 on a 0.5 tone whose own largest step is 0.025. A non-finite
value also went straight into the signal and poisoned downstream filter state (128 NaN samples in
the test). *Fix* the level glides linearly across the block it changes in (a bus that has never
rendered snaps, as `BusPan` does), is exact at rest, skips the pass at unity (a small per-bus
per-block saving), and ignores non-finite values. Same guard added to `Voice::setParam`.
*Risk* low (steady state is bit-identical; golden-output hash of a 600-block scenario unchanged).
*Confidence* high. Test: `bus_gain_test.cpp`.

**2. Bypass by `mix = 0` froze effect memory and replayed it later. P1 · fixed.**
*Location* `delay.h`, `chorus_flanger.h`, `reverb.h`. *Frequency* event. *Why* these effects return
early at mix 0 without clearing their delay lines. Raising the mix again, with **silent input**,
output the old tail: peak 0.40 (delay), 0.17 (chorus), 0.66 (reverb), a loud ghost of audio from
before the bypass. `FxChain::setEnabled(false)` already resets for exactly this reason; the mix-0
path was inconsistent with it. *Fix* clear once on the first bypassed block. While mix > 0 tails
ring on silent input exactly as before (asserted). The Phaser was probed and is clean (its state
had rung out within the probe's ~0.2 s of silence). *Risk* low. *Confidence* high on the defect; medium-high that
"bypass forgets" is the wanted semantic (it matches `setEnabled`). Test: `bypass_state_test.cpp`.

### Recommended, not implemented

**3. Fed buses run their whole FX chain forever, even silent. P1 · needs a decision.**
*Location* `Engine::process` (`isDormant`/`markFed`), `FxChain::process`. *Frequency* render quantum,
per bus. *Why* a bus that has ever been fed never sleeps again (documented, ECS-84, because a
level-threshold skip changed the next sound by up to 0.06). 16 silent configured buses cost 5% of a
core natively (a plain filter-only bus is ~0.04%; the cost is reverb, compressor, saturation and
delay running on zeros). It matters for many-track projects where most tracks are quiet at any
moment. *Recommended approach* gate on **exact silence**, not a level: skip an instance only when
its input block is all zeros *and* it reports itself quiescent (state flushed to exact zero after a
tail horizon; the compressor envelope caught up analytically, `env = floor + (env − floor)·coeffⁿ`,
the case that broke ECS-84). *Risk* medium-high: it touches every effect's state semantics.
*Confidence* high on the cost, medium on the design. Do this only with a benchmark that shows it
matters on a real project.

### Investigated and deliberately left alone

**4. Phaser is the most expensive effect (P2).** `sin` + `exp2` + `tan` per sample per channel for an
LFO ≤ 10 Hz; the coefficient could be updated every 8–16 samples with no audible change. ≈0.17% of a
core per phaser natively; not worth a behaviour change on its own. Compressor `log10`/`pow` per
sample, Saturation `tanh`, Reverb `sin`/`cos` and `%` are inherent or ≤ 0.14% each. *Ignore.*

**5. Mono sample interpolated once per output channel (ignore, measured).** `Voice::render` runs the
Hermite kernel twice for a mono sample. Computing it once made mono voices ~13% faster but the
stereo path ~10% slower from code layout; at 64 voices the win is ≈0.25% of a core. Reverted.
`voice_interp_test.cpp` stays: it pins mono == both channels == a stereo file of two equal channels.

**6. Worklet copies output through `HEAPF32.subarray` per block (ignore, measured).** Looks like
audio-thread garbage. In V8 it is 75–93 ns/block, faster than an index loop (175 ns), and shows zero
GC events over 3 M blocks. Left as is.

**7. No denormal handling (P2, uncertain).** WASM cannot set flush-to-zero. Double-precision biquad
state passes through the denormal range once per decay (worst case ≈ 0.3 s at a 20 Hz low-pass); the
float delay/reverb feedback passes it quickly. Only `percsynth.h` flushes. No penalty on Apple
silicon; **not measured on x86**. If x86 profiles show tail spikes, flush biquad state and reverb tank
state; otherwise leave.

**8. `voice-ended` is one `postMessage` per ended voice (P2).** Bounded by note rate; batching per
block would cut message count with no public API change. Not worth it yet.

**9. Latent, not reachable today (P2).** The `"configure"` command (`webdsp_configure_voices`)
re-`configure()`s every voice: a playing voice's envelope resets to idle while the voice stays
`Playing` (a silent zombie), a smaller pool drops voices without reporting them ended, and
`endedVoices_` is not re-reserved after growth (an allocation inside `process()`). Nothing in
`src/runtime` sends it. Fix these before exposing a runtime `setMaxVoices`.

**10. Sample-voice steal is not click-free (P2, uncertain).** A stolen voice keeps its envelope level
but its read position jumps and its filter resets, a small waveform step on the *quietest* voice. The
synth path got a 2 ms re-attack fade (`f958e4d`); sample voices did not. Only reachable with the pool
(default 128) exhausted. Understand before changing.

**11. Dead code and doc drift (P2).** `DSPChain<N>` (`dsp_node.h`) is unused since every `Bus` runs an
`FxChain`; `ARCHITECTURE.md` still said `Voice` owns a `DSPChain` (fixed). `native/test/smoke.mjs`
comments still describe the old fixed chain. `DSPChain` can be deleted whenever convenient.

## 4. Existing optimisations that look odd but are load-bearing (left untouched)

- **Dormant-bus skip and lazy accumulator zeroing** (`Engine::process`): an unused bus costs nothing.
- **Fed-forever rule**: exists because skipping on level changed the next sound (see 3).
- **`Panner` state in `double`** and the `isUnity()` skip: float never reaches exact unity/silence.
- **`BiquadFilter` double state, `prime()` on slope crossfade, 256-frame fade**: no step when the
  12 ↔ 24 dB path is swapped.
- **`Utility` unity bit-exact pass-through, `Delay`'s `time` pinned non-modulatable**: the read pointer
  jumps, so an LFO on it would click.
- **32-frame control-rate chunking only while an LFO targets a parameter**; otherwise one call.
- **Fixed pending-note arrays, pre-reserved vectors, pointer tables built in `init()`**: keep
  `process()` allocation-free.
- **Engine-side choke fade through a scratch buffer**: one-shot drums ignore `noteOff`.

## 5. Implemented in this audit

| Change | Files |
| --- | --- |
| `BusGain` glide, unity skip, non-finite guard | `native/src/bus.h` |
| Non-finite guard on voice params | `native/src/voice.h` |
| Clear memory on bypass (mix 0) | `native/src/dsp/{delay,chorus_flanger,reverb}.h` |
| Tests | `native/test/{bus_gain,bypass_state,voice_interp}_test.cpp` |
| Docs | this file, `ARCHITECTURE.md`, `docs/{fx-chain,panning,dsp-audit}.md` |
| Rebuilt artifacts | `src/worklet/generated/engine.js`, `lib/engine-processor.js` |
