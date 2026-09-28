import { useEffect, useState } from "react";
import type { AudioRuntime } from "../../runtime";
import type { SampleId, SampleMetadata } from "../../runtime/types";
import { LookaheadPlayer } from "../../sequencing/lookaheadPlayer";
import { usePlayhead } from "../hooks/usePlayhead";
import { resolvePattern } from "../patterns/resolvePattern";
import { testPatterns, type PatternRole } from "../patterns/testPatterns";

const ROLES: PatternRole[] = ["kick", "snare", "hat", "perc"];
const patternNames = Object.keys(testPatterns);

interface Props {
  runtime: AudioRuntime;
  samples: SampleMetadata[];
}

// This is a *client* of the runtime, exactly like PadGrid — it has no special access. It
// compiles a role-based test pattern into generic ScheduledEvents (resolvePattern +
// LookaheadPlayer) and calls runtime.schedule()/cancelScheduled() like any sequencer would.
// See ARCHITECTURE.md, "How future sequencers can integrate".
export function SequencerPanel({ runtime, samples }: Props) {
  const [assignments, setAssignments] = useState<Record<PatternRole, SampleId | null>>({
    kick: null,
    snare: null,
    hat: null,
    perc: null,
  });
  const [patternName, setPatternName] = useState(patternNames[0]);
  const [bpm, setBpm] = useState(120);
  const [player, setPlayer] = useState<LookaheadPlayer | null>(null);
  const playheadStep = usePlayhead(player);

  const pattern = testPatterns[patternName];
  const stepDurationSeconds = 60 / bpm / 2; // eighth-note steps

  useEffect(() => () => player?.stop(), [player]);

  function play() {
    const resolved = resolvePattern(pattern, assignments);
    const p = new LookaheadPlayer(runtime, resolved, stepDurationSeconds, true);
    p.start();
    setPlayer(p);
  }

  function stop() {
    player?.stop();
    setPlayer(null);
  }

  return (
    <section className="panel">
      <h2>Sequencer (test client)</h2>
      <p className="hint">
        Assigns loaded samples to pattern roles, then compiles the selected pattern into
        scheduled runtime events. The engine never sees "kick" or "step 3" — only sampleId
        and time.
      </p>

      <div className="role-assign">
        {ROLES.map((role) => (
          <label key={role}>
            {role}
            <select
              value={assignments[role] ?? ""}
              disabled={!!player}
              onChange={(e) =>
                setAssignments((prev) => ({
                  ...prev,
                  [role]: e.target.value ? Number(e.target.value) : null,
                }))
              }
            >
              <option value="">(none)</option>
              {samples.map((s) => (
                <option key={s.id} value={s.id}>
                  {s.name}
                </option>
              ))}
            </select>
          </label>
        ))}
      </div>

      <label>
        Pattern
        <select value={patternName} disabled={!!player} onChange={(e) => setPatternName(e.target.value)}>
          {patternNames.map((name) => (
            <option key={name} value={name}>
              {name}
            </option>
          ))}
        </select>
      </label>

      <label>
        BPM {bpm}
        <input
          type="range" min={60} max={200} value={bpm} disabled={!!player}
          onChange={(e) => setBpm(Number(e.target.value))}
        />
      </label>

      <div className="transport">
        <button onClick={play} disabled={!!player}>Play</button>
        <button onClick={stop} disabled={!player}>Stop</button>
      </div>

      <div className="steps">
        {pattern.map((_, i) => (
          <span key={i} className={`step${i === playheadStep ? " step--current" : ""}`} />
        ))}
      </div>
    </section>
  );
}
