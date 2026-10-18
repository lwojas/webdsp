#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "test_main.h"

// Covers sample cue regions + chop playback (sample_store.h SampleRegion, Engine::trigger /
// scheduleEvent `region`). Chromatic playback (region = -1) is covered by the other sample tests.
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kSliceLen = 480;
constexpr int32_t kSlices = 8;

// Slice k holds the constant level (k + 1) / 10, so the output level identifies the region.
void commitSlices(Engine& e, int32_t id) {
  const int32_t frames = kSliceLen * kSlices;
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = (i / kSliceLen + 1) / 10.0f;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

void setSliceRegions(Engine& e, int32_t id) {
  e.clearSampleRegions(id);
  for (int32_t k = 0; k < kSlices; k++) e.addSampleRegion(id, k * kSliceLen, (k + 1) * kSliceLen);
}

std::vector<float> render(Engine& e, int64_t& frame, int32_t blocks) {
  std::vector<float> out;
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    out.insert(out.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
  return out;
}

void testRegionPlaysOnlyItsSpan() {
  for (int32_t k = 0; k < kSlices; k++) {
    Engine e;
    e.init(kRate, 2, 16);
    commitSlices(e, 1);
    setSliceRegions(e, 1);
    int64_t frame = 0;
    e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, k);
    auto out = render(e, frame, 6);  // 768 frames > one slice
    const float want = (k + 1) / 10.0f;
    CHECK_NEAR(out[10], want, 1e-4);
    CHECK_NEAR(out[kSliceLen - 2], want, 1e-4);
    CHECK_NEAR(out[kSliceLen + 20], 0.0, 1e-6);  // stopped at the region end, no bleed into the next
  }
}

void testWholeSampleUnchangedWithRegionsPresent() {
  Engine e;
  e.init(kRate, 2, 16);
  commitSlices(e, 1);
  setSliceRegions(e, 1);
  int64_t frame = 0;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);  // no region = chromatic
  auto out = render(e, frame, 4);
  CHECK_NEAR(out[10], 0.1, 1e-4);
  CHECK_NEAR(out[kSliceLen + 20], 0.2, 1e-4);  // plays on through the later slices
}

void testRateAppliesInRegion() {
  Engine e;
  e.init(kRate, 2, 16);
  commitSlices(e, 1);
  setSliceRegions(e, 1);
  int64_t frame = 0;
  e.trigger(1, 1, 0, 1.0f, 2.0f, 0, -1, false, false, -1, 0.0f, 2);  // region 3 at double speed
  auto out = render(e, frame, 4);
  CHECK_NEAR(out[100], 0.3, 1e-4);
  CHECK_NEAR(out[kSliceLen / 2 + 20], 0.0, 1e-6);  // ended after half the frames
}

void testMissingRegionPlaysNothing() {
  Engine e;
  e.init(kRate, 2, 16);
  commitSlices(e, 1);
  int64_t frame = 0;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, 0);  // no regions defined
  CHECK(e.activeVoiceCount() == 0);
  setSliceRegions(e, 1);
  e.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, kSlices);  // out of range
  CHECK(e.activeVoiceCount() == 0);
  render(e, frame, 1);
}

void testRegionsClampedAndCapped() {
  Engine e;
  e.init(kRate, 2, 16);
  commitSlices(e, 1);
  e.addSampleRegion(1, -50, 1000000);
  e.addSampleRegion(99, 0, 10);  // unknown id: no-op
  const Sample* s = e.sample(1);
  int32_t a = 0, b = 0;
  CHECK(s->regionBounds(0, a, b));
  CHECK(a == 0 && b == kSliceLen * kSlices);
  for (int32_t i = 0; i < kMaxSampleRegions + 10; i++) e.addSampleRegion(1, 0, 10);
  CHECK(static_cast<int32_t>(s->regions.size()) == kMaxSampleRegions);
}

void testScheduledRegion() {
  Engine e;
  e.init(kRate, 2, 16);
  commitSlices(e, 1);
  setSliceRegions(e, 1);
  e.scheduleEvent(kBlock + 10, 1, 1, 0, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, 4);
  e.scheduleEvent(kBlock + 10, 2, 1, 0, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, 50);  // dropped
  int64_t frame = 0;
  auto out = render(e, frame, 3);
  CHECK_NEAR(out[kBlock + 5], 0.0, 1e-6);
  CHECK_NEAR(out[kBlock + 40], 0.5, 1e-4);
  CHECK(e.activeVoiceCount() == 1);
}

void testRecommitClearsRegions() {
  Engine e;
  e.init(kRate, 2, 16);
  commitSlices(e, 1);
  setSliceRegions(e, 1);
  commitSlices(e, 1);  // replaced sample starts without regions, like gain
  CHECK(e.sample(1)->regions.empty());
}

}  // namespace

int main() {
  testRegionPlaysOnlyItsSpan();
  testWholeSampleUnchangedWithRegionsPresent();
  testRateAppliesInRegion();
  testMissingRegionPlaysNothing();
  testRegionsClampedAndCapped();
  testScheduledRegion();
  testRecommitClearsRegions();
  std::printf("sample_regions_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
