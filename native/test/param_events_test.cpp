#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Scheduled parameter events (param_events.h through Engine::scheduleParam): steps, linear ramps,
// base-under-LFO, cancel and replace semantics, ordering before notes. docs/scheduled-params.md.
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kUtil = static_cast<int32_t>(FxKind::Utility);
constexpr int32_t kGain = static_cast<int32_t>(NodeParam::UtilityGain);
constexpr int32_t kEffect = static_cast<int32_t>(ParamTarget::Effect);
constexpr int32_t kInstr = static_cast<int32_t>(ParamTarget::Instrument);
constexpr int32_t kSample = static_cast<int32_t>(ParamTarget::Sample);
constexpr int32_t kBass = static_cast<int32_t>(InstrumentKind::Bass);
constexpr int32_t kCutoff = static_cast<int32_t>(BassParam::Cutoff);

struct Rig {
  Engine e;
  int64_t frame = 0;
  Rig() {
    e.init(kRate, 2, 8);
    e.addEffect(0, 1, kUtil);
    e.setEffectParam(0, 1, kGain, 0.0f);
  }
  void blocks(int32_t n) {
    for (int32_t i = 0; i < n; i++) {
      e.process(frame, kBlock);
      frame += kBlock;
    }
  }
  float gain() { return e.effectBaseParam(0, 1, kGain); }
  void step(int64_t at, float v) { e.scheduleParam({at, kEffect, 0, 1, kGain, v, 0.0f, false, 0}); }
  void ramp(int64_t at, float to, int32_t frames, bool hasFrom = false, float from = 0.0f) {
    e.scheduleParam({at, kEffect, 0, 1, kGain, to, from, hasFrom, frames});
  }
};

void testStepLandsInItsBlock() {
  Rig r;
  r.step(kBlock * 3 + 5, -12.0f);
  r.blocks(3);
  CHECK(r.gain() == 0.0f);  // block 3 has not been processed yet
  CHECK(r.e.pendingParamEvents() == 1);
  r.blocks(1);
  CHECK_NEAR(r.gain(), -12.0, 1e-6);  // applied at the start of the block containing the frame
  CHECK(r.e.pendingParamEvents() == 0);
}

void testRampIsLinearAndEndsExactly() {
  Rig r;
  const int32_t frames = kBlock * 40;
  r.ramp(0, -40.0f, frames, true, 0.0f);
  float prev = 1e9f;
  double maxDev = 0;
  for (int32_t b = 0; b < 44; b++) {
    r.blocks(1);
    const double expect = -40.0 * std::min(1.0, (double)(b * kBlock) / frames);
    maxDev = std::max(maxDev, std::fabs(r.gain() - expect));
    CHECK(r.gain() <= prev + 1e-6f);  // monotonic
    prev = r.gain();
  }
  CHECK(maxDev < 1e-3);
  CHECK(r.gain() == -40.0f);  // exact end value, not an approximation
  CHECK(r.e.activeParamRamps() == 0);
}

void testRampWithoutFromStartsAtCurrentBase() {
  Rig r;
  r.e.setEffectParam(0, 1, kGain, -10.0f);
  r.ramp(kBlock * 2, -30.0f, kBlock * 10);
  r.blocks(2);
  CHECK(r.gain() == -10.0f || std::fabs(r.gain() + 10.0f) < 0.1f);  // starts where it was
  r.blocks(20);
  CHECK(r.gain() == -30.0f);
}

void testFromIsIndependentOfPriorState() {
  Rig r;
  r.e.setEffectParam(0, 1, kGain, -55.0f);
  r.ramp(0, 6.0f, kBlock * 8, true, 0.0f);
  r.blocks(1);
  CHECK(std::fabs(r.gain() - 0.0f) < 1.0f);  // began at `from`, not at -55
  r.blocks(10);
  CHECK(r.gain() == 6.0f);
}

void testLateEventCatchesUp() {
  Rig r;
  r.blocks(100);  // clock is at block 100
  r.ramp(0, -20.0f, kBlock * 200, true, 0.0f);  // started "in the past": 100 of 200 blocks ago
  r.blocks(1);
  CHECK(r.gain() < -9.0f && r.gain() > -11.5f);  // already about halfway, not restarted from 0
}

