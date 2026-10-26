# Visualisation

How live audio displays are built across webdsp, webseq, webtracker and webvst: the EQ editor's
spectrum, the master level meter, and the pattern for adding the next one. The short version:

- **webdsp supplies data and maths, never pixels.** No canvas, DOM or framework code lives here.
- **The clients draw**, through one small shared vocabulary (a surface, a few painters, a theme
  reader) that exists once per client and is kept identical in both. Only colours differ.
- **A display never touches React state.** It owns one `requestAnimationFrame` loop that writes to a
  `<canvas>`, so it cannot cause a React render and cannot disturb the playhead or the rest of the UI.

The reference for the spectrum data path alone (tap, FFT, API) is [`spectrum.md`](./spectrum.md); the
EQ itself is [`eq.md`](./eq.md); metering is [`master-limiter.md`](./master-limiter.md). This document
is the map that joins them.

## The layers

```
 audio thread (WASM / native)         plain data               UI thread (client)
 ─────────────────────────────────────────────────────────────────────────────────────────
 effect input/output  ──tap──►  "analysis" event  ──►  SpectrumFeed ─► SpectrumAnalyzer ─┐
 (copy only, no FFT)            (2048 mono samples)    (FFT in rAF)     (dB per column)   │
                                                                                          ▼
 bus / master peaks   ─meters─►  "meters" event   ──►  MeterTracker  ───────────────►  painters
 (read-and-clear)               (peaks, no samples)    (hold / decay)                     │
                                                                                          ▼
 EQ band parameters ───────────────────────────────►  eqCurve()  (theoretical response)  canvas / SVG
```

Two kinds of source, one drawing approach:

| source | gives you | used by |
| --- | --- | --- |
| **Sample tap** (`openAnalysisTap`) | a window of raw mono samples at the input or output of one effect | spectrum |
| **Peak meters** (`onMeters`) | linear peaks per bus and for the master (pre-limiter, post-limiter, gain reduction) | master level bar, clip dots |
| **Parameters** (no audio) | the current EQ bands | EQ response curve |

They are deliberately not merged. A sample tap cannot reproduce the master's pre-limiter level or the
limiter's gain reduction, and the meters are exact (every block contributes) where a tap is a
sampled window. The shared part is everything after the data arrives.

## What lives where

### webdsp (data and maths, no UI)

