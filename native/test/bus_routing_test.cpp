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

// --- sends: a second, post-fader edge, independent of the main route ---

const int32_t kSendTo = static_cast<int32_t>(NodeParam::BusSendTarget);
const int32_t kSendAmt = static_cast<int32_t>(NodeParam::BusSendAmount);
const int32_t kMainGain = static_cast<int32_t>(NodeParam::BusMainGain);

struct Param { int32_t bus; int32_t param; float value; };

// Master peak with a tone (0.5) on `voiceBus`, `params` applied first. Master gets: the source's
// main route (bus 1 -> master unless re-routed) plus whatever the sends deliver.
float sendPeak(const std::vector<Param>& params, int32_t voiceBus = 1) {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 48000);
  for (auto& p : params) e.setBusParam(p.bus, p.param, p.value);
  e.trigger(1, 1, voiceBus, 1.0f, 1.0f, 0, -1, true, false);
  int64_t frame = 0;
  return peak(e, frame, 6);
}

// Send amount scales only the send; main + send add at master. Bus 2 is the FX bus.
void testSendAmount() {
  const float main = sendPeak({{1, kMainGain, 1.0f}});
  CHECK(std::fabs(main - 0.5f) < 1e-3f);
  // amount 0: the FX bus receives nothing, master hears only the main route.
  CHECK(std::fabs(sendPeak({{1, kSendTo, 2}, {1, kSendAmt, 0.0f}}) - main) < 1e-3f);
  // Send routed, main muted: master hears only the send, scaled by the amount.
  const std::vector<Param> sendOnly = {{1, kSendTo, 2}, {1, kMainGain, 0.0f}};
  for (const float a : {0.5f, 1.0f, 0.25f}) {
    auto p = sendOnly;
    p.push_back({1, kSendAmt, a});
    CHECK(std::fabs(sendPeak(p) - 0.5f * a) < 1e-3f);
  }
  // Main + full send: both paths reach master and add.
  CHECK(std::fabs(sendPeak({{1, kSendTo, 2}, {1, kSendAmt, 0.5f}}) - 0.75f) < 1e-3f);
}

// The tap is post-fader: BusGain scales the send too.
void testSendIsPostFader() {
  const float p = sendPeak({{1, kSendTo, 2}, {1, kMainGain, 0.0f}, {1, kSendAmt, 1.0f}, {1, kGain, 0.5f}});
  CHECK(std::fabs(p - 0.25f) < 1e-3f);
  // Muted fader (gain 0) silences the send as well as the main route.
  CHECK(sendPeak({{1, kSendTo, 2}, {1, kGain, 0.0f}}) < 1e-6f);
}

// FX bus solo shape: the source's main route is gated off yet its send still reaches the FX bus,
// which processes it (its own gain applies) on the way to master.
void testMainGateKeepsSend() {
  const float p = sendPeak({{1, kSendTo, 2}, {1, kMainGain, 0.0f}, {2, kGain, 0.5f}});
  CHECK(std::fabs(p - 0.25f) < 1e-3f);
}

// Two sources into one FX bus at independent amounts; muting one removes only its contribution.
void testSharedFxBus() {
  auto run = [](float aGain, float bGain) {
    Engine e;
    e.init(kRate, 2, 64);
    commitTone(e, 1, 48000);
    for (int32_t src : {1, 2}) {
      e.setBusParam(src, kSendTo, 3);
      e.setBusParam(src, kMainGain, 0.0f);
    }
    e.setBusParam(1, kSendAmt, 0.25f);
    e.setBusParam(2, kSendAmt, 0.75f);
    e.setBusParam(1, kGain, aGain);
    e.setBusParam(2, kGain, bGain);
    e.trigger(1, 1, 1, 1.0f, 1.0f, 0, -1, true, false);
    e.trigger(2, 1, 2, 1.0f, 1.0f, 0, -1, true, false);
    int64_t frame = 0;
    return peak(e, frame, 6);
  };
  CHECK(std::fabs(run(1.0f, 1.0f) - 0.5f) < 1e-3f);          // 0.5*0.25 + 0.5*0.75
  CHECK(std::fabs(run(0.0f, 1.0f) - 0.375f) < 1e-3f);        // A muted: B unaffected
  CHECK(std::fabs(run(1.0f, 0.0f) - 0.125f) < 1e-3f);        // B muted: A unaffected
}

// Sends share the cycle guard with outputs, and the send destination still orders after its
// sources whatever the bus indices.
void testSendRejectionsAndOrder() {
  const std::vector<Param> sendOnly = {{1, kMainGain, 0.0f}};
  auto with = [&](std::vector<Param> extra) {
    auto p = sendOnly;
    p.insert(p.end(), extra.begin(), extra.end());
    return sendPeak(p);
  };
  CHECK(with({{1, kSendTo, 1}}) < 1e-6f);                                  // self-send ignored
  CHECK(with({{1, kSendTo, 99}}) < 1e-6f);                                 // unknown ignored
  CHECK(std::fabs(with({{3, kSendTo, 1}, {1, kSendTo, 3}}) - 0.0f) < 1e-6f);  // 1->3 closes 3->1: rejected
  CHECK(with({{2, kOut, 1}, {1, kSendTo, 2}}) < 1e-6f);                    // send cycle via an output route
  CHECK(std::fabs(with({{1, kSendTo, 3}, {3, kOut, 2}}) - 0.5f) < 1e-3f);  // 1 -send-> 3 -> 2 -> master
  CHECK(std::fabs(with({{1, kSendTo, 2}}) - 0.5f) < 1e-3f);                // dest index above and below
  CHECK(std::fabs(sendPeak({{2, kSendTo, 1}, {2, kMainGain, 0.0f}}, 2) - 0.5f) < 1e-3f);
  // Re-pointing and clearing a send leaves one active edge: no duplicate delivery.
  CHECK(std::fabs(with({{1, kSendTo, 2}, {1, kSendTo, 3}, {1, kSendTo, 0}})) < 1e-6f);
}

// A bus that only receives a send (never fed a voice) wakes and runs, and its FX tail is kept.
void testSendWakesDormantDestination() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 48000);
  e.setBusParam(1, kSendTo, 2);
  e.setBusParam(1, kMainGain, 0.0f);
  e.setBusParam(2, kGain, 0.5f);
  e.trigger(1, 1, 1, 1.0f, 1.0f, 0, -1, true, false);
  int64_t frame = 0;
  CHECK(std::fabs(peak(e, frame, 6) - 0.25f) < 1e-3f);
}

// A level change after the bus has rendered glides across the block instead of stepping.
void testMainGainGlides() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 48000);
  e.trigger(1, 1, 1, 1.0f, 1.0f, 0, -1, true, false);
  int64_t frame = 0;
  for (int b = 0; b < 4; b++) { e.process(frame, kBlock); frame += kBlock; }
  e.setBusParam(1, kMainGain, 0.0f);
  e.process(frame, kBlock);
  CHECK(e.outputChannel(0)[0] > 0.45f);
  CHECK(e.outputChannel(0)[kBlock - 1] < 0.02f);
  CHECK(std::fabs(e.outputChannel(0)[1] - e.outputChannel(0)[0]) < 0.01f);
}

}  // namespace

int main() {
  testDefaultGoesToMaster();
  testRoutedThroughDestination();
  testChain();
  testMixing();
  testRejections();
  testRouteBackToMaster();
  testSendAmount();
  testSendIsPostFader();
  testMainGateKeepsSend();
  testSharedFxBus();
  testSendRejectionsAndOrder();
  testSendWakesDormantDestination();
  testMainGainGlides();
  std::printf("bus_routing_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
