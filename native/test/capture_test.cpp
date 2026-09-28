#include <vector>
#include "../src/capture.h"
#include "test_main.h"

using namespace webdsp;

constexpr double kSampleRate = 48000.0;

// One channel of a constant, easily-distinguishable signal — value == its absolute frame
// index within the source block, so a captured sample's value alone tells us which frame it
// actually came from.
static std::vector<float> makeBlock(int64_t blockStartFrame, int32_t numFrames) {
  std::vector<float> block(numFrames);
  for (int32_t i = 0; i < numFrames; i++) block[i] = static_cast<float>(blockStartFrame + i);
  return block;
}

static void testArmedCaptureStartsExactlyOnFrameMidBlock() {
  Capture cap;
  CHECK(cap.arm(1, kSampleRate, /*startFrame*/ 64, /*stopFrame*/ 192));  // 128 frames

  auto block0 = makeBlock(0, 128);  // frames [0,128) -- window starts mid-block at 64
  float* ch0[] = {block0.data()};
  cap.appendBlock(ch0, 1, 128, 0);
  CHECK(cap.isActive());
  CHECK(!cap.isFinished());
  CHECK(cap.length() == 64);  // only frames [64,128) fell inside the window
  CHECK(cap.channelData(0)[0] == 64.0f);
  CHECK(cap.channelData(0)[63] == 127.0f);

  auto block1 = makeBlock(128, 128);  // frames [128,256) -- window ends mid-block at 192
  float* ch1[] = {block1.data()};
  cap.appendBlock(ch1, 1, 128, 128);
  CHECK(cap.length() == 128);
  CHECK(cap.channelData(0)[64] == 128.0f);
  CHECK(cap.channelData(0)[127] == 191.0f);
  CHECK(cap.isFinished());
  CHECK(!cap.isActive());
}

static void testBlocksEntirelyBeforeTheWindowAreIgnored() {
  Capture cap;
  CHECK(cap.arm(1, kSampleRate, /*startFrame*/ 1000, /*stopFrame*/ 1128));

  auto block = makeBlock(0, 128);
  float* ch[] = {block.data()};
  cap.appendBlock(ch, 1, 128, 0);
  CHECK(cap.length() == 0);
  CHECK(cap.isActive());
  CHECK(!cap.isFinished());
}

static void testArmRefusesAnEmptyOrOverCapacityWindow() {
  Capture empty;
  CHECK(!empty.arm(1, kSampleRate, 100, 100));  // stopFrame == startFrame
  CHECK(!empty.isActive());

  Capture reversed;
  CHECK(!reversed.arm(1, kSampleRate, 200, 100));  // stopFrame < startFrame
  CHECK(!reversed.isActive());

  Capture tooLong;
  const int64_t overCapacityFrames = static_cast<int64_t>(kSampleRate * kMaxCaptureSeconds) + 1;
  CHECK(!tooLong.arm(1, kSampleRate, 0, overCapacityFrames));
  CHECK(!tooLong.isActive());
  CHECK(tooLong.length() == 0);  // refused outright, never silently truncated
}

static void testArmedCaptureExactlyOneBlockWide() {
  Capture cap;
  CHECK(cap.arm(2, kSampleRate, 0, 128));  // exactly one render quantum, two channels

  auto blockL = makeBlock(0, 128);
  auto blockR = makeBlock(10000, 128);
  float* ch[] = {blockL.data(), blockR.data()};
  cap.appendBlock(ch, 2, 128, 0);

  CHECK(cap.isFinished());
  CHECK(cap.length() == 128);
  CHECK(cap.channelData(0)[0] == 0.0f);
  CHECK(cap.channelData(0)[127] == 127.0f);
  CHECK(cap.channelData(1)[0] == 10000.0f);
  CHECK(cap.channelData(1)[127] == 10127.0f);
}

static void testManualModeIsUnboundedUntilExplicitStop() {
  Capture cap;
  cap.start(1, kSampleRate);

  for (int64_t start = 0; start < 512; start += 128) {
    auto block = makeBlock(start, 128);
    float* ch[] = {block.data()};
    cap.appendBlock(ch, 1, 128, start);
    CHECK(!cap.isFinished());  // manual mode never auto-finishes
  }
  CHECK(cap.length() == 512);
  cap.stop();
  CHECK(!cap.isActive());
}

int main() {
  testArmedCaptureStartsExactlyOnFrameMidBlock();
  testBlocksEntirelyBeforeTheWindowAreIgnored();
  testArmRefusesAnEmptyOrOverCapacityWindow();
  testArmedCaptureExactlyOneBlockWide();
  testManualModeIsUnboundedUntilExplicitStop();

  std::printf("capture_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
