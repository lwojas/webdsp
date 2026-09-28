var __defProp = Object.defineProperty;
var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
var __publicField = (obj, key, value) => __defNormalProp(obj, typeof key !== "symbol" ? key + "" : key, value);

// src/sequencing/pattern.ts
function compileStep(step, time) {
  return step.map((note) => ({
    time,
    sampleId: note.sampleId,
    gain: note.velocity ?? 1,
    pitch: note.pitch
  }));
}
function compilePattern(pattern, opts) {
  const loops = opts.loops ?? 1;
  const patternDuration = pattern.length * opts.stepDurationSeconds;
  const events = [];
  for (let loop = 0; loop < loops; loop++) {
    pattern.forEach((step, stepIndex) => {
      const time = opts.startTime + loop * patternDuration + stepIndex * opts.stepDurationSeconds;
      events.push(...compileStep(step, time));
    });
  }
  return events;
}

// src/sequencing/lookaheadPlayer.ts
var LookaheadPlayer = class {
  constructor(runtime, pattern, stepDurationSeconds, loop = true, lookaheadSeconds = 0.15, tickIntervalMs = 25) {
    __publicField(this, "runtime", runtime);
    __publicField(this, "pattern", pattern);
    __publicField(this, "stepDurationSeconds", stepDurationSeconds);
    __publicField(this, "loop", loop);
    __publicField(this, "lookaheadSeconds", lookaheadSeconds);
    __publicField(this, "tickIntervalMs", tickIntervalMs);
    __publicField(this, "timerId", null);
    __publicField(this, "nextStepIndex", 0);
    __publicField(this, "loopStartTime", 0);
  }
  start(startAt) {
    this.loopStartTime = startAt ?? this.runtime.getCurrentTime() + 0.05;
    this.nextStepIndex = 0;
    this.timerId = setInterval(() => this.tick(), this.tickIntervalMs);
    this.tick();
  }
  stop() {
    if (this.timerId !== null) {
      clearInterval(this.timerId);
      this.timerId = null;
    }
    this.runtime.cancelScheduled();
  }
  isPlaying() {
    return this.timerId !== null;
  }
  /** Current step index for playhead visualization. Purely derived from the engine's own
   * clock (getCurrentTime()) — never the timer above — so a slow/throttled UI thread never
   * desyncs audio, only the visual playhead's redraw rate. Returns -1 before playback
   * has audibly started (e.g. during the initial scheduling lead-in). */
  getPlayheadStep() {
    if (this.timerId === null) return -1;
    const elapsed = this.runtime.getCurrentTime() - this.loopStartTime;
    if (elapsed < 0) return -1;
    const step = Math.floor(elapsed / this.stepDurationSeconds);
    return this.loop ? step % this.pattern.length : Math.min(step, this.pattern.length - 1);
  }
  tick() {
    const horizon = this.runtime.getCurrentTime() + this.lookaheadSeconds;
    while (this.stepTime(this.nextStepIndex) < horizon) {
      if (!this.loop && this.nextStepIndex >= this.pattern.length) {
        this.stop();
        return;
      }
      const patternIndex = this.nextStepIndex % this.pattern.length;
      const time = this.stepTime(this.nextStepIndex);
      const notes = this.pattern[patternIndex];
      if (notes.length > 0) this.runtime.schedule(compileStep(notes, time));
      this.nextStepIndex++;
    }
  }
  stepTime(index) {
    return this.loopStartTime + index * this.stepDurationSeconds;
  }
};
export {
  LookaheadPlayer,
  compilePattern,
  compileStep
};
//# sourceMappingURL=sequencing.js.map