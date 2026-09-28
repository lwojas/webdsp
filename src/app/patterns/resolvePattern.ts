import type { SampleId } from "../../runtime/types";
import type { Pattern } from "../../sequencing/pattern";
import type { PatternRole, RolePattern } from "./testPatterns";

/** Substitutes each role with whatever SampleId the user currently has assigned to it.
 * Notes whose role has no assignment are dropped (so playing a pattern before assigning
 * all four roles just plays fewer sounds, rather than erroring). */
export function resolvePattern(
  pattern: RolePattern,
  assignments: Record<PatternRole, SampleId | null>,
): Pattern {
  return pattern.map((step) =>
    step
      .filter((note) => assignments[note.role] !== null)
      .map((note) => ({
        sampleId: assignments[note.role] as SampleId,
        velocity: note.velocity,
        pitch: note.pitch,
      })),
  );
}
