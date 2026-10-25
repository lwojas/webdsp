// The two axis mappings every level/frequency display shares (spectrum, level meters, and any
// future graph): where a value sits between the ends of a scale, as 0..1 (not clamped). Pure
// maths; clients multiply by their own width/height. Keeping them in one place means every
// visualisation reads dB the same way.

/** 0..1 position of `hz` on a log axis from `minHz` to `maxHz` (not clamped). */
export function logFreqToUnit(hz: number, minHz: number, maxHz: number): number {
  return Math.log(hz / minHz) / Math.log(maxHz / minHz);
}

/** 0..1 position of `db` between `minDb` (0) and `maxDb` (1) (not clamped). */
export function dbToUnit(db: number, minDb: number, maxDb: number): number {
  return (db - minDb) / (maxDb - minDb);
}
