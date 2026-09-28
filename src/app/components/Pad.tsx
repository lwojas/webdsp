import type { SampleMetadata } from "../../runtime/types";

export interface PadConfig {
  sampleId: number | null;
  gain: number;
  pitch: number;
  start: number; // fraction 0..1 of sample length
  end: number; // fraction 0..1 of sample length
  loop: boolean;
  reverse: boolean;
}

interface Props {
  index: number;
  config: PadConfig;
  sample: SampleMetadata | undefined;
  samples: SampleMetadata[];
  active: boolean;
  onTrigger: () => void;
  onChange: (config: PadConfig) => void;
}

export function Pad({ index, config, sample, samples, active, onTrigger, onChange }: Props) {
  return (
    <div className={`pad${active ? " pad--active" : ""}`}>
      <button
        className="pad__trigger"
        disabled={!config.sampleId}
        onClick={onTrigger}
        title={sample ? sample.name : "No sample assigned"}
      >
        {index + 1}
        <span className="pad__name">{sample?.name ?? "—"}</span>
      </button>

      <select
        value={config.sampleId ?? ""}
        onChange={(e) => onChange({ ...config, sampleId: e.target.value ? Number(e.target.value) : null })}
      >
        <option value="">(none)</option>
        {samples.map((s) => (
          <option key={s.id} value={s.id}>
            {s.name}
          </option>
        ))}
      </select>

      <label>
        Gain {config.gain.toFixed(2)}
        <input
          type="range" min={0} max={1.5} step={0.01} value={config.gain}
          onChange={(e) => onChange({ ...config, gain: Number(e.target.value) })}
        />
      </label>
      <label>
        Pitch {config.pitch > 0 ? "+" : ""}{config.pitch}st
        <input
          type="range" min={-24} max={24} step={1} value={config.pitch}
          onChange={(e) => onChange({ ...config, pitch: Number(e.target.value) })}
        />
      </label>
      <label>
        Start {(config.start * 100).toFixed(0)}%
        <input
          type="range" min={0} max={0.95} step={0.01} value={config.start}
          onChange={(e) => onChange({ ...config, start: Math.min(Number(e.target.value), config.end - 0.01) })}
        />
      </label>
      <label>
        End {(config.end * 100).toFixed(0)}%
        <input
          type="range" min={0.05} max={1} step={0.01} value={config.end}
          onChange={(e) => onChange({ ...config, end: Math.max(Number(e.target.value), config.start + 0.01) })}
        />
      </label>
      <div className="pad__toggles">
        <label>
          <input type="checkbox" checked={config.loop} onChange={(e) => onChange({ ...config, loop: e.target.checked })} />
          Loop
        </label>
        <label>
          <input type="checkbox" checked={config.reverse} onChange={(e) => onChange({ ...config, reverse: e.target.checked })} />
          Reverse
        </label>
      </div>
    </div>
  );
}