void testReplaceAndDirectWriteCancel() {
  Rig r;
  r.ramp(0, -40.0f, kBlock * 100, true, 0.0f);
  r.blocks(10);
  CHECK(r.e.activeParamRamps() == 1);
  r.ramp(r.frame, 10.0f, kBlock * 4, true, -5.0f);  // same key: replaces
  r.blocks(1);
  CHECK(r.e.activeParamRamps() == 1);
  r.blocks(6);
  CHECK(r.gain() == 10.0f);
  CHECK(r.e.activeParamRamps() == 0);

  r.ramp(r.frame, -50.0f, kBlock * 100, true, 0.0f);
  r.blocks(5);
  r.e.setEffectParam(0, 1, kGain, -3.0f);  // a direct write wins and ends the ramp
  CHECK(r.e.activeParamRamps() == 0);
  r.blocks(20);
  CHECK(r.gain() == -3.0f);
}

void testCancelScheduledDropsQueuedEventsButNotRunningRamps() {
  Rig r;
  r.step(kBlock * 50, -9.0f);
  r.ramp(0, -20.0f, kBlock * 100, true, 0.0f);
  r.blocks(2);
  r.e.cancelScheduled(kBlock * 10);
  CHECK(r.e.pendingParamEvents() == 0);
  CHECK(r.e.activeParamRamps() == 1);  // already running: keeps going until a direct write or its end
  r.blocks(60);
  CHECK(r.gain() < -5.0f);
  r.e.setEffectParam(0, 1, kGain, 0.0f);  // what an app's restore-on-stop does
  r.blocks(60);
  CHECK(r.gain() == 0.0f);
}

void testRampRidesUnderLfo() {
  // The ramp moves the BASE; the LFO keeps adding on top (effective = base + LFO throughout).
  Rig r;
  LfoConfig l;
  l.enabled = true;
  l.target = kGain;
  l.waveform = LfoWaveform::Square;
  l.amount = 0.1f;
  l.rateHz = 0.5f;
  CHECK(r.e.setEffectLfo(0, 1, 0, l));
  r.ramp(0, -40.0f, kBlock * 300, true, -10.0f);
  for (int32_t b = 0; b < 320; b++) {
    r.blocks(1);
    const double eff = r.e.effectEffectiveParam(0, 1, kGain);
    const double base = r.gain();
    if (b > 5) CHECK_NEAR(std::fabs(eff - base), 3.6, 0.6);  // 0.1 of a 72 dB range
  }
  CHECK(r.gain() == -40.0f);
}

void testInstrumentAndSampleTargets() {
  Engine e;
  e.init(kRate, 2, 8);
  CHECK(e.createInstrument(5, kBass, 0));
  e.scheduleParam({0, kInstr, 0, 5, kCutoff, 800.0f, 0.0f, false, 0});
  int64_t frame = 0;
  for (int i = 0; i < 2; i++) {
    e.process(frame, kBlock);
    frame += kBlock;
  }
  CHECK_NEAR(e.instrumentBaseParam(5, kCutoff), 800.0, 1e-3);
  e.scheduleParam({frame, kInstr, 0, 5, kCutoff, 2000.0f, 800.0f, true, kBlock * 4});
  for (int i = 0; i < 6; i++) {
    e.process(frame, kBlock);
    frame += kBlock;
  }
  CHECK_NEAR(e.instrumentBaseParam(5, kCutoff), 2000.0, 1e-3);
  // Unknown targets are dropped quietly and leave no ramp behind.
  e.scheduleParam({frame, kInstr, 0, 99, kCutoff, 1.0f, 0.0f, false, kBlock});
  e.scheduleParam({frame, kSample, 0, 99, 0, 1.0f, 0.0f, false, kBlock});
  e.scheduleParam({frame, kEffect, 3, 99, kGain, 1.0f, 0.0f, false, kBlock});
  e.process(frame, kBlock);
  CHECK(e.activeParamRamps() == 0);
  CHECK(e.pendingParamEvents() == 0);
}

void testRejectsMalformed() {
  Rig r;
  r.step(0, std::nanf(""));
  r.e.scheduleParam({0, 7, 0, 1, kGain, -5.0f, 0.0f, false, 0});     // unknown target kind
  r.e.scheduleParam({0, -1, 0, 1, kGain, -5.0f, 0.0f, false, 0});
  CHECK(r.e.pendingParamEvents() == 0);
  r.e.scheduleParam({0, kEffect, 0, 1, kGain, -5.0f, std::nanf(""), true, kBlock * 2});  // bad `from`: use current base
  r.blocks(4);
  CHECK(r.gain() == -5.0f);
}

