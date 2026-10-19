#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/dsp/limiter.h"
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
const int32_t kGain = static_cast<int32_t>(NodeParam::BusGain);
const int32_t kEnable = static_cast<int32_t>(NodeParam::LimiterEnable);
const int32_t kCeiling = static_cast<int32_t>(NodeParam::LimiterCeiling);

// A looping constant-level sample, so the master level is exactly `level` * the voice gain.
void commitDc(Engine& e, int32_t id, float level) {
  const int32_t frames = 48000;
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = level;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

struct Rig {
  Engine e;
  int64_t frame = 0;
  explicit Rig(float level) {
    e.init(kRate, 2, 64);
    commitDc(e, 1, level);
  }
  // Runs `blocks` blocks; returns the largest |output| seen (both channels).
  float run(int32_t blocks) {
    float p = 0.0f;
    for (int32_t b = 0; b < blocks; b++) {
      e.process(frame, kBlock);
      frame += kBlock;
      for (int32_t c = 0; c < 2; c++)
        for (int32_t f = 0; f < kBlock; f++) p = std::max(p, std::fabs(e.outputChannel(c)[f]));
    }
    return p;
  }
};

// --- limiter ---

void testBelowCeilingIsTransparent() {
  Rig r(0.5f);
  r.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  CHECK_NEAR(r.run(8), 0.5f, 1e-6);
  const float* m = r.e.readMeters();
  CHECK_NEAR(m[Engine::kMeterGainReduction], 0.0f, 1e-6);
  CHECK_NEAR(m[Engine::kMeterMasterIn], m[Engine::kMeterMasterOut], 1e-6);
}

void testExcessIsReducedToCeiling() {
  Rig r(0.5f);
  r.e.setBusParam(0, kGain, 4.0f);  // 2.0 into the limiter (+6 dBFS)
  r.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  const float ceiling = std::pow(10.0f, Limiter::kDefaultCeilingDb / 20.0f);
  // Every output sample, including the first block, stays at or under the ceiling.
  CHECK(r.run(1) <= ceiling * 1.0001f);
  CHECK(r.run(200) <= ceiling * 1.0001f);  // sustained loud input
  const float* m = r.e.readMeters();
  CHECK(m[Engine::kMeterGainReduction] < -5.0f);
  CHECK_NEAR(m[Engine::kMeterMasterIn], 2.0f, 1e-5);
  CHECK(m[Engine::kMeterMasterOut] <= ceiling * 1.0001f);
}

void testCeilingAndEnableParams() {
  Rig r(0.5f);
  r.e.setBusParam(0, kGain, 4.0f);
  r.e.setBusParam(0, kCeiling, -6.0f);
  r.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  CHECK(r.run(20) <= std::pow(10.0f, -6.0f / 20.0f) * 1.0001f);
  r.e.setBusParam(0, kEnable, 0.0f);
  CHECK_NEAR(r.run(4), 2.0f, 1e-5);  // disabled: passes untouched
  // Track buses have no limiter: the param is ignored there.
  r.e.setBusParam(1, kEnable, 0.0f);
  r.e.setBusParam(0, kEnable, 1.0f);
  CHECK(r.run(4) <= std::pow(10.0f, -6.0f / 20.0f) * 1.0001f);
}

void testReleaseRecovers() {
  Rig r(0.5f);
  r.e.setBusParam(0, kGain, 4.0f);
  r.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  r.run(10);
  r.e.setBusParam(0, kGain, 1.0f);  // back to 0.5 into the limiter
  r.run(800);                       // ~2 s >> 100 ms release
  CHECK_NEAR(r.run(2), 0.5f, 1e-4);
}

void testQuietMasterOutputUnchanged() {
  // No regression: a signal under the ceiling is bit-identical with the limiter on and off.
  Rig a(0.3f), b(0.3f);
  b.e.setBusParam(0, kEnable, 0.0f);
  a.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  b.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  for (int32_t blk = 0; blk < 4; blk++) {
    a.e.process(a.frame, kBlock); a.frame += kBlock;
    b.e.process(b.frame, kBlock); b.frame += kBlock;
    for (int32_t f = 0; f < kBlock; f++) CHECK(a.e.outputChannel(0)[f] == b.e.outputChannel(0)[f]);
  }
}

// --- metering ---

void testSilenceIsZero() {
  Rig r(0.5f);
  r.run(4);
  const float* m = r.e.readMeters();
  for (int32_t i = 0; i < Engine::kMeterCount; i++) CHECK(m[i] == 0.0f);
}

void testPeakTracksLevelAndClears() {
  float last = 0.0f;
  for (float level : {0.05f, 0.1f, 0.4f, 0.9f}) {
    Rig r(level);
    r.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
    r.run(4);
    const float p = r.e.readMeters()[Engine::kMeterMasterIn];
    CHECK_NEAR(p, level, 1e-5);
    CHECK(p > last);
    last = p;
  }
  Rig r(0.5f);
  r.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  r.run(2);
  r.e.readMeters();
  r.e.stop(1);
  r.run(40);                   // let the voice end
  r.e.readMeters();            // interval containing the tail
  r.run(2);
  CHECK(r.e.readMeters()[Engine::kMeterMasterIn] == 0.0f);  // read-and-clear: nothing new
}

void testTrackClipIdentifiesBus() {
  Rig r(0.8f);
  r.e.setBusParam(2, kGain, 2.0f);  // bus 2 -> 1.6, bus 3 stays 0.8
  r.e.trigger(1, 1, 2, 1.0f, 1.0f, 0, -1, true, false);
  r.e.trigger(2, 1, 3, 1.0f, 1.0f, 0, -1, true, false);
  r.run(4);
  const float* m = r.e.readMeters();
  CHECK(m[Engine::kMeterFirstBus + 1] >= 1.0f);  // bus 2 clips
  CHECK(m[Engine::kMeterFirstBus + 2] < 1.0f);   // bus 3 does not
  CHECK_NEAR(m[Engine::kMeterFirstBus + 2], 0.8f, 1e-5);
  CHECK(m[Engine::kMeterFirstBus + 0] == 0.0f);  // bus 1 unused
}

void testMasterClipDetectedAndOutputSafe() {
  Rig r(0.6f);
  r.e.trigger(1, 1, 1, 1.0f, 1.0f, 0, -1, true, false);
  r.e.trigger(2, 1, 2, 1.0f, 1.0f, 0, -1, true, false);  // 0.6 + 0.6 = 1.2 at master
  r.run(4);
  const float* m = r.e.readMeters();
  CHECK(m[Engine::kMeterMasterIn] >= 1.0f);
  CHECK(m[Engine::kMeterMasterOut] < 1.0f);
  CHECK(m[Engine::kMeterGainReduction] < 0.0f);
  // Neither track bus clips on its own.
  CHECK(m[Engine::kMeterFirstBus + 0] < 1.0f);
  CHECK(m[Engine::kMeterFirstBus + 1] < 1.0f);
}

void testCaptureIsPreLimiter() {
  Rig r(0.5f);
  r.e.setBusParam(0, kGain, 4.0f);
  r.e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  r.e.startCapture(1, 0);
  r.run(4);
  const Capture* c = r.e.capture(1);
  CHECK(c != nullptr && c->length() > 0);
  if (c) CHECK_NEAR(c->channelData(0)[kBlock], 2.0f, 1e-5);
}

}  // namespace

int main() {
  testBelowCeilingIsTransparent();
  testExcessIsReducedToCeiling();
  testCeilingAndEnableParams();
  testReleaseRecovers();
  testQuietMasterOutputUnchanged();
  testSilenceIsZero();
  testPeakTracksLevelAndClears();
  testTrackClipIdentifiesBus();
  testMasterClipDetectedAndOutputSafe();
  testCaptureIsPreLimiter();
  std::printf("limiter_meter_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures, webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
