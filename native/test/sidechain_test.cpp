#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

// Engine-level compressor sidechain: a compressor on one bus keyed by another bus's signal.
namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
const int32_t kOut = static_cast<int32_t>(NodeParam::BusOutput);
const int32_t kMainGain = static_cast<int32_t>(NodeParam::BusMainGain);
const int32_t kKey = static_cast<int32_t>(NodeParam::CompSidechain);
constexpr int32_t kComp = 10;  // effect id

void commitTone(Engine& e, int32_t id, int32_t frames) {
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = 0.5f;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

void setup(Engine& e) {
  e.init(kRate, 2, 64);
  commitTone(e, 1, 480000);
}

void compress(Engine& e, int32_t bus, float key) {
  e.addEffect(bus, kComp, static_cast<int32_t>(FxKind::Compressor));
  const auto set = [&](NodeParam p, float v) { e.setEffectParam(bus, kComp, static_cast<int32_t>(p), v); };
  set(NodeParam::CompThreshold, -12.0f);
  set(NodeParam::CompRatio, 20.0f);
  set(NodeParam::CompAttack, 0.0005f);
  set(NodeParam::CompRelease, 0.05f);
  set(NodeParam::CompKnee, 0.0f);
  e.setEffectParam(bus, kComp, kKey, key);
}

// Peak of the left master channel over the last 4 of `blocks` blocks.
float settledPeak(Engine& e, int32_t blocks) {
  int64_t frame = 0;
  float p = 0.0f;
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    if (b < blocks - 4) continue;
    for (int32_t f = 0; f < kBlock; f++) p = std::max(p, std::fabs(e.outputChannel(0)[f]));
  }
  return p;
}

// The listener plays a tone at -18 dBFS (0.125), under the -12 dB threshold. The key bus plays -6 dBFS (0.5) into the
// master with its main route cut, so only the listener is heard. The key is read after the chain, before the main gain.
float listenerPeak(int32_t listener, int32_t keyBus, float keyParam, bool keyPlays) {
  Engine e;
  setup(e);
  compress(e, listener, keyParam);
  e.setBusParam(keyBus, kMainGain, 0.0f);
  e.trigger(1, 1, listener, 0.25f, 1.0f, 0, -1, true, false);
  if (keyPlays) e.trigger(2, 1, keyBus, 1.0f, 1.0f, 0, -1, true, false);
  return settledPeak(e, 40);
}

void testKeyDucksTheListener() {
  const float free = listenerPeak(1, 2, 0.0f, true);                   // sidechain off: its own -18 dB is under the threshold
  CHECK(std::fabs(free - 0.125f) < 0.005f);
  CHECK(std::fabs(listenerPeak(1, 2, 2.0f, false) - 0.125f) < 0.005f);  // keyed, key silent: untouched
  CHECK(listenerPeak(1, 2, 2.0f, true) < 0.08f);                       // keyed, key loud: ducked
}

// The key renders first whichever way the bus indices fall.
void testOrderIndependentOfBusIndex() {
  CHECK(listenerPeak(1, 5, 5.0f, true) < 0.08f);
  CHECK(listenerPeak(5, 1, 1.0f, true) < 0.08f);
  CHECK(listenerPeak(40, 3, 3.0f, true) < 0.08f);
}

// A key bus that never wakes yields silence rather than reading stale memory.
void testAsleepKeyIsSilence() {
  Engine e;
  setup(e);
  compress(e, 1, 7.0f);
  e.trigger(1, 1, 1, 0.25f, 1.0f, 0, -1, true, false);
  CHECK(std::fabs(settledPeak(e, 40) - 0.125f) < 0.005f);
}

// A key that the listening bus feeds would be a loop: it is refused and the compressor stays on its own input.
void testCyclicKeyIsRefused() {
  Engine e;
  setup(e);
  e.setBusParam(1, kOut, 2);  // 1 feeds 2
  compress(e, 1, 2.0f);       // 1 keyed by 2: would need 2 first, but 2 needs 1
  CHECK(e.effectBaseParam(1, kComp, kKey) == 0.0f);
  compress(e, 3, 3.0f);  // itself
  CHECK(e.effectBaseParam(3, kComp, kKey) == 0.0f);
  compress(e, 4, 99.0f);  // unknown bus
  CHECK(e.effectBaseParam(4, kComp, kKey) == 0.0f);
  compress(e, 2, 1.0f);  // 2 keyed by 1: fine, 1 already runs first
  CHECK(e.effectBaseParam(2, kComp, kKey) == 1.0f);
  // and a route made afterwards cannot close the loop through the key
  e.setBusParam(2, kOut, 1);
  e.trigger(1, 1, 1, 0.25f, 1.0f, 0, -1, true, false);
  CHECK(settledPeak(e, 40) > 0.01f);  // still renders (no bus dropped from the order)
}

// Removing or disabling the compressor releases the ordering; the master bus can listen to a track bus too.
void testMasterKeyAndRemoval() {
  Engine e;
  setup(e);
  compress(e, 0, 2.0f);
  e.setBusParam(2, kMainGain, 0.0f);
  e.trigger(1, 1, 1, 0.25f, 1.0f, 0, -1, true, false);
  e.trigger(2, 1, 2, 1.0f, 1.0f, 0, -1, true, false);
  CHECK(settledPeak(e, 40) < 0.08f);
  e.removeEffect(0, kComp);
  CHECK(std::fabs(settledPeak(e, 40) - 0.125f) < 0.005f);
}

}  // namespace

int main() {
  testKeyDucksTheListener();
  testOrderIndependentOfBusIndex();
  testAsleepKeyIsSilence();
  testCyclicKeyIsRefused();
  testMasterKeyAndRemoval();
  std::printf("sidechain_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures, webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
