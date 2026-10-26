#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>
#include "../src/dsp/adsr_envelope.h"
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the ADSR amplitude envelope (native/src/dsp/adsr_envelope.h) on its own and as wired
// into Voice (voice.h) through the Engine: lifecycle, retrigger, voice reuse, and
// allocation-freedom of process(). See ARCHITECTURE.md, "Envelopes".
using namespace webdsp;

namespace {
// These tests assert exact full-scale levels at the master output, so the master limiter
// (which would hold such levels to its ceiling) is switched off; it has its own tests.
void initRaw(Engine& e, int32_t maxVoices) {
  e.init(48000.0, 2, maxVoices);
  e.setBusParam(0, static_cast<int32_t>(NodeParam::LimiterEnable), 0.0f);
}
}  // namespace

// Counts heap allocations so a test can assert a region is allocation-free.
static long g_allocs = 0;
void* operator new(std::size_t n) {
  g_allocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr double kRate = 48000.0;
constexpr int32_t kBlock = 128;

AdsrParams params(float a, float d, float s, float r) {
  AdsrParams p;
  p.attack = a;
  p.decay = d;
  p.sustain = s;
  p.release = r;
  return p;
}

AdsrEnvelope makeEnv() {
  AdsrEnvelope e;
  e.configure(kRate);
  e.reset();
  return e;
}

void run(AdsrEnvelope& e, int frames, std::vector<float>* out = nullptr) {
  for (int i = 0; i < frames; i++) {
    const float v = e.next();
    if (out) out->push_back(v);
  }
}

// --- envelope in isolation ---

void testAttackRisesToPeak() {
  AdsrEnvelope e = makeEnv();
  const AdsrParams p = params(0.01f, 0.1f, 0.5f, 0.1f);  // 480-frame attack
  e.noteOn(p);
  std::vector<float> out;
  run(e, 480, &out);
  CHECK(out.front() == 0.0f);
  bool monotonic = true;
  for (size_t i = 1; i < out.size(); i++) monotonic = monotonic && out[i] >= out[i - 1];
  CHECK(monotonic);
  CHECK_NEAR(out[240], 0.5, 0.01);
  CHECK_NEAR(e.level(), 1.0, 1e-4);  // peak reached on the frame after the last attack sample
  CHECK(e.stage() == AdsrEnvelope::Stage::Decay);
}

void testDecayToSustainAndHold() {
  AdsrEnvelope e = makeEnv();
  e.noteOn(params(0.0f, 0.01f, 0.4f, 0.1f));  // instant attack, 480-frame decay
  std::vector<float> out;
  run(e, 480, &out);
  CHECK_NEAR(out.front(), 1.0, 1e-6);
  CHECK(e.stage() == AdsrEnvelope::Stage::Sustain);
  CHECK_NEAR(e.level(), 0.4, 1e-5);
  // Held: stays exactly at sustain for a long time.
  std::vector<float> held;
  run(e, 48000, &held);
  float lo = 1.0f, hi = 0.0f;
  for (float v : held) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  CHECK_NEAR(lo, 0.4, 1e-5);
  CHECK_NEAR(hi, 0.4, 1e-5);
}

void testReleaseReachesZeroAndDeactivates() {
  AdsrEnvelope e = makeEnv();
  const AdsrParams p = params(0.0f, 0.0f, 0.6f, 0.01f);  // 480-frame release
  e.noteOn(p);
  run(e, 100);
  e.noteOff(p);
  CHECK(e.stage() == AdsrEnvelope::Stage::Release);
  int frames = 0;
  while (e.isActive() && frames < 10000) {
    e.next();
    frames++;
  }
  CHECK(frames == 480);
  CHECK(e.level() == 0.0f);
  CHECK(e.next() == 0.0f);
}

void testParameterValuesChangeShape() {
  // Longer attack -> slower rise; lower sustain -> lower hold; longer release -> longer tail.
  AdsrEnvelope fast = makeEnv(), slow = makeEnv();
  fast.noteOn(params(0.001f, 0.0f, 1.0f, 0.01f));
  slow.noteOn(params(0.02f, 0.0f, 1.0f, 0.01f));
  run(fast, 100);
  run(slow, 100);
  CHECK(fast.level() > slow.level());

  AdsrEnvelope lo = makeEnv(), hi = makeEnv();
  lo.noteOn(params(0, 0.001f, 0.2f, 0.01f));
  hi.noteOn(params(0, 0.001f, 0.8f, 0.01f));
  run(lo, 200);
  run(hi, 200);
  CHECK_NEAR(lo.level(), 0.2, 1e-5);
  CHECK_NEAR(hi.level(), 0.8, 1e-5);

  auto releaseFrames = [](float seconds) {
    AdsrEnvelope e = makeEnv();
    const AdsrParams p = params(0, 0, 1.0f, seconds);
    e.noteOn(p);
    e.next();
    e.noteOff(p);
    int n = 0;
    while (e.isActive() && n < 1000000) {
      e.next();
      n++;
    }
    return n;
  };
  CHECK(releaseFrames(0.1f) == 4800);
  CHECK(releaseFrames(0.2f) == 9600);
}

void testNoteOffDuringAttackReleasesFromCurrentLevel() {
  AdsrEnvelope e = makeEnv();
  const AdsrParams p = params(0.01f, 0.0f, 1.0f, 0.01f);
  e.noteOn(p);
  run(e, 240);
  const float at = e.level();
  CHECK(at > 0.4f && at < 0.6f);
  e.noteOff(p);
  CHECK_NEAR(e.next(), at, 1e-6);  // no jump at note-off
  CHECK(e.stage() == AdsrEnvelope::Stage::Release);
}

void testExtremeTimesStayValid() {
  const float cases[] = {0.0f, 1e-9f, 1.0f / 48000.0f, 0.00002f, -1.0f, 1e9f, NAN, INFINITY};
  for (float t : cases) {
    AdsrEnvelope e = makeEnv();
    const AdsrParams p = params(t, t, 0.5f, t);
    e.noteOn(p);
    bool valid = true;
    for (int i = 0; i < 2000; i++) {
      const float v = e.next();
      valid = valid && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
    }
    e.noteOff(p);
    for (int i = 0; i < 2000; i++) {
      const float v = e.next();
      valid = valid && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
    }
    CHECK(valid);
  }
  // Zero release: voice-ending signal is immediate, not stuck.
  AdsrEnvelope e = makeEnv();
  const AdsrParams p = params(0, 0, 1, 0);
  e.noteOn(p);
  e.noteOff(p);
  CHECK(!e.isActive());
}

void testRetriggerDuringReleaseContinuesFromCurrentLevel() {
  AdsrEnvelope e = makeEnv();
  const AdsrParams p = params(0.01f, 0.0f, 1.0f, 0.02f);
  e.noteOn(p);
  run(e, 480);   // peak
  e.noteOff(p);
  run(e, 480);   // half way down the 960-frame release
  const float before = e.level();
  CHECK(before > 0.4f && before < 0.6f);
  e.noteOn(p);
  CHECK_NEAR(e.level(), before, 1e-6);  // no snap
  const float first = e.next();
  const float second = e.next();
  CHECK_NEAR(first, before, 1e-6);
  CHECK(second > first);                // rising again, no discontinuity
  CHECK(e.stage() == AdsrEnvelope::Stage::Attack);
  // Deterministic: the same sequence yields the same samples.
  AdsrEnvelope f = makeEnv();
  f.noteOn(p);
  run(f, 480);
  f.noteOff(p);
  run(f, 480);
  f.noteOn(p);
  CHECK(f.next() == first);
  CHECK(f.next() == second);
}

// --- voice integration through Engine ---

void commitConstant(Engine& e, int32_t id, int32_t frames, float level) {
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = level;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

void setEnv(Engine& e, int32_t sample, float a, float d, float s, float r) {
  e.setSampleParam(sample, static_cast<int32_t>(SampleParam::EnvAttack), a);
  e.setSampleParam(sample, static_cast<int32_t>(SampleParam::EnvDecay), d);
  e.setSampleParam(sample, static_cast<int32_t>(SampleParam::EnvSustain), s);
  e.setSampleParam(sample, static_cast<int32_t>(SampleParam::EnvRelease), r);
}

// Renders `blocks` blocks, appending channel 0 to `out` and ended voice ids to `ended`.
void render(Engine& e, int64_t& frame, int blocks, std::vector<float>* out,
            std::vector<int32_t>* ended = nullptr) {
  for (int b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    if (out) out->insert(out->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
    if (ended) ended->insert(ended->end(), e.endedVoices().begin(), e.endedVoices().end());
  }
}

void testVoiceFullLifecycle() {
  Engine e;
  initRaw(e, 8);
  commitConstant(e, 1, 480000, 1.0f);       // 10 s: never ends on its own
  setEnv(e, 1, 0.01f, 0.01f, 0.5f, 0.02f);  // 480 / 480 / hold / 960 frames

  int64_t frame = 0;
  std::vector<float> out;
  std::vector<int32_t> ended;
  e.trigger(7, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 8, &out, &ended);  // 1024 frames: through attack+decay, into sustain
  CHECK(out[0] == 0.0f);
  CHECK(out[240] > 0.4f && out[240] < 0.6f);          // mid-attack
  CHECK(out[480] > 0.99f);                             // at peak
  CHECK_NEAR(out[1000], 0.5, 1e-4);                    // sustaining
  CHECK(e.activeVoiceCount() == 1);
  CHECK(ended.empty());

  e.release(7);
  std::vector<float> tail;
  render(e, frame, 4, &tail, &ended);  // 512 frames, release is 960: still sounding
  CHECK(e.activeVoiceCount() == 1);    // alive while releasing
  CHECK(ended.empty());
  CHECK(tail[0] > 0.45f);
  CHECK(tail[500] > 0.1f && tail[500] < tail[0]);

  render(e, frame, 6, &tail, &ended);  // well past the end of release
  CHECK(e.activeVoiceCount() == 0);
  CHECK(ended.size() == 1 && ended[0] == 7);
  CHECK(tail.back() == 0.0f);
}

void testReleasingVoiceIsNotReusedWhileIdleVoiceAvailable() {
  Engine e;
  initRaw(e, 2);
  commitConstant(e, 1, 480000, 1.0f);
  setEnv(e, 1, 0.0f, 0.0f, 1.0f, 0.5f);  // long release

  int64_t frame = 0;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 2, nullptr);
  e.release(1);
  render(e, frame, 2, nullptr);
  e.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, false, false);  // must take the second (idle) voice
  render(e, frame, 1, nullptr);
  CHECK(e.activeVoiceCount() == 2);  // voice 1 still releasing, not stolen
}

void testStealRetriggerHasNoLevelJump() {
  Engine e;
  initRaw(e, 1);  // single voice: second trigger must steal
  commitConstant(e, 1, 480000, 1.0f);
  setEnv(e, 1, 0.01f, 0.0f, 1.0f, 0.01f);

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 2, nullptr);  // 256 frames into a 480-frame attack
  e.process(frame, kBlock);
  frame += kBlock;
  const float last = e.outputChannel(0)[kBlock - 1];
  CHECK(last > 0.5f && last < 1.0f);
  e.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  e.process(frame, kBlock);
  const float first = e.outputChannel(0)[0];
  CHECK(std::fabs(first - last) < 0.01f);  // continues from where it was (no snap to 0 or 1)
  CHECK(e.activeVoiceCount() == 1);
}

