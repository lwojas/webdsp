import { useRef, useState } from "react";
import type { AudioRuntime } from "../../runtime";
import type { SampleMetadata } from "../../runtime/types";

interface Props {
  runtime: AudioRuntime;
  samples: SampleMetadata[];
  onLoaded: (sample: SampleMetadata) => void;
}

export function SampleLoader({ runtime, samples, onLoaded }: Props) {
  const inputRef = useRef<HTMLInputElement>(null);
  const [loading, setLoading] = useState(false);

  async function handleFiles(files: FileList | null) {
    if (!files || files.length === 0) return;
    setLoading(true);
    try {
      for (const file of Array.from(files)) {
        const buffer = await file.arrayBuffer();
        const meta = await runtime.loadSample(buffer, { name: file.name });
        onLoaded(meta);
      }
    } finally {
      setLoading(false);
      if (inputRef.current) inputRef.current.value = "";
    }
  }

  return (
    <section className="panel">
      <h2>Samples</h2>
      <input
        ref={inputRef}
        type="file"
        accept="audio/*"
        multiple
        disabled={loading}
        onChange={(e) => handleFiles(e.target.files)}
      />
      {loading && <p className="hint">Decoding…</p>}
      <ul className="sample-list">
        {samples.map((s) => (
          <li key={s.id}>
            <strong>{s.name}</strong>
            <span className="meta">
              {s.channels}ch · {s.sampleRate}Hz · {s.duration.toFixed(2)}s ·{" "}
              {(s.byteLength / 1024).toFixed(0)}KB
            </span>
          </li>
        ))}
        {samples.length === 0 && <li className="hint">No samples loaded yet.</li>}
      </ul>
    </section>
  );
}
