import { useEffect, useState } from "react";
import type { LookaheadPlayer } from "../../sequencing/lookaheadPlayer";

// Polls the player's current step via requestAnimationFrame — this is the one place React
// state is driven by audio timing, and it's explicitly allowed to be (project brief section
// 13): the playhead is a *visualization* of runtime state, derived every animation frame
// from the engine's own clock (LookaheadPlayer.getPlayheadStep() -> AudioRuntime.
// getCurrentTime()), not an input to it. Nothing here schedules audio or owns timing.
export function usePlayhead(player: LookaheadPlayer | null): number {
  const [step, setStep] = useState(-1);

  useEffect(() => {
    if (!player) {
      setStep(-1);
      return;
    }
    let raf: number;
    const loop = () => {
      setStep(player.getPlayheadStep());
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [player]);

  return step;
}
