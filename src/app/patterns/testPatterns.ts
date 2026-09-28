// Illustrative test patterns (project brief section 9): several arrays exercising
// one-shot, repeated, simultaneous, differing velocity/pitch, overlapping voices, and rapid
// retriggering. These reference abstract "roles" rather than concrete SampleIds (which
// don't exist until the user loads files) — see resolvePattern.ts for how a role is turned
// into whatever sample the user has currently assigned to it. This role indirection is
// app/test-UI glue, not a runtime concept.
export type PatternRole = "kick" | "snare" | "hat" | "perc";

export interface RoleNote {
  role: PatternRole;
  velocity?: number;
  pitch?: number;
}

export type RolePattern = RoleNote[][];

function repeat<T>(n: number, fn: (i: number) => T): T[] {
  return Array.from({ length: n }, (_, i) => fn(i));
}

export const testPatterns: Record<string, RolePattern> = {
  "One-shot kick": [[{ role: "kick", velocity: 1.0 }], [], [], []],

  "Repeated kick": repeat(8, (i) => (i % 2 === 0 ? [{ role: "kick" as const }] : [])),

  "Kick / snare alternating": repeat(8, (i) => {
    if (i % 4 === 0) return [{ role: "kick" as const }];
    if (i % 4 === 2) return [{ role: "snare" as const }];
    return [];
  }),

  "Simultaneous + velocities": [
    [{ role: "kick", velocity: 1.0 }, { role: "hat", velocity: 0.6 }],
    [{ role: "hat", velocity: 0.35 }],
    [{ role: "snare", velocity: 0.9 }, { role: "hat", velocity: 0.5 }],
    [{ role: "hat", velocity: 0.3 }],
    [{ role: "kick", velocity: 0.8 }, { role: "hat", velocity: 0.6 }],
    [{ role: "hat", velocity: 0.35 }],
    [{ role: "snare", velocity: 1.0 }, { role: "hat", velocity: 0.5 }, { role: "kick", velocity: 0.5 }],
    [{ role: "hat", velocity: 0.3 }],
  ],

  "Pitch sweep": repeat(8, (i) => [{ role: "perc" as const, pitch: -12 + i * 3 }]),

  "Polyphonic stress (4-voice hits)": repeat(16, (i) =>
    i % 2 === 0
      ? [{ role: "kick" as const }, { role: "snare" as const }, { role: "hat" as const }, { role: "perc" as const }]
      : [],
  ),

  "Rapid retrigger (hat)": repeat(32, () => [{ role: "hat" as const, velocity: 0.5 + Math.random() * 0.5 }]),
};
