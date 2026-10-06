#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;

void commitTone(Engine& e, int32_t id, int32_t frames, float level = 0.5f) {
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = level;
  e.commitSample(id, 2, frames, 48000, ch);
}

void renderBlocks(Engine& e, int64_t& frame, int32_t blocks) {
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
  }
}

bool endedContains(const Engine& e, int32_t voiceId) {
  for (int32_t id : e.endedVoices()) {
    if (id == voiceId) return true;
  }
  return false;
}

// Removing a sample while a voice is still reading it must stop that voice and must not let
// any later render touch the freed PCM. Built with AddressSanitizer this test fails on a
// use-after-free if the voice were left running.
void testRemoveWhilePlayingStopsVoice() {
  Engine e;
  e.init(48000, 2, 64);
  commitTone(e, 1, 48000);  // one second, far longer than the test renders

  int64_t frame = 0;
  e.trigger(7, 1, /*busId=*/0, 1.0f, 1.0f, 0, -1, /*loop=*/true, false);
  renderBlocks(e, frame, 2);
  CHECK(e.activeVoiceCount() == 1);

  e.removeSample(1);
  CHECK(e.loadedSampleCount() == 0);
  CHECK(e.sampleMemoryBytes() == 0);

  renderBlocks(e, frame, 10);
  CHECK(e.activeVoiceCount() == 0);
}

// The stopped voice is reported through endedVoices() on the next process(), so the host can
// clear state (loop toggles, mono choke) for it, exactly as for a voice that finished normally.
void testRemovedVoiceIsReportedEnded() {
  Engine e;
  e.init(48000, 2, 64);
  commitTone(e, 1, 48000);

  int64_t frame = 0;
  e.trigger(9, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  renderBlocks(e, frame, 1);
  e.removeSample(1);
  renderBlocks(e, frame, 1);
  CHECK(endedContains(e, 9));

  renderBlocks(e, frame, 1);
  CHECK(!endedContains(e, 9));  // reported once, not repeated on later blocks
}

// A voice on a different sample is untouched by removing this one.
void testRemoveLeavesOtherVoicesAlone() {
  Engine e;
  e.init(48000, 2, 64);
  commitTone(e, 1, 48000);
  commitTone(e, 2, 48000);

  int64_t frame = 0;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  e.trigger(2, 2, 0, 1.0f, 1.0f, 0, -1, true, false);
  renderBlocks(e, frame, 1);
  e.removeSample(1);
  renderBlocks(e, frame, 5);
  CHECK(e.activeVoiceCount() == 1);
}

// A scheduled event that outlives its sample must be dropped, not played and not allowed to
// steal a voice from something still sounding.
void testScheduledEventForRemovedSampleIsDropped() {
  Engine e;
  e.init(48000, 2, 1);  // one voice: a stolen voice would be obvious
  commitTone(e, 1, 4800);
  commitTone(e, 2, 48000);

  int64_t frame = 0;
  e.trigger(1, 2, 0, 1.0f, 1.0f, 0, -1, true, false);
  renderBlocks(e, frame, 1);
  e.scheduleEvent(static_cast<int64_t>(frame + 4 * kBlock), 5, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  e.removeSample(1);
  renderBlocks(e, frame, 8);
  CHECK(e.activeVoiceCount() == 1);
  CHECK(!endedContains(e, 1));
}

// Replacing an id frees the old PCM, so a voice still reading the old buffer is stopped first.
void testReplacingSampleStopsItsVoices() {
  Engine e;
  e.init(48000, 2, 64);
  commitTone(e, 1, 48000, 0.25f);

  int64_t frame = 0;
  e.trigger(3, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  renderBlocks(e, frame, 1);
  commitTone(e, 1, 4800, 0.75f);
  renderBlocks(e, frame, 1);
  CHECK(endedContains(e, 3));
  CHECK(e.activeVoiceCount() == 0);
  CHECK(e.loadedSampleCount() == 1);
}

// Removing an id that was never loaded is a harmless no-op.
void testRemoveUnknownIdIsNoOp() {
  Engine e;
  e.init(48000, 2, 64);
  commitTone(e, 1, 4800);
  e.removeSample(42);
  CHECK(e.loadedSampleCount() == 1);
}

}  // namespace

int main() {
  testRemoveWhilePlayingStopsVoice();
  testRemovedVoiceIsReportedEnded();
  testRemoveLeavesOtherVoicesAlone();
  testScheduledEventForRemovedSampleIsDropped();
  testReplacingSampleStopsItsVoices();
  testRemoveUnknownIdIsNoOp();

  std::printf("sample_lifetime_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