void testDurationAutoReleaseUsesEnvelopeRelease() {
  Engine e;
  initRaw(e, 4);
  commitConstant(e, 1, 480000, 1.0f);
  setEnv(e, 1, 0.0f, 0.0f, 1.0f, 0.02f);  // 960-frame release

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false, /*durationFrames=*/256);
  render(e, frame, 20, &out);
  CHECK_NEAR(out[255], 1.0, 1e-6);
  CHECK(out[256 + 480] > 0.4f && out[256 + 480] < 0.6f);
  CHECK(out[256 + 959] > 0.0f);
  CHECK(out[256 + 960] == 0.0f);
  CHECK(e.activeVoiceCount() == 0);
}

void testDefaultsPreserveLegacyBehaviour() {
  Engine e;
  initRaw(e, 4);
  commitConstant(e, 1, 480000, 0.5f);

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 2, &out);
  CHECK_NEAR(out[0], 0.5, 1e-6);  // instant full level
  e.release(1);
  render(e, frame, 6, &out);      // legacy 10 ms (480-frame) fade
  CHECK(e.activeVoiceCount() == 0);
}

void testProcessDoesNotAllocate() {
  Engine e;
  initRaw(e, 16);
  commitConstant(e, 1, 480000, 1.0f);
  setEnv(e, 1, 0.005f, 0.05f, 0.7f, 0.1f);
  int64_t frame = 0;
  for (int32_t v = 0; v < 8; v++) e.trigger(v, 1, 0, 1.0f, 1.0f, 0, -1, false, false, 2000);
  render(e, frame, 1, nullptr);

  const long before = g_allocs;
  render(e, frame, 200, nullptr);  // covers attack, decay, sustain, auto-release, completion
  e.release(0);
  setEnv(e, 1, 0.01f, 0.01f, 0.2f, 0.05f);
  render(e, frame, 50, nullptr);
  CHECK(g_allocs == before);
  CHECK(e.activeVoiceCount() == 0);
}


