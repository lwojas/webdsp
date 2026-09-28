import { useState } from "react";
import type { AudioRuntime } from "../../runtime";
import type { CaptureHandle, SampleMetadata } from "../../runtime/types";

interface Props {
  runtime: AudioRuntime;
  onCaptured: (meta: SampleMetadata) => void;
  lastCapture: SampleMetadata | null;
}

// Minimal proof of concept for project brief section 11: record the master bus, register
// the result as a brand new runtime Sample, play it back. See AudioRuntime.startCapture/
// stopCapture and native/src/capture.h for where this actually happens (never in this
// component — it only calls the public API, same as everything else in src/app).
export function CapturePanel({ runtime, onCaptured, lastCapture }: Props) {
  const [handle, setHandle] = useState<CaptureHandle | null>(null);
  const [busy, setBusy] = useState(false);

  function start() {
    setHandle(runtime.startCapture());
  }

  async function stop() {
    if (!handle) return;
    setBusy(true);
    try {
      const meta = await runtime.stopCapture(handle);
      onCaptured(meta);
    } finally {
      setHandle(null);
      setBusy(false);
    }
  }

  function playCapture() {
    if (lastCapture) runtime.trigger({ sampleId: lastCapture.id });
  }

  return (
    <section className="panel">
      <h2>Resample / capture (proof of concept)</h2>
      <p className="hint">
        Records the master bus's output. Trigger some pads while capturing, then stop to
        register the recording as a new sample you can play back like any other.
      </p>
      <div className="transport">
        <button onClick={start} disabled={!!handle || busy}>Start capture</button>
        <button onClick={stop} disabled={!handle || busy}>Stop capture</button>
        <button onClick={playCapture} disabled={!lastCapture}>Play captured sample</button>
      </div>
      {handle && <p className="hint">Recording…</p>}
      {lastCapture && (
        <p className="hint">
          Last capture: {lastCapture.duration.toFixed(2)}s, {lastCapture.channels}ch
        </p>
      )}
    </section>
  );
}
