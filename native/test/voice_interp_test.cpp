#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "../src/resampler.h"
#include "test_main.h"

// A mono sample is duplicated onto both output channels by Voice::render. That duplication must be
// exact, and must be the same signal a stereo sample holding two identical channels produces (the
// interpolation of a mono sample is computed once, not once per output channel).
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kFrames = 9000;

float tone(int32_t i) { return 0.6f * std::sin(i * 0.037f) + 0.2f * std::sin(i * 0.31f); }

float* makeChannel() {
  float* c = static_cast<float*>(std::calloc(kFrames, sizeof(float)));
  for (int32_t i = 0; i < kFrames; i++) c[i] = tone(i);
  return c;
}

std::vector<float> render(Engine& e, int32_t blocks, int32_t channel) {
  std::vector<float> out;
  int64_t frame = 0;
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    out.insert(out.end(), e.outputChannel(channel), e.outputChannel(channel) + kBlock);
  }
  return out;
}

void testMonoMatchesDirectInterpolation() {
  Engine e;
  e.init(kRate, 2, 4);
  float* m[1] = {makeChannel()};
  e.commitSample(1, 1, kFrames, 48000, m);
  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 0.8f);
  const float rate = 1.37f, gain = 0.7f;
  e.trigger(1, 1, 0, gain, rate, 0, -1, false, false);
  const std::vector<float> l = render(e, 20, 0);
  std::vector<float> ref(kFrames);
  for (int32_t i = 0; i < kFrames; i++) ref[i] = tone(i);
  double pos = 0.0;
  for (size_t i = 0; i < l.size() && pos < kFrames - 1; i++, pos += rate) {
    const float expected = cubicHermite(ref.data(), kFrames, pos) * gain * 0.8f;
    CHECK_NEAR(l[i], expected, 1e-5);
  }
}

void testMonoEqualsBothChannelsAndStereoDuplicate() {
  Engine mono, stereo;
  mono.init(kRate, 2, 4);
  stereo.init(kRate, 2, 4);
  float* m[1] = {makeChannel()};
  float* s[2] = {makeChannel(), makeChannel()};
  mono.commitSample(1, 1, kFrames, 48000, m);
  stereo.commitSample(1, 2, kFrames, 48000, s);
  for (Engine* e : {&mono, &stereo}) {
    e->trigger(1, 1, 0, 0.9f, 0.83f, 100, 8000, /*loop=*/true, /*reverse=*/false);
    e->trigger(2, 1, 0, 0.5f, 1.91f, 0, -1, /*loop=*/true, /*reverse=*/true);
  }
  int64_t frame = 0;
  for (int32_t b = 0; b < 40; b++, frame += kBlock) {
    mono.process(frame, kBlock);
    stereo.process(frame, kBlock);
    for (int32_t i = 0; i < kBlock; i++) {
      CHECK(mono.outputChannel(0)[i] == mono.outputChannel(1)[i]);        // mono: left == right exactly
      CHECK(mono.outputChannel(0)[i] == stereo.outputChannel(0)[i]);      // == stereo with identical channels
      CHECK(mono.outputChannel(1)[i] == stereo.outputChannel(1)[i]);
    }
  }
}

}  // namespace

int main() {
  testMonoMatchesDirectInterpolation();
  testMonoEqualsBothChannelsAndStereoDuplicate();
  std::printf("voice_interp_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