// --- review additions: frame-count convention, live-edit semantics, lifecycle regressions ---

// Convention: a stage of N frames renders N frames strictly short of its target; the target
// itself is first rendered on frame N. Release renders N non-zero frames, then 0.
void testStageFrameConvention() {
  AdsrEnvelope e = makeEnv();
  const AdsrParams p = params(0.001f, 0.001f, 0.5f, 0.001f);  // 48 frames each
  e.noteOn(p);
  std::vector<float> out;
  run(e, 48 + 48 + 4, &out);
  CHECK(out[0] == 0.0f);
  CHECK(out[47] < 1.0f);
  CHECK(out[48] == 1.0f);   // peak on frame 48 (= attack frames)
  CHECK(out[95] > 0.5f);
  CHECK(out[96] == 0.5f);   // sustain level on frame 48 + 48
  e.noteOff(p);
  std::vector<float> rel;
  run(e, 50, &rel);
  int nonzero = 0;
  for (float v : rel) nonzero += v > 0.0f;
  CHECK(nonzero == 48);
  CHECK(rel[0] == 0.5f);    // release starts at the held level, no jump
  CHECK(rel[48] == 0.0f);
  CHECK(!e.isActive());

  // Non-multiple of the sample period rounds to the nearest frame (0.00001 s = 0.48 -> min 1).
  AdsrEnvelope s = makeEnv();
  s.noteOn(params(0.00001f, 0, 1, 0.01f));
  CHECK(s.next() == 0.0f);
  CHECK(s.next() == 1.0f);
  AdsrEnvelope t = makeEnv();
  t.noteOn(params(0.0000313f, 0, 1, 0.01f));  // 1.5 frames -> 2
  t.next();
  CHECK(t.next() < 1.0f);
  CHECK(t.next() == 1.0f);

  // attack = 0 and decay = 0: first rendered frame is already at sustain.
  AdsrEnvelope z = makeEnv();
  z.noteOn(params(0, 0, 0.3f, 0.01f));
  CHECK(z.next() == 0.3f);
}

