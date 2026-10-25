# Spectrum analysis (display only)

A live spectrum for client UIs, first used behind the EQ curve. webdsp does the minimum on the
audio thread and exposes plain data; clients draw. There is no UI code in webdsp.

```
engine tap (copy of one effect's input, mono)        native/src/analysis_tap.h, FxChain::setTap
   -> worklet posts the latest 2048 samples ~23x/s   "analysis" event, only if new audio arrived
   -> AudioRuntime.onAnalysis(samples, sampleRate)
   -> SpectrumFeed   (stores the window; FFT once per animation frame)       src/runtime/spectrum.ts
   -> SpectrumAnalyzer.db  (dBFS per log-spaced column, rises instantly, falls 40 dB/s)
   -> spectrumPoints(...)  (x,y pairs: log frequency / dB range)  -> client canvas
```

## API

```ts
runtime.setAnalysisTap(bus, effectId);   // input of that effect (works while bypassed); null = off
const analyzer = new SpectrumAnalyzer({ columns: 160, minHz: 20, maxHz: 20000 });
const feed = new SpectrumFeed(runtime, bus, effectId, analyzer);   // sets the tap, subscribes
// requestAnimationFrame: if (feed.frame(now)) { spectrumPoints(analyzer, w, h, -90, -10, pts); /* stroke pts */ }
feed.dispose();                           // unsubscribes and turns the tap off
```

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

- Audio thread: one mono-sum copy of each block into a 4096-float ring, only for the tapped
  instance, nothing when no tap is set. No FFT, no allocation.
- Messaging: nothing is posted unless a tap is set and the ring advanced (an idle/dormant bus
  stays quiet); 8 KB per event.
- UI thread: the FFT (2048 points, Hann, preallocated) runs in `SpectrumFeed.frame()`, at most
  once per rAF, so hidden tabs/unmounted views cost nothing. An idle display stops redrawing.

## Limits

- One tap at a time engine-wide: a second `setAnalysisTap` moves it. Each "analysis" event carries
  its `bus` and `effect`, and a `SpectrumFeed` ignores frames that are not its own, so a display
  whose tap was taken over goes quiet instead of showing another effect's audio.
  `releaseAnalysisTap` (used by `dispose()`) switches the tap off only if it is still that
  display's. Two EQ editors open at once therefore means only the most recent one is live; real
  simultaneous taps would need several tap slots in the engine.
- The tap is the effect's INPUT (pre-processing). A post-effect tap would be the next effect's
  input or a tap on the chain output; not added.
- Level is a peak-per-column of a Hann-windowed FFT (a full-scale sine reads about 0 dBFS), not
  an RMS/octave-band analyser.

Tests: `native/test/analysis_tap_test.cpp`, `native/test/smoke.mjs`, `test/spectrum.test.ts`.
Clients: webseq `hooks/useSpectrumCanvas.ts`, webtracker `ui/useSpectrumCanvas.js` (a canvas
behind the SVG; the SVG keeps all interaction).
