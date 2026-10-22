#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// NodeParam::BusGain is the bus fader (and a host's mute). Like BusMainGain / BusSendAmount and
// BusPan it must not step the signal: a change glides across the block it lands in, a bus that has
// never rendered snaps to its level, and a bus at rest is exact (unity bit-identical).
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr float kAmp = 0.5f;
constexpr float kStep = 0.05f;  // radians per frame of the test tone: its own largest per-frame move is ~0.025
const int32_t kGain = static_cast<int32_t>(NodeParam::BusGain);

void commitTone(Engine& e) {
  const int32_t frames = 48000;
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = kAmp * std::sin(i * kStep);
  e.commitSample(1, 2, frames, static_cast<int32_t>(kRate), ch);
}

struct Rig {
  Engine e;
  int64_t frame = 0;
  explicit Rig(int32_t bus) {
    e.init(kRate, 2, 8);
    commitTone(e);
    e.trigger(1, 1, bus, 1.0f, 1.0f, 0, -1, /*loop=*/true, /*reverse=*/false);
  }
  std::vector<float> block() {
    e.process(frame, kBlock);
    frame += kBlock;
    return std::vector<float>(e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
};

float maxJump(float prev, const std::vector<float>& b) {
  float m = 0.0f;
  for (float x : b) {
    m = std::max(m, std::fabs(x - prev));
    prev = x;
  }
  return m;
}

// The mute click: gain 1 -> 0 on a sounding bus must not step the signal.
void testMuteDoesNotStep() {
  for (int32_t bus : {0, 1}) {
    Rig r(bus);
    for (int i = 0; i < 20; i++) r.block();
    const std::vector<float> before = r.block();
    r.e.setBusParam(bus, kGain, 0.0f);
    const std::vector<float> muted = r.block();
    // A tone sample-to-sample move is <= ~0.025; a fader glide adds <= kAmp / kBlock. A step would
    // be a jump of the signal's whole instantaneous value (hundreds of times larger).
    CHECK(maxJump(before.back(), muted) < 0.04f);
    CHECK(muted.back() == 0.0f || std::fabs(muted.back()) < 0.01f);  // glide ends at the target
    const std::vector<float> after = r.block();
    for (float x : after) CHECK(x == 0.0f);  // fully muted from the next block on, exactly
  }
}

// Un-muting is the same glide in the other direction.
void testUnmuteDoesNotStep() {
  Rig r(1);
  r.e.setBusParam(1, kGain, 0.0f);
  for (int i = 0; i < 20; i++) r.block();
  const std::vector<float> before = r.block();
  r.e.setBusParam(1, kGain, 1.0f);
  const std::vector<float> up = r.block();
  CHECK(maxJump(before.back(), up) < 0.04f);
}

// Setting a level before the bus has ever rendered applies at once: nothing to glide from.
void testFirstBlockSnaps() {
  Rig dry(1);
  Rig set(1);
  set.e.setBusParam(1, kGain, 0.5f);
  const std::vector<float> a = dry.block();
  const std::vector<float> b = set.block();
  for (int32_t i = 0; i < kBlock; i++) CHECK(b[i] == a[i] * 0.5f);
}

// At rest the level is exact, and an untouched bus is bit-identical to the pre-glide behaviour.
void testSteadyStateExact() {
  Rig dry(1);
  Rig set(1);
  set.e.setBusParam(1, kGain, 1.0f);  // explicit unity == default
  Rig half(1);
  half.e.setBusParam(1, kGain, 0.25f);
  for (int i = 0; i < 4; i++) {
    const std::vector<float> a = dry.block();
    const std::vector<float> b = set.block();
    const std::vector<float> c = half.block();
    for (int32_t k = 0; k < kBlock; k++) {
      CHECK(a[k] == b[k]);
      CHECK(c[k] == a[k] * 0.25f);
    }
  }
}

// A glide always finishes within the block it starts in; the block after is at the target exactly.
void testReachesTargetInOneBlock() {
  Rig dry(1);
  Rig r(1);
  for (int i = 0; i < 10; i++) { dry.block(); r.block(); }
  r.e.setBusParam(1, kGain, 0.3f);
  r.block();
  dry.block();
  const std::vector<float> a = dry.block();
  const std::vector<float> b = r.block();
  for (int32_t k = 0; k < kBlock; k++) CHECK(b[k] == a[k] * 0.3f);
}

// A malformed (non-finite) level is ignored, never written into the render path.
void testNonFiniteIgnored() {
  Rig r(1);
  r.e.setBusParam(1, kGain, 0.5f);
  for (int i = 0; i < 4; i++) r.block();
  r.e.setBusParam(1, kGain, std::numeric_limits<float>::quiet_NaN());
  r.e.setBusParam(1, kGain, std::numeric_limits<float>::infinity());
  const std::vector<float> b = r.block();
  for (float x : b) CHECK(std::isfinite(x));
  CHECK(std::fabs(b[10]) <= kAmp * 0.5f + 1e-6f);
}

}  // namespace

int main() {
  testMuteDoesNotStep();
  testUnmuteDoesNotStep();
  testFirstBlockSnaps();
  testSteadyStateExact();
  testReachesTargetInOneBlock();
  testNonFiniteIgnored();
  std::printf("bus_gain_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