void testRetriggerInEveryStageIsContinuous() {
  const AdsrParams p = params(0.01f, 0.01f, 0.5f, 0.02f);  // 480 / 480 / hold / 960
  struct Case { int frames; bool off; };
  const Case cases[] = {{200, false}, {700, false}, {2000, false}, {2000, true}};  // A, D, S, R
  for (const Case& c : cases) {
    AdsrEnvelope e = makeEnv();
    e.noteOn(p);
    run(e, c.frames);
    if (c.off) {
      e.noteOff(p);
      run(e, 300);
    }
    const float before = e.level();
    e.noteOn(p);
    float prev = before;
    bool ok = true;
    for (int i = 0; i < 480; i++) {
      const float v = e.next();
      ok = ok && std::fabs(v - prev) < 0.01f && v >= prev - 1e-6f && v <= 1.0f;
      prev = v;
    }
    CHECK(ok);
    CHECK(prev >= before);
  }
}

void testRestartRetriggerReattacksFromSilence() {
  const AdsrParams p = params(0.01f, 0.0f, 1.0f, 0.02f);  // 480-frame attack, full sustain
  AdsrEnvelope e = makeEnv();
  e.noteOn(p);
  run(e, 2000);  // sitting at sustain, level 1
  CHECK(e.level() == 1.0f);
  e.noteOnRestart(p);
  // Declicked fade to zero (monotonic, ~2 ms), then a fresh attack from silence.
  float prev = e.level();
  int fade = 0;
  bool ok = true;
  while (e.stage() == AdsrEnvelope::Stage::Fade && fade < 1000) {
    const float v = e.next();
    ok = ok && v <= prev + 1e-6f;
    prev = v;
    fade++;
  }
  CHECK(ok);
  CHECK(fade > 50 && fade <= 100);
  CHECK(e.level() == 0.0f);
  CHECK(e.stage() == AdsrEnvelope::Stage::Attack);
  run(e, 240);
  CHECK(e.level() > 0.4f && e.level() < 0.6f);  // halfway up a 10 ms attack
  run(e, 400);
  CHECK(e.level() == 1.0f);
  // From idle it is exactly noteOn.
  AdsrEnvelope f = makeEnv();
  f.noteOnRestart(p);
  CHECK(f.stage() == AdsrEnvelope::Stage::Attack);
  // noteOff mid-fade releases from the current level and drops the pending note.
  AdsrEnvelope g = makeEnv();
  g.noteOn(p);
  run(g, 2000);
  g.noteOnRestart(p);
  run(g, 10);
  g.noteOff(p);
  CHECK(g.stage() == AdsrEnvelope::Stage::Release);
}

