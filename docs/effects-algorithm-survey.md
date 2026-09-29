# Effects algorithm survey

ECS-14. Shortlists established algorithms for reverb, compressor, saturation, chorus/flanger,
and phaser, with tradeoffs and licensing, as of commit `fd80924`. This is a paper survey —
**no new `native/src/dsp/` code exists yet** for any of these; see `dsp-audit.md` (ECS-13) for
what's actually implemented today (biquad filter, delay) and the `DSPNode` shape any of this
would plug into. Treat every reference implementation below as an algorithmic reference to
re-derive from its paper, not a dependency to vendor — matches `ARCHITECTURE.md`'s existing
"DSP library investigation" stance (biquad + resampler were hand-written for the same reason).

## 1. Reverb candidates

| Algorithm | Audible character | CPU / memory | Browser/WASM fit | Implementation cost | Reference / license |
|---|---|---|---|---|---|
| **Schroeder** (4 parallel comb + 2 series allpass) | Metallic, audible periodicity ("ringing") from comb spacing; frequency-independent decay (no damping) unless combs are extended with a lowpass in the feedback path. Weakest of the five. | Cheapest: ~6 delay lines, no matrix, O(1) per sample per line. | Trivial — plain arrays, no deps. | Smallest (~50–80 lines/channel), but tuning delay lengths to avoid audible periodicity is fiddly and never fully succeeds. | Schroeder, "Natural Sounding Artificial Reverberation" (JAES 1962). No single canonical reference implementation; textbook algorithm, public-domain description. |
| **Moorer** (Schroeder + per-comb one-pole damping + tapped-delay early reflections + extra allpass diffusion) | Clear improvement over Schroeder — damping tames the ringing, early-reflection taps add a sense of room size before the diffuse tail starts. Still audibly "comb-y" under scrutiny vs. FDN/Dattorro. | Comb bank + early-reflection tap delay; modestly more than Schroeder, still all O(1)/sample. | Trivial, same as Schroeder. | Small-to-medium; early-reflection tap design (delay times + gains) is its own tuning problem, same genre of fiddliness as Schroeder's comb spacing. | Moorer, "About This Reverberation Business" (Computer Music Journal, 1979). Textbook algorithm, no canonical reference implementation. |
| **FDN** (N delay lines through an N×N unitary/lossless feedback matrix — Hadamard or Householder) | Best-in-class for smooth, dense, natural-sounding tails; per-band decay is tunable via per-line damping filters. The gold standard for "large space" reverbs when done well. | O(N²) per sample for a dense mixing matrix (O(N) for a fast Hadamard/Householder-with-structure), N delay lines (4–16 typical). Most CPU/memory of the five, scales with N. | Trivial to port (no exotic dependencies) but **the one candidate with a real stability risk**: the mixing matrix must stay unitary/lossless or the tail can grow without bound — needs careful implementation and testing, not just careful tuning. | Largest of the five: matrix design + per-line damping + modulation to avoid metallic ringing at low N is a genuine DSP design task, not a lookup-and-implement. | Jot & Chaigne, "Digital Delay Networks for Designing Artificial Reverberators" (AES 1991); Julius O. Smith III, [*Physical Audio Signal Processing*, FDN chapter](https://ccrma.stanford.edu/~jos/pasp/Feedback_Delay_Networks.html) — free online textbook, public reference, no code license issue. |
| **Dattorro plate** (4-stage input diffusion allpass chain → two cross-fed "tank" loops, each with a delay, one modulated allpass, a one-pole damping filter, and a long delay) | Widely regarded as the best quality-per-CPU tradeoff of the five — this exact topology (or a close variant) underlies several well-known commercial plate reverbs. Mono-in/stereo-out by design, dense and smooth without FDN's stability risk. | Fixed, modest topology: ~2 diffusion allpasses + 2 tank loops each with 1 delay + 1 modulated allpass + 1 one-pole filter + a couple of long delay taps. Comparable order of magnitude to Freeverb, denser-sounding per CPU cycle. | Trivial — plain delay lines, allpass filters, one-pole filters; the modulated allpass needs the same fractional-delay interpolation the project already has in `native/src/resampler.h` (cubic Hermite) — reusable, not new code. | Medium: the topology is fully specified (all delay lengths, coefficients, and the block diagram) in the source paper, so it's a direct port rather than a fresh design — closer to "translate a paper" than "invent an algorithm." No stability tuning required, unlike FDN. | Dattorro, ["Effect Design, Part 1: Reverberator and Other Filters"](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf) (JAES 45(9), 1997). Paper itself carries no code; algorithm description is freely citable. Third-party ports exist under a range of licenses (MIT, GPL, public-domain) — re-derive from the paper's own delay-length table rather than copying any specific port, to sidestep per-port license review entirely. |
| **Freeverb** (8 parallel comb filters w/ damping + 4 series allpass, per channel, slightly detuned delay lengths per channel for stereo width) | Good, well-known "workhorse" sound — smoother than plain Schroeder/Moorer thanks to per-comb damping, but a simpler, less dense topology than Dattorro/FDN; still has a faint comb character on sustained tones if you listen for it. | Similar order of magnitude to Dattorro (8+4 = 12 filters/channel vs. Dattorro's ~6), somewhat less code/state per filter (all comb/allpass, no modulation). | Trivial — this is *the* reference "easy WASM port" case; it's been ported to essentially every language and platform already (C, Rust, Pure Data, Csound...). | Smallest of the "serious" candidates — one widely-read, ~200-line reference source (structure, not license, is what to reuse), fixed tuned constants published alongside it. | Jezar at Dreampoint, Freeverb (2000) — [source explicitly released to the public domain](http://blog.bjornroche.com/2012/06/freeverb-original-public-domain-code-by.html), the one candidate here with zero licensing ambiguity even if code were copied directly rather than re-derived. |

### 1a. Recommendation: Dattorro plate as the first reverb candidate

**Dattorro plate is the serious first candidate.** Reasoning, weighed against this project's
actual constraints (single-threaded WASM render callback, existing `DSPNode` 3-method
interface, "own small native algorithms" bias per `ARCHITECTURE.md`):

- It's a fully-specified topology (exact delay lengths, coefficients, block diagram in one
  paper) — a **port**, not a **design**, unlike Schroeder/Moorer (which need original
  comb/tap tuning to sound good) or FDN (which needs matrix-stability engineering). Lowest
  design risk for the quality it delivers.
- No stability-proof burden. FDN sounds better at high N but a bug in the mixing matrix
  produces a runaway feedback tail — a correctness risk Dattorro's fixed feedback topology
  doesn't have. Given there's currently zero test coverage for any DSP node's *audible
  behavior* beyond the smoke test's attenuation check (`dsp-audit.md` §6), that risk is worth
  avoiding for the first reverb.
- Reuses code already in the tree: the modulated allpass needs the same fractional-delay
  interpolation as `native/src/resampler.h`.
- Mono-in/stereo-out matches how reverb is actually wanted on a bus (one dry signal in, a
  wide wet return) better than Freeverb's per-channel-instance stereo trick.

**Freeverb is the fallback if Dattorro's implementation cost proves too high in practice** —
strictly simpler (no modulation, no cross-feeding between two tank loops), zero licensing
ambiguity even as a direct-port starting point, and "good enough" for a first cut. Treat it as
the pressure-release valve, not the primary plan.

**Schroeder/Moorer are not recommended as a shipped v1** — they exist in this table because the
ticket asked for them by name, and Moorer specifically is worth reading for the early-reflection
tapped-delay idea (which Dattorro's paper doesn't emphasize), but neither beats Dattorro or
Freeverb on any axis for a from-scratch v1.

## 2. Smallest-useful approach for the remaining effects

| Effect | Smallest-useful approach | Why | Reference |
|---|---|---|---|
| **Compressor** | Feedforward, log-domain gain computer (peak or RMS detector → soft-knee static curve → one-pole attack/release smoothing in the log domain) | The standard modern digital design; feedback topologies (auto-modeling analog behavior) exist but add complexity this project doesn't need for a first compressor. Log-domain smoothing avoids the audible "zipper"/overshoot artifacts of naive linear-domain gain smoothing. | Giannoulis, Massberg, Reiss, ["Digital Dynamic Range Compressor Design — A Tutorial and Analysis"](https://www.researchgate.net/publication/277772168_Digital_Dynamic_Range_Compressor_Design-A_Tutorial_and_Analysis) (JAES 60(6), 2012) — compares feedforward/feedback and peak/RMS/log variants directly; openly available. |
| **Saturation** | Single memoryless waveshaper (`tanh` or a cubic soft-clip), no oversampling for v1 | Cheapest possible correct-shaped nonlinearity; a single `DSPNode::process` with no extra state. Tradeoff to record explicitly: no oversampling means audible aliasing on hard-driven high-frequency content — acceptable for a first cut, and the documented upgrade path (2×/4× oversampling, or antiderivative antialiasing) is well understood if it's not. | DAFX (Zölzer, ed.), nonlinear processing chapter, for the general waveshaping approach; antiderivative antialiasing (the upgrade path) per Parker/Zavalishin/Bilbao (DAFx conference papers, "Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution", 2016). |
| **Chorus/flanger** | One modulated fractional-delay tap (LFO sweeping read position, ~1–30 ms range) summed with dry — chorus and flanger are the same primitive at different delay ranges/feedback amounts, not different algorithms | Directly reuses the fractional-delay interpolation already in `native/src/resampler.h`; a single modulated delay tap is the textbook minimum for a chorus/flanger effect, with additional voices/taps as a pure quality dial, not a required part of a first cut. | Dattorro, ["Effect Design, Part 2: Delay-Line Modulation and Chorus"](https://ccrma.stanford.edu/~dattorro/EffectDesignPart2.pdf) (JAES 45(10), 1997) — the direct companion to the Part 1 reverb paper above, covers exactly this. |
| **Phaser** | Cascade of 4 first-order allpass stages, each coefficient swept by one shared LFO, with a feedback path for notch depth | Classic analog-phaser-emulation topology; 4 stages is the practical minimum for an audible sweep (fewer sounds too subtle), more stages just add notches. Each stage is a ~3-line allpass, so total cost is small and linear in stage count. | Julius O. Smith III, ["Phasing"](https://ccrma.stanford.edu/~jos/pasp/Phasing.html) — *Physical Audio Signal Processing*, free online textbook chapter. |

None of these four need a topology *comparison* the way reverb did — each genuinely has one
dominant standard approach at the "smallest useful" tier; the tradeoffs worth recording are
within the chosen approach (oversampling, stage count, detector type), captured above.

## 3. Owning a small native algorithm vs. a mature library vs. Faust

| | Own it (`native/src/dsp/`, C++, as done today) | Mature C/C++ library (e.g. freeverb3, STK, Airwindows) | Faust (`.dsp` → generated C++, or `faustwasm`) |
|---|---|---|---|
| **Binary size** | Smallest — only the exact algorithm compiled in, same pattern as existing `biquad_filter.h`/`delay.h`. | Larger, and not fully controllable: static-linking a library built for general use pulls in code paths this project won't call unless carefully isolated per-file; WASM has no dynamic linker to defer that cost. | Medium-large — generated C++ per `.dsp` file is usually compact per-effect, but adds the Faust runtime/architecture glue unless trimmed. |
| **Build/debug cost** | Lowest — same toolchain already in use (`npm run build:wasm`, Emscripten only), same `native/test/*_test.cpp` pattern for unit tests. | New: vendoring + compiling a third-party tree under Emscripten, checking it tolerates `-fno-exceptions`/no-RTTI (common WASM build flags), and staying current with upstream. | New, second toolchain: the Faust compiler itself (buildable from source and usable fully offline — see below — but still a tool to install, pin, and document alongside Emscripten). Generated C++ is harder to hand-audit than the project's existing flat, hand-written `api.cpp`-style ABI. |
| **Offline builds** | Yes, already true today — no network dependency once `.emsdk/` is present. | Depends on the library; most pure C/C++ audio libraries (STK, Airwindows, freeverb3) vendor cleanly and build offline once fetched once. | Yes, once the Faust compiler binary is built/vendored locally (`make` from the Faust source tree has no external deps and is fast); the *hosted* `faustwasm`/online IDE route is not offline and should be avoided for this reason. |
| **API compatibility** | Perfect by construction — every node is written directly against the `DSPNode` 3-method interface. | Needs an adapter per library (different call conventions, often block-oriented or object-graph-shaped, not a bare `process/setParam/reset` triple). | Needs an adapter: Faust's generated `dsp` class (`compute()`, `buildUserInterface()` for params) doesn't match `DSPNode` directly — a thin wrapper is required either way, effect-by-effect. |
| **Maintainability** | Full ownership, full understanding — matches this project's existing "small, auditable, hand-written" bias (see `ARCHITECTURE.md`'s flat-ABI rationale). Cost: algorithm-design and tuning labor is entirely on this project. | Upstream API drift, and license terms travel with the code: LGPL in particular is awkward under WASM static linking (no relinkability story without shipping object files). GPL would affect the whole binary's distribution terms. | Faust itself and the widely-used `faustlibraries` effects (reverbs, phaser, chorus, compressor — nearly the whole shortlist already exists as tested `.dsp` code) are STK-4.3-licensed (MIT-style) as of the current `faustlibraries` repo, but license is declared **per file**, not blanket — verify the specific file before treating any one as pre-cleared. |

**Recommendation, consistent with `ARCHITECTURE.md`'s existing stance:** keep hand-writing
`DSPNode` subclasses for the reverb/compressor/saturation/chorus/phaser shortlist. Each is
individually small enough (see §1–§2) that the labor saved by a library or Faust is modest,
while the binary-size, build-toolchain, and licensing costs are concrete and immediate. Faust
remains the documented fallback if this effects list keeps growing past what hand-writing
comfortably supports — evaluate it again at that point, not now.

## 4. Measurement questions to answer before committing to an implementation

The comparisons above are structural (topology, complexity, licensing); the following need an
actual build + measurement, not a table, before picking a final approach:

1. **WASM binary size delta.** `ls -la src/worklet/generated/engine.js` (or the `.wasm` if
   split) before and after adding the new node — compare against the size of the two existing
   nodes (`git log`/`git show` around `1668f13` or the initial commit can establish a rough
   per-node baseline) to sanity-check the estimates in §1's table.
2. **Per-block CPU cost.** No CPU/underrun diagnostic currently exists in this engine
   (`ARCHITECTURE.md`, "Intentionally deferred") — the practical proxy today is timing
   `native/test/smoke.mjs`-style runs (wall-clock over N processed blocks) before/after, the
   same way `dsp-audit.md` §6 measures filter behavior via RMS, not a live `cpuLoad` field.
3. **Audible quality, A/B'd against a known-good reference render** (e.g. a reference Dattorro
   or Freeverb render from an existing DAW/plugin) — the tables above are drawn from the
   literature's own claims, not from listening to this project's own port.
4. **Memory footprint** — sum of `delay line length × channels × sizeof(float)` for the
   chosen topology (Dattorro's longest tank delay is on the order of tens of ms at the
   engine's sample rate; confirm against the paper's own sample-rate-scaled table rather than
   assuming) versus the fixed `kMaxTrackBuses = 32`-wide pool this would need to multiply
   across if instantiated per-bus like the existing filter/delay (`dsp-audit.md` §2).
5. **FDN stability, if ever revisited** — confirm the chosen mixing matrix is unitary/lossless
   under the project's actual float precision (32-bit, per WASM `HEAPF32` usage throughout)
   before shipping; this is the one candidate in §1 where an implementation bug is a runaway
   signal, not just a wrong-sounding one.

## Sources

- Schroeder, "Natural Sounding Artificial Reverberation," JAES 10(3), 1962.
- Moorer, "About This Reverberation Business," Computer Music Journal 3(2), 1979.
- Jot & Chaigne, "Digital Delay Networks for Designing Artificial Reverberators," AES 90th
  Convention, 1991.
- Julius O. Smith III, [*Physical Audio Signal Processing*](https://www.dsprelated.com/freebooks/pasp/)
  — free online textbook; [FDN chapter](https://ccrma.stanford.edu/~jos/pasp/Feedback_Delay_Networks.html),
  [Phasing chapter](https://ccrma.stanford.edu/~jos/pasp/Phasing.html).
- Dattorro, ["Effect Design, Part 1: Reverberator and Other Filters"](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf),
  JAES 45(9), 1997.
- Dattorro, ["Effect Design, Part 2: Delay-Line Modulation and Chorus"](https://ccrma.stanford.edu/~dattorro/EffectDesignPart2.pdf),
  JAES 45(10), 1997.
- Jezar at Dreampoint, Freeverb (2000) — [public-domain release](http://blog.bjornroche.com/2012/06/freeverb-original-public-domain-code-by.html).
- Giannoulis, Massberg, Reiss, ["Digital Dynamic Range Compressor Design — A Tutorial and Analysis"](https://www.researchgate.net/publication/277772168_Digital_Dynamic_Range_Compressor_Design-A_Tutorial_and_Analysis),
  JAES 60(6), 2012.
- Parker, Zavalishin, Bilbao, "Reducing the Aliasing of Nonlinear Waveshaping Using
  Continuous-Time Convolution," DAFx 2016 (antiderivative antialiasing — saturation upgrade path).
- [`faustlibraries` `reverbs.lib`](https://github.com/grame-cncm/faustlibraries/blob/master/reverbs.lib)
  and [Faust Libraries docs](https://faustlibraries.grame.fr/libs/reverbs/) — per-file
  STK-4.3/MIT-style licensing, referenced in §3; verify per file before use.
- This project's own `ARCHITECTURE.md` ("DSP library investigation", "Adding a new master-bus
  module") and `docs/dsp-audit.md` (ECS-13) — prior art for the "own it" bias and the exact
  `DSPNode`/`DSPChain` shape any of this plugs into.
