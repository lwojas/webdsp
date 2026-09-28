import { useEffect, useState } from "react";
import type { AudioRuntime } from "../../runtime";
import type { SampleMetadata, VoiceHandle } from "../../runtime/types";
import { Pad, type PadConfig } from "./Pad";

const PAD_COUNT = 16;

function defaultConfig(): PadConfig {
  return { sampleId: null, gain: 1.0, pitch: 0, start: 0, end: 1, loop: false, reverse: false };
}

interface Props {
  runtime: AudioRuntime;
  samples: SampleMetadata[];
}

// Pads are entirely a UI concept: this component owns pad->sample assignment and per-pad
// playback settings, and translates a click into a single AudioRuntime.trigger() call with
// generic TriggerParams. The runtime never sees a "pad index" — see ARCHITECTURE.md, "What
// belongs to the application".
export function PadGrid({ runtime, samples }: Props) {
  const [configs, setConfigs] = useState<PadConfig[]>(() =>
    Array.from({ length: PAD_COUNT }, defaultConfig),
  );
  const [activeVoiceByPad, setActiveVoiceByPad] = useState<Map<number, VoiceHandle>>(new Map());

  useEffect(() => {
    return runtime.onVoiceEnded((voice) => {
      setActiveVoiceByPad((prev) => {
        for (const [pad, v] of prev) {
          if (v === voice) {
            const next = new Map(prev);
            next.delete(pad);
            return next;
          }
        }
        return prev;
      });
    });
  }, [runtime]);

  function trigger(index: number) {
    const config = configs[index];
    const sample = samples.find((s) => s.id === config.sampleId);
    if (!sample || !config.sampleId) return;

    const voice = runtime.trigger({
      sampleId: config.sampleId,
      gain: config.gain,
      pitch: config.pitch,
      start: Math.floor(config.start * sample.length),
      end: Math.floor(config.end * sample.length),
      loop: config.loop,
      reverse: config.reverse,
    });
    setActiveVoiceByPad((prev) => new Map(prev).set(index, voice));
  }

  return (
    <section className="panel">
      <h2>Pads</h2>
      <div className="pad-grid">
        {configs.map((config, i) => (
          <Pad
            key={i}
            index={i}
            config={config}
            sample={samples.find((s) => s.id === config.sampleId)}
            samples={samples}
            active={activeVoiceByPad.has(i)}
            onTrigger={() => trigger(i)}
            onChange={(next) => setConfigs((prev) => prev.map((c, idx) => (idx === i ? next : c)))}
          />
        ))}
      </div>
    </section>
  );
}