void testRampTableFull() {
  Rig r;
  // Fill with distinct (effect, param) keys across many effect instances on bus 0.
  int32_t made = 0;
  for (int32_t id = 2; id < 2 + kMaxFxPerBus - 1; id++) made += r.e.addEffect(0, id, static_cast<int32_t>(FxKind::Compressor)) ? 1 : 0;
  // Compressor has 6 params: (15 instances x 6) = 90 > 64 keys.
  const int32_t compParams[] = {10, 11, 12, 13, 14, 15};
  for (int32_t id = 2; id < 2 + made; id++)
    for (int32_t p : compParams) r.e.scheduleParam({0, kEffect, 0, id, p, 1.0f, 0.5f, true, kBlock * 50});
  r.blocks(1);
  CHECK(r.e.activeParamRamps() == kMaxParamRamps);
  CHECK(r.e.droppedParamRamps() == made * 6 - kMaxParamRamps);
  r.blocks(60);
  CHECK(r.e.activeParamRamps() == 0);
}

// A sample target, audibly: a looping tone whose SampleParam::Gain is stepped by a scheduled event.
void testSampleGainTarget() {
  Engine e;
  e.init(kRate, 2, 8);
  const int32_t frames = 48000;
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))), static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = 0.5f;
  e.commitSample(7, 2, frames, static_cast<int32_t>(kRate), ch);
  e.trigger(1, 7, 0, 1.0f, 1.0f, 0, -1, true, false);
  e.scheduleParam({kBlock * 20, kSample, 0, 7, static_cast<int32_t>(SampleParam::Gain), 0.25f, 0.0f, false, 0});
  int64_t f = 0;
  auto level = [&]() {
    e.process(f, kBlock);
    f += kBlock;
    return std::fabs(e.outputChannel(0)[kBlock - 1]);
  };
  float before = 0;
  for (int i = 0; i < 18; i++) before = level();
  float after = 0;
  for (int i = 0; i < 6; i++) after = level();
  CHECK(before > 0.2f);
  CHECK_NEAR(after, before * 0.25, 0.02);
  // A ramp without `from` begins at the sample's current gain.
  e.scheduleParam({f, kSample, 0, 7, static_cast<int32_t>(SampleParam::Gain), 1.0f, 0.0f, false, kBlock * 4});
  float end = 0;
  for (int i = 0; i < 8; i++) end = level();
  CHECK_NEAR(end, before, 0.02);
  CHECK(e.activeParamRamps() == 0);
}

// The ordering guarantee: a parameter event in the same block as a note is in place before the note
// reads it (webdsp Kick's decay is read at note-on).
void testParamAppliedBeforeNoteInSameBlock() {
  Engine e;
  e.init(kRate, 2, 8);
  const int32_t kick = static_cast<int32_t>(InstrumentKind::Kick);
  const int32_t decay = static_cast<int32_t>(KickParam::Decay);
  CHECK(e.createInstrument(1, kick, 0));
  e.setInstrumentParam(1, decay, 0.1f);
  auto render = [&](float decayValue) {
    Engine k;
    k.init(kRate, 2, 8);
    k.createInstrument(1, kick, 0);
    k.setInstrumentParam(1, decay, 0.1f);
    k.scheduleParam({10, kInstr, 0, 1, decay, decayValue, 0.0f, false, 0});
    k.scheduleInstrumentNote(60, 1, 1.0f);  // the note is later in the same block
    double energy = 0;
    int64_t f = 0;
    for (int i = 0; i < 300; i++) {
      k.process(f, kBlock);
      f += kBlock;
      for (int j = 0; j < kBlock; j++) energy += std::fabs(k.outputChannel(0)[j]);
    }
    return energy;
  };
  CHECK(render(1.5f) > render(0.05f) * 2.0);  // the longer decay took effect for that note
}

}  // namespace

int main() {
  testStepLandsInItsBlock();
  testRampIsLinearAndEndsExactly();
  testRampWithoutFromStartsAtCurrentBase();
  testFromIsIndependentOfPriorState();
  testLateEventCatchesUp();
  testReplaceAndDirectWriteCancel();
  testCancelScheduledDropsQueuedEventsButNotRunningRamps();
  testRampRidesUnderLfo();
  testInstrumentAndSampleTargets();
  testSampleGainTarget();
  testRejectsMalformed();
  testRampTableFull();
  testParamAppliedBeforeNoteInSameBlock();
  std::printf("param_events_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
