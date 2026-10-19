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
const int32_t kOut = static_cast<int32_t>(NodeParam::BusOutput);
const int32_t kGain = static_cast<int32_t>(NodeParam::BusGain);

void commitTone(Engine& e, int32_t id, int32_t frames) {
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = 0.5f;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

// Peak of the left channel over `blocks` blocks (skipping the first, for any smoothing).
float peak(Engine& e, int64_t& frame, int32_t blocks) {
  float p = 0.0f;
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    if (b == 0) continue;
    for (int32_t f = 0; f < kBlock; f++) p = std::max(p, std::fabs(e.outputChannel(0)[f]));
  }
  return p;
}

float peakWith(const std::vector<std::pair<int32_t, int32_t>>& routes, int32_t voiceBus,
               const std::vector<std::pair<int32_t, float>>& gains = {}) {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 48000);
  for (auto& [bus, dest] : routes) e.setBusParam(bus, kOut, static_cast<float>(dest));
  for (auto& [bus, g] : gains) e.setBusParam(bus, kGain, g);
  e.trigger(1, 1, voiceBus, 1.0f, 1.0f, 0, -1, true, false);
  int64_t frame = 0;
  return peak(e, frame, 6);
}

// Default routing is unchanged: bus 1 -> master.
void testDefaultGoesToMaster() { CHECK(peakWith({}, 1) > 0.1f); }

// 1 -> 2 reaches master through 2 whichever bus index is higher, and the destination's gain
// applies to the routed signal (so its chain really sits in the path).
void testRoutedThroughDestination() {
  const float direct = peakWith({}, 1);
  CHECK(std::fabs(peakWith({{1, 2}}, 1) - direct) < 1e-4f);
  CHECK(std::fabs(peakWith({{2, 1}}, 2) - direct) < 1e-4f);
  CHECK(std::fabs(peakWith({{1, 2}}, 1, {{2, 0.5f}}) - direct * 0.5f) < 1e-3f);
  CHECK(std::fabs(peakWith({{1, 2}}, 1, {{1, 0.5f}}) - direct * 0.5f) < 1e-3f);
}

// Multi-hop chains sum correctly regardless of index order: 3 -> 1 -> 2 -> master.
void testChain() {
  const float direct = peakWith({}, 3);
  CHECK(std::fabs(peakWith({{3, 1}, {1, 2}}, 3) - direct) < 1e-4f);
}

// Two sources into one destination mix at the destination.
void testMixing() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 48000);
  e.setBusParam(1, kOut, 3);
  e.setBusParam(2, kOut, 3);
  e.trigger(1, 1, 1, 0.25f, 1.0f, 0, -1, true, false);
  e.trigger(2, 1, 2, 0.25f, 1.0f, 0, -1, true, false);
  int64_t frame = 0;
  const float both = peak(e, frame, 4);
  CHECK(std::fabs(both - 0.25f) < 1e-3f);  // 0.5 * 0.25 * 2
}

// Self-routes, cycles and unknown targets are rejected; the old route stays.
void testRejections() {
  const float direct = peakWith({}, 1);
  CHECK(std::fabs(peakWith({{1, 1}}, 1) - direct) < 1e-4f);
  // 1 -> 2 accepted, 2 -> 1 rejected (would loop): the signal must still arrive once.
  CHECK(std::fabs(peakWith({{1, 2}, {2, 1}}, 1) - direct) < 1e-4f);
  CHECK(std::fabs(peakWith({{1, 99}}, 1) - direct) < 1e-4f);
  CHECK(std::fabs(peakWith({{1, 2}, {2, 3}, {3, 1}}, 1) - direct) < 1e-4f);
}

// Re-pointing back to master works.
void testRouteBackToMaster() {
  const float direct = peakWith({}, 1);
  CHECK(std::fabs(peakWith({{1, 2}, {1, 0}}, 1, {{2, 0.0f}}) - direct) < 1e-4f);
}

}  // namespace

int main() {
  testDefaultGoesToMaster();
  testRoutedThroughDestination();
  testChain();
  testMixing();
  testRejections();
  testRouteBackToMaster();
  std::printf("bus_routing_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