void testDuckThenRestartAttacksOnTheNoteFrame() {
  const AdsrParams p = params(0.01f, 0.0f, 1.0f, 0.02f);  // 480-frame attack, full sustain
  AdsrEnvelope e = makeEnv();
  e.noteOn(p);
  run(e, 2000);
  e.duck(96);
  // Monotonic fade that reaches silence on exactly the 96th frame, then holds.
  float prev = 1.0f;
  bool ok = true;
  for (int i = 0; i < 96; i++) {
    const float v = e.next();
    ok = ok && v <= prev + 1e-6f;
    prev = v;
  }
  CHECK(ok);
  CHECK(e.stage() == AdsrEnvelope::Stage::Held);
  CHECK(e.isActive());
  CHECK(e.next() == 0.0f);
  // The retrigger then attacks at once, from zero: no second fade.
  e.noteOnRestart(p);
  CHECK(e.stage() == AdsrEnvelope::Stage::Attack);
  CHECK(e.level() == 0.0f);
  run(e, 240);
  CHECK(e.level() > 0.4f && e.level() < 0.6f);
  // An idle or silent envelope ignores duck; a held one that is never retriggered ends.
  AdsrEnvelope idle = makeEnv();
  idle.duck(96);
  CHECK(idle.stage() == AdsrEnvelope::Stage::Idle);
  AdsrEnvelope h = makeEnv();
  h.noteOn(p);
  run(h, 2000);
  h.duck(10);
  run(h, 10 + kHeldMaxFrames + 1);
  CHECK(h.stage() == AdsrEnvelope::Stage::Idle);
  // noteOff during the duck releases from the current level (the voice does not end up held).
  AdsrEnvelope g = makeEnv();
  g.noteOn(p);
  run(g, 2000);
  g.duck(96);
  run(g, 10);
  g.noteOff(p);
  CHECK(g.stage() == AdsrEnvelope::Stage::Release);
}

void testLiveEditSemanticsThroughEngine() {
  auto make = [](Engine& e) {
    initRaw(e, 4);
    commitConstant(e, 1, 480000, 1.0f);
    setEnv(e, 1, 0.01f, 0.01f, 0.5f, 0.02f);
    e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  };
  Engine x, y;
  make(x);
  make(y);
  int64_t fx = 0, fy = 0;
  std::vector<float> ox, oy;
  render(x, fx, 2, &ox);  // 256 frames: mid-attack
  render(y, fy, 2, &oy);
  // Attack/decay/sustain are captured at note-on: editing them now must not touch this note.
  setEnv(x, 1, 0.5f, 0.5f, 0.1f, 0.02f);
  render(x, fx, 12, &ox);
  render(y, fy, 12, &oy);
  CHECK(ox == oy);  // bit-identical through attack, decay and into sustain
  CHECK_NEAR(ox.back(), 0.5, 1e-5);
  // Release is read at note-off: a longer release now applies to the held note.
  x.setSampleParam(1, static_cast<int32_t>(SampleParam::EnvRelease), 0.04f);
  x.release(1);
  y.release(1);
  render(x, fx, 9, nullptr);  // 1152 frames: Y's 960-frame release is over, X's 1920 is not
  render(y, fy, 9, nullptr);
  CHECK(x.activeVoiceCount() == 1);
  CHECK(y.activeVoiceCount() == 0);
  // And a note triggered afterwards picks up the edited A/D/S.
  x.stop(1);
  x.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  std::vector<float> fresh;
  render(x, fx, 1, &fresh);
  CHECK(fresh[100] < 0.01f);  // 0.5 s attack: barely risen after 100 frames
}