| file | role |
| --- | --- |
| `native/src/analysis_tap.h` | `AnalysisTap`: a 4096-float mono ring written on the audio thread, read as a 2048-sample window. `kMaxAnalysisTaps` = 4. |
| `native/src/fx_chain.h`, `bus.h`, `engine.h` | `FxChain::addTap(effectId, post, tap)` copies the block before (`post` false) or after (`post` true) an instance; `Engine::setAnalysisTap(slot, bus, effect, post)` owns the slots. |
| `native/src/api.cpp` | `webdsp_set_analysis_tap`, `webdsp_analysis_slots / window / written / snapshot`. |
| `src/worklet/engine-processor.ts` | every 16 blocks, for each active slot whose ring advanced, posts `{ type: "analysis", slot, samples }`. |
| `src/runtime/index.ts` | `openAnalysisTap(bus, effect, post)` → slot or `null`, `closeAnalysisTap(slot)`, `onAnalysis(fn)`, plus the existing `onMeters`. |
| `src/runtime/spectrum.ts` | `SpectrumAnalyzer` (Hann FFT, log-spaced columns, peak-per-column, instant rise / 40 dB/s fall), `SpectrumFeed` (owns a slot, defers the FFT to the frame), `spectrumPoints()` (x,y pairs). |
| `src/runtime/scale.ts` | `logFreqToUnit`, `dbToUnit`: the only two axis mappings. |
| `src/runtime/meters.ts` | `MeterTracker` (hold/decay/clip), `peakToUnit`, `METER_MIN_DB / MAX_DB / RED_DB`, the LED thresholds (now the bar's tick marks). |
| `src/runtime/eq.ts` | `eqCurve`, `eqBandResponseDb`: the theoretical EQ response, from parameters only. |

### Clients (drawing)

The two clients carry the same files (`webseq/src/viz/*.ts`, `webtracker/src/ui/viz/*.js`). Each
mirror says so in its header; change one, change the other.

| file | role |
| --- | --- |
| `canvasSurface` | Wraps a `<canvas>`: sizes it to its element at the device pixel ratio via `ResizeObserver`, exposes drawing units, `begin()` (set transform + clear), and a `dirty` flag set on resize. Drawing units are either fixed (match an SVG `viewBox`) or the element's CSS pixels. |
| `paint` | `paintSpectrum(ctx, pts, columns, height, {fill, stroke})` and `paintLevelBar(ctx, w, h, bar, style)`. Pure drawing from already-mapped geometry and colour strings; no audio or EQ knowledge. |
| `theme` | `cssColor(el, "--var", fallback)` and `withAlpha`: colours come from the app's CSS variables, read once at mount. |
| `useSpectrumCanvas` | The spectrum display: surface + `SpectrumFeed`s + painters in one rAF loop. |
| `levelMeterView` | The master level display: surface + `MeterTracker` + `paintLevelBar`. |

Both painters use the same picture: a filled area on a dB scale with a line on top, and a warning
colour for the danger zone. That is the "visual vocabulary"; a new display should look like a sibling.

### webvst (native host)

The plugin runs the same engine natively, so it must also feed the page. `EngineHost` handles
`set-analysis-tap` (slot, bus, effect, post), copies each active slot's window on the audio thread into
a per-slot buffer under a **seqlock** (odd sequence = being written), posts a small event, and the
message thread reads the buffer back and sends `{type:"analysis", slot, samples}`; `shim.js` turns
`samples` into a `Float32Array`, exactly as it does for `meters`. A test
(`engine_host_test.cpp`) enforces that every webdsp `HostCommand` is handled and that feeding the taps
allocates nothing on the audio thread.

## The EQ editor, composed

The editor is three stacked layers over one shared frequency/dB geometry
(`model/eqGeometry`, which uses `freqToX` / `dbToY` for both the curve and the handles):

```
 ┌────────────────────────────────────────────────┐  wrapper: position:relative, aspect-ratio 640/190
 │ <canvas>  live spectrum      pointer-events:none│  layer 1  what the audio contains
 │ <svg>     grid, band fills, EQ curve, handles   │  layer 2+3 what the EQ does / what you control
 └────────────────────────────────────────────────┘
```

1. **Spectrum (canvas, behind).** Two feeds on the same effect: the **output** (`post: true`) is the
   main fill and moves as bands are dragged; the **input** (`post: false`) is fainter behind it, so the
   gap between them is what the EQ is changing. Own dB scale (dBFS, `-90 … -10`), independent of the
   EQ's ±18 dB axis.
2. **EQ response curve (SVG).** `eqCurve(bands)`: the theoretical combined response and the per-band
   fills, recomputed when parameters change. It is never derived from audio.
3. **Band handles (SVG).** Unchanged interaction: drag = frequency/gain, wheel or shift-drag = Q/slope,
   double-click = on/off. Because the canvas has `pointer-events: none` and sits *behind* the SVG, the
   spectrum cannot intercept a gesture.

The canvas and SVG share the wrapper's fixed aspect ratio and the same 640×190 drawing units, so they
line up at any rendered size; there is no desktop-only size.

How the editor gets its audio: the FX panel passes a `spectrumTap()` callback that returns
`{ source: runtime, bus, effect }` or `null`. The hook **polls** it each frame until it returns a tap,
because the engine effect is created by the FX sync *after* the first render (webseq) or by an async
engine start (webtracker). Nothing is ever created speculatively.

## The master level meter

The old LED column is now a canvas bar from the same painters (`paintLevelBar`): fill from the bottom
on the shared dB scale (`-48 … 0` dBFS), the part above `METER_RED_DB` (-2 dBFS) in the danger colour, a
cap while clipping, and the old LED thresholds as faint tick marks. Data is still the engine's exact
peaks via `MeterTracker` (level falls 24 dB/s, clip stays lit 1.5 s). `useMixerMeters` runs one rAF loop
for the mixer: `LevelMeterView.draw()` for the bar, and `paintMeters()` for the DOM bits that stay DOM
(the master strip's `clipping` class and the per-track clip dots).

## Performance rules (these are what keep the playhead smooth)

Any new display must follow them; they are the reason the existing ones are cheap.

1. **No React state from a data stream.** Peaks and spectra update 20–50 times a second. The loop reads
   from refs/objects and writes to a canvas or toggles a class; it never calls `setState`.
2. **One rAF loop per display, started in an effect, cancelled on unmount.** A hidden tab or an
   unmounted view costs nothing (rAF pauses with the tab; a panel that is not open is not mounted, so it has no loop).
3. **Repaint only on change.** `SpectrumFeed.frame()` returns `true` only when the data changed (or is
   still decaying) and returns `false` once settled at the floor; the meter repaints only when the bar
   moved a pixel or the clip state flipped; resize sets `surface.dirty`. An idle display does no drawing.
4. **Heavy work in the frame, not the message handler.** The `onAnalysis` handler only stores the
   latest window; the FFT runs in `frame()`, at most once per animation frame, so a burst of messages
   coalesces and a hidden tab never computes it.
5. **No allocation per frame.** Analyzer buffers, the point array and the FFT tables are allocated once.
6. **Audio thread does the minimum.** A tap is one mono-sum copy of the block per open slot; nothing when
   no slot is open. The FFT, windowing, smoothing and drawing are all UI-side.
7. **Never poll the engine for the tap in React render.** The `resolve` callback runs inside the rAF
   loop (a map lookup), not in render.

Costs, for scale: 2 taps = two 2048-float copies per 16 blocks per message (8 KB each, ~23/s); one FFT of
2048 points per feed per frame at most.

## Adding a new display

1. **Pick the source.** Raw audio for frequency/waveform → a sample tap (`SpectrumFeed`, or read
   `onAnalysis` frames yourself for a scope). A level → `MeterTracker`/`peakToUnit`. A parameter-derived
   curve → compute it in a pure helper like `eqCurve`.
2. **Map to geometry in webdsp-side pure functions** (`logFreqToUnit`, `dbToUnit`, or a new helper with a
   test), so both clients get identical numbers.
3. **Draw with the shared pieces.** Create a `CanvasSurface`, call `begin()` only when something changed
   (`dirty` or new data), use or add a painter in `paint` (and add it to *both* clients), take colours
   from CSS variables with `cssColor`.
4. **Own the loop.** One rAF, started and stopped by the component's effect; follow the rules above.
5. **Layer, don't replace.** Put the canvas behind interactive SVG/DOM with `pointer-events: none`.
6. **Test the pure parts** (analyzer maths, geometry, painter calls with a recording fake context), not the
   canvas pixels. Examples: `test/spectrum.test.ts`, `webseq/test/vizPaint.test.ts`,
   `webtracker/test/vizPaint.test.js`.

Possible next consumers the current pieces already support: a master or per-track spectrum (tap a
Utility or any chain effect), a standalone analyser panel, compressor/filter input-vs-output views,
and, with a new painter, a waveform scope (the tap already delivers the raw window).

## Tunable constants

| what | where | value |
| --- | --- | --- |
| spectrum dB window | `EqCurveEditor` (`SPECTRUM_MIN_DB / MAX_DB`) | -90 … -10 dBFS |
| spectrum layer colours / opacity | `EqCurveEditor` (`main`, `input`) | output ≈ 0.14–0.22 fill, input ≈ 0.05–0.07 |
| columns, FFT size, fall rate | `SpectrumOptions` (`spectrum.ts`) | 160, 2048, 40 dB/s |
| frequency range | `SpectrumOptions` | 20 Hz – 20 kHz (matches the EQ axis) |
| analysis rate / window | `ANALYSIS_INTERVAL_BLOCKS` (worklet), `kAnalysisWindow` (native) | every 16 blocks, 2048 samples |
| tap slots | `kMaxAnalysisTaps`, `ANALYSIS_TAP_SLOTS` | 4 |
| meter range / danger zone | `meters.ts` | -48 … 0 dBFS, red from -2 |
| meter fall / clip hold | `MeterTracker` options | 24 dB/s, 1500 ms |

The dB window and opacities are first guesses (they work with real audio; a tuning pass is planned).

## Known limits

- Four tap slots engine-wide; an EQ editor uses two. A feed that finds none free stays empty rather than
  failing. webtracker's FX panel can show several EQs at once, so the third one will be blank.
- The spectrum is peak-per-column from a Hann FFT (a full-scale sine reads ≈ 0 dBFS), not RMS or octave
  bands.
- The tap is mono (L+R averaged) and sees the effect's input/output, not the master mix.
- Verified working with real audio by the maintainer; ranges, colours and smoothing are first-pass values and are due a tuning pass.

## Tests

`native/test/analysis_tap_test.cpp` (ring ordering, input vs output taps, slot limits),
`native/test/smoke.mjs` (ABI), `test/spectrum.test.ts` (analyzer, feed, axis maths, meter scale),
`test/meters.test.ts`, webseq `test/vizPaint.test.ts` + `test/meterView.test.ts`, webtracker
`test/vizPaint.test.js` + `test/meterView.test.js`, webvst `test/engine_host_test.cpp`.
