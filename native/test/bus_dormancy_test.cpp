#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;

// A constant-amplitude sample of `frames` frames, stereo, owned by the engine after commit.
void commitTone(Engine& e, int32_t id, int32_t frames) {
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = 0.5f;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

// Renders `blocks` blocks starting at `frame`, appending output to `out` when given.
void render(Engine& e, int64_t& frame, int32_t blocks, std::vector<float>* out) {
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    if (!out) continue;
    for (int32_t c = 0; c < 2; c++) out->insert(out->end(), e.outputChannel(c), e.outputChannel(c) + kBlock);
  }
}

// Delay and reverb engaged on a bus, so its skipped-or-not state actually matters.
void configureTail(Engine& e, int32_t busId) {
  e.setBusParam(busId, static_cast<int32_t>(NodeParam::DelayTime), 0.25f);
  e.setBusParam(busId, static_cast<int32_t>(NodeParam::DelayFeedback), 0.5f);
  e.setBusParam(busId, static_cast<int32_t>(NodeParam::DelayMix), 0.5f);
  e.setBusParam(busId, static_cast<int32_t>(NodeParam::ReverbMix), 0.5f);
  e.setBusParam(busId, static_cast<int32_t>(NodeParam::ReverbDecay), 0.5f);
}

// A short click on a bus must keep its delay echo audible after the click has ended. A bus
// that has been fed is processed through its silent input, so its tail is not cut off.
void testDelayTailSurvivesVoiceEnd() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 480);  // 10 ms click
  configureTail(e, 1);

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, /*busId=*/1, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, static_cast<int32_t>(0.3 * kRate / kBlock), &out);

  // Echo lands at 0.25 s; the window [0.25 s, 0.30 s) must carry energy.
  const size_t from = static_cast<size_t>(0.25 * kRate) * 2;
  float peak = 0.0f;
  for (size_t i = from; i < out.size(); i += 2) peak = std::max(peak, std::fabs(out[i]));
  CHECK(peak > 0.01f);
}

// Buses that were never fed are skipped, which is only sound if they hold no state. Parameters
// set on those idle buses must therefore have no effect on the output of the bus in use, and
// the output must match an engine that never touched them, bit for bit.
void testIdleBusesDoNotChangeActiveOutput() {
  Engine withIdle;
  withIdle.init(kRate, 2, 64);
  commitTone(withIdle, 1, 4800);
  configureTail(withIdle, 1);
  for (int32_t bus = 2; bus <= kMaxTrackBuses; bus++) {
    withIdle.setBusParam(bus, static_cast<int32_t>(NodeParam::DelayMix), 0.9f);
    withIdle.setBusParam(bus, static_cast<int32_t>(NodeParam::ReverbMix), 0.9f);
  }

  Engine plain;
  plain.init(kRate, 2, 64);
  commitTone(plain, 1, 4800);
  configureTail(plain, 1);

  int64_t f1 = 0, f2 = 0;
  std::vector<float> a, b;
  withIdle.trigger(1, 1, 1, 1.0f, 1.0f, 0, -1, false, false);
  plain.trigger(1, 1, 1, 1.0f, 1.0f, 0, -1, false, false);
  render(withIdle, f1, 400, &a);
  render(plain, f2, 400, &b);
  CHECK(a == b);
}

// Every track bus is addressable: the last bus of the pool is wired to a real chain, not a
// dead slot.
void testLastTrackBusIsLive() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 480);
  e.setBusParam(kMaxTrackBuses, static_cast<int32_t>(NodeParam::BusGain), 0.0f);

  int64_t frame = 0;
  std::vector<float> muted, live;
  e.trigger(1, 1, kMaxTrackBuses, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &muted);
  float peakMuted = 0.0f;
  for (float x : muted) peakMuted = std::max(peakMuted, std::fabs(x));
  CHECK(peakMuted == 0.0f);

  e.setBusParam(kMaxTrackBuses, static_cast<int32_t>(NodeParam::BusGain), 1.0f);
  e.trigger(2, 1, kMaxTrackBuses, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &live);
  float peakLive = 0.0f;
  for (float x : live) peakLive = std::max(peakLive, std::fabs(x));
  CHECK(peakLive > 0.1f);
}

}  // namespace

int main() {
  testDelayTailSurvivesVoiceEnd();
  testIdleBusesDoNotChangeActiveOutput();
  testLastTrackBusIsLive();

  std::printf("bus_dormancy_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
