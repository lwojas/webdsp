# Spectrum analysis (display only)

A live spectrum for client UIs, first used behind the EQ curve. webdsp does the minimum on the
audio thread and exposes plain data; clients draw. There is no UI code in webdsp.

```
engine tap slot (copy of an effect's input OR output)  native/src/analysis_tap.h, FxChain::addTap
   -> worklet posts the latest 2048 samples ~23x/s    "analysis" event per slot, only if new audio arrived
   -> AudioRuntime.onAnalysis(frame)
   -> SpectrumFeed   (stores the window; FFT once per animation frame)       src/runtime/spectrum.ts
   -> SpectrumAnalyzer.db  (dBFS per log-spaced column, rises instantly, falls 40 dB/s)
   -> spectrumPoints(...)  (x,y pairs: log frequency / dB range)  -> client canvas
```

## API

```ts
const analyzer = new SpectrumAnalyzer({ columns: 160, minHz: 20, maxHz: 20000 });
const feed = new SpectrumFeed(runtime, bus, effectId, /* post */ true, analyzer);  // opens a tap slot
// requestAnimationFrame: if (feed.frame(now)) { spectrumPoints(analyzer, w, h, -90, -10, pts); /* stroke pts */ }
feed.dispose();                           // closes the slot
```

`post` false taps the effect's INPUT, true its OUTPUT (a bypassed effect's output is its input). The
EQ editors open two feeds on the same effect: output as the main fill, input fainter behind it, so the
gap between them is what the EQ changes. Underneath, `runtime.openAnalysisTap(bus, effect, post)`
returns a slot (or null if all `ANALYSIS_TAP_SLOTS` = 4 are in use) and `closeAnalysisTap(slot)` frees it.

`logFreqToUnit` / `dbToUnit` are the two axis mappings, usable on their own. The analyzer knows
nothing about EQ; any consumer that can name a bus + effect id can show a spectrum.

## Shared visual vocabulary (clients)

Every live dB display draws the same way, so new ones are a small step, not a new pattern:

| piece | role | webseq | webtracker |
| --- | --- | --- | --- |
| scale maths | `logFreqToUnit`, `dbToUnit`, `peakToUnit` (webdsp) | | |
| `CanvasSurface` | size, hi-dpi, clear, `dirty` on resize | `src/viz/canvasSurface.ts` | `src/ui/viz/canvasSurface.js` |
| painters | `paintSpectrum`, `paintLevelBar` (area + line, red danger zone, clip cap) | `src/viz/paint.ts` | `src/ui/viz/paint.js` |
| theme | colours from CSS variables, read at mount | `src/viz/theme.ts` | `src/ui/viz/theme.js` |
| displays | EQ spectrum hook, master `LevelMeterView` | `src/viz/` | `src/ui/viz/` |

A display owns its rAF loop, repaints only when its data changed, and never touches React state.
The two clients keep these files in step (each mirror says so in its header); only colours differ.

## Cost and safety

- Audio thread: one mono-sum copy of each block into a 4096-float ring per open slot, only for the
  tapped instance, nothing when no slot is open. No FFT, no allocation.
- Messaging: nothing is posted unless a tap is set and the ring advanced (an idle/dormant bus
  stays quiet); 8 KB per event.
- UI thread: the FFT (2048 points, Hann, preallocated) runs in `SpectrumFeed.frame()`, at most
  once per rAF, so hidden tabs/unmounted views cost nothing. An idle display stops redrawing.

## Limits

- Four tap slots engine-wide. Each feed owns a slot and ignores frames from the others, so displays do
  not mix; a feed that finds none free just stays empty. An EQ editor uses two, so at most two EQ
  editors can be fully live at once (webtracker's FX panel can show several).
- Level is a peak-per-column of a Hann-windowed FFT (a full-scale sine reads about 0 dBFS), not
  an RMS/octave-band analyser.

Tests: `native/test/analysis_tap_test.cpp`, `native/test/smoke.mjs`, `test/spectrum.test.ts`.
Clients: webseq `hooks/useSpectrumCanvas.ts`, webtracker `ui/useSpectrumCanvas.js` (a canvas
behind the SVG; the SVG keeps all interaction).
