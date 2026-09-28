import { useEffect, useState } from "react";
import type { AudioRuntime } from "../../runtime";
import type { RuntimeCapabilities, RuntimeDiagnostics, SampleMetadata } from "../../runtime/types";

interface Props {
  runtime: AudioRuntime;
  capabilities: RuntimeCapabilities;
  samples: SampleMetadata[];
}

// Diagnostics are pushed by the worklet every ~20 render quanta (see engine-processor.ts) —
// a rate chosen to be useful for a stress-test readout without turning every audio block
// into a React render. This is the one place the UI polls engine internals; it never
// influences playback.
export function Diagnostics({ runtime, capabilities, samples }: Props) {
  const [diag, setDiag] = useState<RuntimeDiagnostics>(runtime.getDiagnostics());

  useEffect(() => runtime.onDiagnostics(setDiag), [runtime]);

  function stressTest() {
    if (samples.length === 0) return;
    const voiceCount = capabilities.maxVoices + 16; // deliberately exceed the pool to exercise voice stealing
    for (let i = 0; i < voiceCount; i++) {
      const sample = samples[i % samples.length];
      runtime.trigger({
        sampleId: sample.id,
        gain: 0.4 + Math.random() * 0.4,
        pitch: Math.floor(Math.random() * 24) - 12,
      });
    }
  }

  return (
    <section className="panel">
      <h2>Diagnostics</h2>
      <dl className="diagnostics">
        <dt>Active voices</dt>
        <dd>{diag.activeVoices} / {capabilities.maxVoices}</dd>
        <dt>Loaded samples</dt>
        <dd>{diag.loadedSamples}</dd>
        <dt>Sample memory</dt>
        <dd>{(diag.sampleMemoryBytes / 1024 / 1024).toFixed(2)} MB</dd>
        <dt>Sample rate</dt>
        <dd>{capabilities.sampleRate} Hz</dd>
        <dt>Output channels</dt>
        <dd>{capabilities.outputChannels}</dd>
      </dl>
      <button onClick={stressTest} disabled={samples.length === 0}>
        Stress test: fire {capabilities.maxVoices + 16} voices at once
      </button>
    </section>
  );
}
