import { useState } from "react";
import type { SampleMetadata } from "../runtime/types";
import { CapturePanel } from "./components/CapturePanel";
import { Diagnostics } from "./components/Diagnostics";
import { PadGrid } from "./components/PadGrid";
import { SampleLoader } from "./components/SampleLoader";
import { SequencerPanel } from "./components/SequencerPanel";
import { useAudioRuntime } from "./hooks/useAudioRuntime";

// This component tree is the *entire* application-specific layer. It is the only thing
// that would be thrown away and replaced by a tracker, step sequencer, or MIDI instrument
// UI later — none of src/runtime, src/worklet, or native/ change. See ARCHITECTURE.md.
export function App() {
  const { runtime, capabilities, error, init } = useAudioRuntime();
  const [samples, setSamples] = useState<SampleMetadata[]>([]);
  const [lastCapture, setLastCapture] = useState<SampleMetadata | null>(null);

  function addSample(meta: SampleMetadata) {
    setSamples((prev) => [...prev.filter((s) => s.id !== meta.id), meta]);
  }

  if (!runtime || !capabilities) {
    return (
      <div className="boot">
        <h1>webdsp</h1>
        <p className="hint">
          Standalone audio engine test harness. This page is only a client of the runtime —
          nothing plays until the audio context starts, which browsers require a user
          gesture for.
        </p>
        <button onClick={init}>Start audio engine</button>
        {error && <p className="error">{error}</p>}
      </div>
    );
  }

  return (
    <div className="app">
      <h1>webdsp</h1>
      <div className="grid">
        <SampleLoader runtime={runtime} samples={samples} onLoaded={addSample} />
        <Diagnostics runtime={runtime} capabilities={capabilities} samples={samples} />
        <PadGrid runtime={runtime} samples={samples} />
        <SequencerPanel runtime={runtime} samples={samples} />
        <CapturePanel
          runtime={runtime}
          lastCapture={lastCapture}
          onCaptured={(meta) => {
            addSample(meta);
            setLastCapture(meta);
          }}
        />
      </div>
    </div>
  );
}