void testZeroReleaseThroughEngine() {
  Engine e;
  initRaw(e, 4);
  commitConstant(e, 1, 480000, 1.0f);
  setEnv(e, 1, 0.0f, 0.0f, 1.0f, 0.0f);
  int64_t frame = 0;
  std::vector<int32_t> ended;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 2, nullptr, &ended);
  e.release(1);
  render(e, frame, 1, nullptr, &ended);
  CHECK(e.activeVoiceCount() == 0);
  CHECK(ended.size() == 1 && ended[0] == 1);
}

void testEndedReportedOnceAndNoStaleEventAfterSteal() {
  Engine e;
  initRaw(e, 1);  // one voice: every trigger steals
  commitConstant(e, 1, 480000, 1.0f);
  setEnv(e, 1, 0.0f, 0.0f, 1.0f, 0.02f);

  int64_t frame = 0;
  std::vector<int32_t> ended;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 2, nullptr, &ended);
  e.release(1);
  render(e, frame, 2, nullptr, &ended);   // mid-release (512 of 960 frames)
  CHECK(e.activeVoiceCount() == 1 && ended.empty());

  e.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, false, false);  // steals the releasing voice
  e.release(1);                                          // stale handle: must be a no-op
  std::vector<float> out;
  render(e, frame, 20, &out, &ended);
  CHECK_NEAR(out[100], 1.0, 1e-6);  // voice 2 still sustaining despite release(1), no stale release
  CHECK(ended.empty());
  CHECK(e.activeVoiceCount() == 1);

  e.release(2);
  render(e, frame, 20, nullptr, &ended);
  CHECK(ended.size() == 1 && ended[0] == 2);  // exactly once, only the live id, never id 1
  render(e, frame, 20, nullptr, &ended);
  CHECK(ended.size() == 1);
  CHECK(e.activeVoiceCount() == 0);
}

void testStopLeavesNoStaleEnvelope() {
  Engine e;
  initRaw(e, 1);
  commitConstant(e, 1, 480000, 1.0f);
  setEnv(e, 1, 0.01f, 0.0f, 1.0f, 0.02f);
  int64_t frame = 0;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 8, nullptr);  // at full level
  e.stop(1);
  std::vector<float> out;
  e.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, false, false);  // reuses the stopped voice
  render(e, frame, 1, &out);
  CHECK(out[0] == 0.0f);  // fresh attack from silence, not the stopped voice's old level
}

}  // namespace

int main() {
  testAttackRisesToPeak();
  testDecayToSustainAndHold();
  testReleaseReachesZeroAndDeactivates();
  testParameterValuesChangeShape();
  testNoteOffDuringAttackReleasesFromCurrentLevel();
  testExtremeTimesStayValid();
  testRetriggerDuringReleaseContinuesFromCurrentLevel();
  testVoiceFullLifecycle();
  testReleasingVoiceIsNotReusedWhileIdleVoiceAvailable();
  testStealRetriggerHasNoLevelJump();
  testDurationAutoReleaseUsesEnvelopeRelease();
  testDefaultsPreserveLegacyBehaviour();
  testProcessDoesNotAllocate();
  testStageFrameConvention();
  testRetriggerInEveryStageIsContinuous();
  testRestartRetriggerReattacksFromSilence();
  testDuckThenRestartAttacksOnTheNoteFrame();
  testLiveEditSemanticsThroughEngine();
  testZeroReleaseThroughEngine();
  testEndedReportedOnceAndNoStaleEventAfterSteal();
  testStopLeavesNoStaleEnvelope();
  std::printf("adsr_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
