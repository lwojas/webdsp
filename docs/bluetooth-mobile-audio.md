# Bluetooth and mobile playback: the buffer report

A report on why a webdsp client sounded erratic on a phone over Bluetooth, what fixed it, and what a client
should do. Found and fixed in EFEM8 (`efem`), then ported to Pulse8 and hybrid8 (`Omni8`). No change to
webdsp itself was needed: everything below is done by the client, using options webdsp already exposes.

## Symptom

On a phone with Bluetooth headphones, playback was erratic: crackling, uneven timing, dropouts. The same app was
clean on a desktop through wired audio or the speakers, and clean on the phone when Bluetooth was off. A second,
smaller symptom appeared once the sound was fixed: the playhead did not feel as smooth as without Bluetooth.

## Cause

`AudioRuntime.create()` builds its own `new AudioContext()` when no `audioContext` option is given. That asks the
browser for its default latency hint, `"interactive"`, the **smallest output buffer** the device will give.

A small buffer is right for a desktop with a wired output, and wrong for a phone over Bluetooth:

- **Bluetooth is jittery.** Audio goes over a compressed link (A2DP) that adds a delay of roughly 150-300 ms, and
  the delay is not constant. The platform audio stack must keep the link fed.
- **Phones are busy.** A mobile CPU shares time with the browser, the page and the OS, so the audio thread is more
  likely to miss a deadline.
- **A small buffer has no slack.** If the audio thread is late, the buffer runs dry (an *underrun*) and the output
  glitches. A larger buffer gives the same late thread room to catch up.

The scheduler also contributed. The client's lookahead scheduler queued only 120 ms of audio ahead of the clock,
so one late timer tick on a busy phone was enough to leave nothing queued.

## Fix 1: a larger buffer on touch devices

Create the context yourself and choose the latency hint by device, then hand it to webdsp:

```ts
const touch = matchMedia("(pointer: coarse) and (hover: none)").matches;
const audioContext = new AudioContext({ latencyHint: touch ? "playback" : "interactive" });
const runtime = await AudioRuntime.create({ audioContext, workletModuleUrl: defaultWorkletUrl, maxVoices: 32 });
```

- `"playback"` asks for a larger buffer that favours smooth output over low delay.
- Desktop keeps `"interactive"`, so there is no added delay where it is not needed.
- `(pointer: coarse) and (hover: none)` matches phones and tablets, where audio most often goes out over
  Bluetooth. It is a proxy: a laptop with Bluetooth headphones still gets the desktop profile.

The cost is a little more output delay on phones, in addition to Bluetooth's own. In testing this was accepted:
the audio became clean, and the delay is mostly the headphones'.

`"balanced"` sits between the two. It has less delay than `"playback"` and may be enough on a lightly loaded
device, but it was not needed here and was not tested.

## Fix 2: more scheduler lookahead on touch devices

A lookahead scheduler keeps a window of audio scheduled ahead of `currentTime`. On touch devices the window went
from 0.12 s to 0.25 s, so a late timer tick or a hiccup does not leave the buffer empty.

This only affects what the running sequence has already queued. Live triggers (pad hits, knob moves) are sent
with no time and play immediately, so the larger window does not delay them.

## Fix 3: a playhead that follows what is heard

After the sound was clean, the playhead still felt less smooth with Bluetooth on. Two causes:

1. **It ran ahead of the sound.** The playhead flipped when `currentTime` passed a step, but `currentTime` is the
   time of audio being *rendered*, not *heard*. With Bluetooth the two differ by the output latency.
2. **It moved in coarse jumps.** With a large buffer, `currentTime` advances in whole buffers, and the client
   updated the playhead from a 10 ms timer, not in step with screen frames.

The fix has two parts:

- **A "heard" clock.** `AudioContext.getOutputTimestamp()` returns the audio-clock time currently coming out of
  the device, paired with a `performance.now()` time. Extrapolating from it gives a time that includes the output
  delay and advances smoothly:

  ```ts
  const ts = ctx.getOutputTimestamp();
  const heard = ts.contextTime + (performance.now() - ts.performanceTime) / 1000;
  ```

  Guard it: use it only when it is not in the future and not more than about 0.5 s behind `currentTime`, and fall
  back to `currentTime - (outputLatency || baseLatency)` when the browser has no output timestamp.
- **Frame-driven updates.** The playhead and key lights update from `requestAnimationFrame`, once per screen
  frame, not from the scheduler timer. A hidden page gets no frames, so the timer still drives it there; this keeps
  anything that must keep running in the background (such as recording FX writes) going.

Scheduling still uses `currentTime`. Only what is *shown* uses the heard clock.

## Result

On a phone with cheap Bluetooth headphones, the audio went from erratic to clean after fixes 1 and 2. The
remaining delay is the headphones' own and cannot be removed by the page. Fix 3 made the playhead track the sound;
the author judged it smoother, and the remaining difference from wired audio is the Bluetooth delay itself.

This was checked by ear on one device and pair of headphones. The type-check and unit tests pass, but they do not
exercise the audio hardware.

## Sample rate

A sample-rate mismatch between the context and the Bluetooth device was suspected and ruled out as the cause. It
would affect tone or pitch, not timing or smoothness. A context created without a `sampleRate` option runs at the
device's own rate, so the page does no resampling of its own. Leave `sampleRate` unset unless you need a fixed
rate.

## Checklist for a webdsp client

- Create the `AudioContext` yourself and pass it as `audioContext`; pick `latencyHint` by device.
- Give the lookahead scheduler more headroom on touch devices (0.25 s worked).
- Show the playhead from the heard clock (`getOutputTimestamp`), updated per animation frame, with a timer fallback
  for hidden pages.
- Leave `sampleRate` unset.
- Test on a real phone with real Bluetooth headphones. Desktop and wired audio will not show the problem.

## Reference implementation

- EFEM8: `src/engine/audio.ts` (`Groovebox.create`, `isTouchDevice`, `heard`) and `src/engine/controller.ts`
  (`LOOKAHEAD`, `frame`). Commits `53945dd`, `c5ca0db`, `c78e0b4`.
- Pulse8 and hybrid8 (`Omni8`) carry the same four changes.

## Possible webdsp follow-ups

These are suggestions only and nothing here has been built:

- Default `AudioRuntime.create()` to a device-aware `latencyHint` when no context is passed.
- Expose a `heardTime` helper on the runtime so clients do not each reimplement the output-timestamp logic.
