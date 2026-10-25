#include <cmath>
#include <vector>
#include "../src/analysis_tap.h"
#include "../src/engine.h"
#include "../src/fx_chain.h"
#include "test_main.h"

using namespace webdsp;

namespace {
constexpr int32_t kBlock = 128;
constexpr int32_t kEq = static_cast<int32_t>(FxKind::Eq);
constexpr int32_t kUtility = static_cast<int32_t>(FxKind::Utility);

// Ramp 0,1,2,... on L and 2x that on R, so the mono sum is 1.5 * index.
void fill(std::vector<float>& l, std::vector<float>& r, int32_t from) {
  for (int32_t i = 0; i < kBlock; i++) { l[i] = static_cast<float>(from + i); r[i] = 2.0f * (from + i); }
}

void testTapRingOrdering() {
  AnalysisTap tap;
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  CHECK(tap.written() == 0);
  const int32_t blocks = 40;  // 5120 frames: wraps the 4096 ring
  for (int32_t b = 0; b < blocks; b++) { fill(l, r, b * kBlock); tap.write(ch, 2, kBlock); }
  CHECK(tap.written() == static_cast<uint32_t>(blocks * kBlock));
  const float* s = tap.snapshot();
  const int32_t total = blocks * kBlock;
  CHECK_NEAR(s[kAnalysisWindow - 1], 1.5f * (total - 1), 1e-3);  // newest last
  CHECK_NEAR(s[0], 1.5f * (total - kAnalysisWindow), 1e-3);       // oldest first
}

void testChainTapsInputOfOneInstance() {
  FxChain chain;
  chain.configure(48000.0);
  CHECK(chain.add(1, kUtility, -1));
  CHECK(chain.add(2, kEq, -1));
  AnalysisTap tap;
  chain.setTap(2, &tap);
  std::vector<float> l(kBlock, 0.25f), r(kBlock, 0.25f);
  float* ch[2] = {l.data(), r.data()};
  chain.process(ch, 2, kBlock);
  CHECK(tap.written() == static_cast<uint32_t>(kBlock));
  CHECK_NEAR(tap.snapshot()[kAnalysisWindow - 1], 0.25f, 1e-6);
  // A bypassed instance still reports its input.
  chain.setEnabled(2, false);
  chain.process(ch, 2, kBlock);
  CHECK(tap.written() == static_cast<uint32_t>(2 * kBlock));
  // Detached: no more writes.
  chain.setTap(0, nullptr);
  chain.process(ch, 2, kBlock);
  CHECK(tap.written() == static_cast<uint32_t>(2 * kBlock));
}

void testEngineAttachDetach() {
  Engine e;
  e.init(48000.0, 2, 8);
  CHECK(!e.setAnalysisTap(1, 5));  // no such effect
  CHECK(e.addEffect(1, 5, kEq));
  CHECK(e.setAnalysisTap(1, 5));
  CHECK(e.setAnalysisTap(1, 0));   // off
  CHECK(e.analysisWritten() == 0);
  CHECK(e.addEffect(0, 7, kEq));   // master bus
  CHECK(e.setAnalysisTap(0, 7));   // retarget works, including the master bus
  CHECK(e.setAnalysisTap(0, 0));
}
}  // namespace

int main() {
  testTapRingOrdering();
  testChainTapsInputOfOneInstance();
  testEngineAttachDetach();
  std::printf("analysis_tap_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
