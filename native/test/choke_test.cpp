#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "test_main.h"

// Covers choke groups (VoiceManager::trigger / Voice::choke, Engine::trigger / scheduleEvent
// `chokeGroup`): a trigger into group g stops the other sources' active voices in g at its own
// frame, independent of voice allocation. Sample N holds the constant level kLevel[N], so the
// master output level identifies exactly which voices are sounding. Bus ids 1..3 are unmixed here
// (no createBus), so they sum straight into the master output, which is all these tests read.
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kLen = 48000;
constexpr float kLevel[] = {0.0f, 0.1f, 0.2f, 0.4f};  // samples 1, 2, 3

std::vector<int32_t> g_ended;  // every voice id reported ended by render() so far

void commit(Engine& e, int32_t id) {
  float* ch[2] = {static_cast<float*>(std::calloc(kLen, sizeof(float))),
                  static_cast<float*>(std::calloc(kLen, sizeof(float)))};
  for (int32_t i = 0; i < kLen; i++) ch[0][i] = ch[1][i] = kLevel[id];
  e.commitSample(id, 2, kLen, static_cast<int32_t>(kRate), ch);
}

void setup(Engine& e, int32_t maxVoices = 16) {
  g_ended.clear();
  e.init(kRate, 2, maxVoices);
  for (int32_t id = 1; id <= 3; id++) commit(e, id);
}

std::vector<float> render(Engine& e, int64_t& frame, int32_t blocks) {
  std::vector<float> out;
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    g_ended.insert(g_ended.end(), e.endedVoices().begin(), e.endedVoices().end());
    frame += kBlock;
    out.insert(out.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
  return out;
}

void trig(Engine& e, int32_t voice, int32_t sample, int32_t bus, int32_t group) {
  e.trigger(voice, sample, bus, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, group);
}

bool ended(int32_t voice) {
  for (int32_t v : g_ended) if (v == voice) return true;
  return false;
}

void testNoGroupUnchanged() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 0);
  trig(e, 2, 2, 2, 0);
  auto out = render(e, frame, 4);
  CHECK_NEAR(out[kBlock * 3], 0.1 + 0.2, 1e-4);  // both keep sounding
}

void testChokesOtherSource() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);  // A
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1, 1e-4);
  trig(e, 2, 2, 2, 1);  // B chokes A
  out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.2, 1e-4);  // only B
  CHECK(ended(1));   // the choked voice is reported ended like any other
  CHECK(!ended(2));
  CHECK(e.activeVoiceCount() == 1);
}

void testReverseDirection() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 2, 2, 1);  // B
  render(e, frame, 2);
  trig(e, 2, 1, 1, 1);  // A chokes B
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1, 1e-4);
}

void testMultipleVoicesChoked() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  trig(e, 2, 2, 2, 1);
  trig(e, 3, 3, 3, 1);  // chokes 1 and 2 (the first two choke each other too: 2 chokes 1)
  auto out = render(e, frame, 3);
  CHECK_NEAR(out[kBlock * 2], 0.4, 1e-4);
  CHECK(e.activeVoiceCount() == 1);
}

void testOtherGroupAndNoGroupUnaffected() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 2);  // group 2
  trig(e, 2, 2, 2, 0);  // no group
  render(e, frame, 1);
  trig(e, 3, 3, 3, 1);  // group 1: touches neither
  auto out = render(e, frame, 3);
  CHECK_NEAR(out[kBlock * 2], 0.1 + 0.2 + 0.4, 1e-4);
  trig(e, 4, 1, 3, 0);  // an ungrouped trigger chokes nothing either
  out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1 + 0.2 + 0.4 + 0.1, 1e-4);
}

void testSameSourceNotChoked() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  render(e, frame, 1);
  trig(e, 2, 2, 1, 1);  // same bus: the track's own voices stack as before
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1 + 0.2, 1e-4);
}

void testChokesWithIdleVoicesAvailable() {
  Engine e;
  setup(e, 64);  // plenty of idle voices: this is not stealing
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  render(e, frame, 1);
  CHECK(e.activeVoiceCount() == 1);
  trig(e, 2, 2, 2, 1);
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.2, 1e-4);
}

void testChokeIsSampleAccurateAndDeclicked() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  render(e, frame, 2);
  const int64_t at = frame + 40;  // mid-block
  e.scheduleEvent(at, 2, 2, 2, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, 1);
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[39], 0.1, 1e-4);               // A untouched before the choke frame
  CHECK(out[40] < 0.1 + 0.2 - 1e-4 && out[40] > 0.2);  // A already fading while B starts
  CHECK(out[60] < out[40]);                     // ...and still fading (declicked, not a hard cut)
  CHECK_NEAR(out[40 + 200], 0.2, 1e-4);         // A fully gone (fade ~96 frames), B remains
  CHECK(e.activeVoiceCount() == 1);
}

void testMaxVoicesStillStealsIndependently() {
  Engine e;
  setup(e, 2);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 0);
  trig(e, 2, 2, 2, 0);
  trig(e, 3, 3, 3, 0);  // pool of 2, no groups: steals the quietest (voice 1) as before
  auto out = render(e, frame, 2);
  CHECK(e.activeVoiceCount() == 2);
  CHECK_NEAR(out[kBlock + 5], 0.2 + 0.4, 1e-4);
}

void testChokedPendingVoiceNeverStarts() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  // Same frame: the later-triggered voice wins deterministically.
  e.scheduleEvent(frame + 30, 1, 1, 1, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, 1);
  e.scheduleEvent(frame + 30, 2, 2, 2, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, 1);
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.2, 1e-4);
  CHECK(e.activeVoiceCount() == 1);
}

}  // namespace

int main() {
  testNoGroupUnchanged();
  testChokesOtherSource();
  testReverseDirection();
  testMultipleVoicesChoked();
  testOtherGroupAndNoGroupUnaffected();
  testSameSourceNotChoked();
  testChokesWithIdleVoicesAvailable();
  testChokeIsSampleAccurateAndDeclicked();
  testMaxVoicesStillStealsIndependently();
  testChokedPendingVoiceNeverStarts();
  std::printf("choke_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
