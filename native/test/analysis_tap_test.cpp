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

void testChainTapsInputAndOutput() {
  FxChain chain;
  chain.configure(48000.0);
  CHECK(chain.add(1, kUtility, -1));
  CHECK(chain.add(2, kEq, -1));
  chain.setParam(1, static_cast<int32_t>(NodeParam::UtilityGain), 6.0f);  // +6 dB ahead of the EQ
  AnalysisTap in, out;
  CHECK(chain.addTap(2, false, &in));
  CHECK(chain.addTap(2, true, &out));
  std::vector<float> l(kBlock, 0.25f), r(kBlock, 0.25f);
  float* ch[2] = {l.data(), r.data()};
  chain.process(ch, 2, kBlock);
  CHECK(in.written() == static_cast<uint32_t>(kBlock));
  CHECK(out.written() == static_cast<uint32_t>(kBlock));
  const float inLevel = in.snapshot()[kAnalysisWindow - 1];
  CHECK(inLevel > 0.4f);  // the +6 dB Utility ahead of the EQ is already in the input tap
  // Disabled EQ: its output is its input.
  chain.setEnabled(2, false);
  chain.process(ch, 2, kBlock);
  CHECK(in.written() == static_cast<uint32_t>(2 * kBlock) && out.written() == static_cast<uint32_t>(2 * kBlock));
  // A band that boosts only shows in the OUTPUT tap.
  chain.setEnabled(2, true);
  chain.setParam(2, static_cast<int32_t>(NodeParam::EqBand2On), 1.0f);
  chain.setParam(2, static_cast<int32_t>(NodeParam::EqBand2Type), 1.0f);  // low shelf
  chain.setParam(2, static_cast<int32_t>(NodeParam::EqBand2Freq), 100.0f);
  chain.setParam(2, static_cast<int32_t>(NodeParam::EqBand2Gain), 12.0f);
  for (int32_t b = 0; b < 40; b++) {
    std::vector<float> a(kBlock, 0.1f), c(kBlock, 0.1f);
    float* chn[2] = {a.data(), c.data()};
    chain.process(chn, 2, kBlock);  // DC is boosted by a low shelf
  }
  CHECK(out.snapshot()[kAnalysisWindow - 1] > in.snapshot()[kAnalysisWindow - 1] * 1.5f);
  // Removing one tap leaves the other running, and frees room for a new one.
  chain.removeTap(&in);
  const uint32_t inBefore = in.written(), outBefore = out.written();
  chain.process(ch, 2, kBlock);
  CHECK(in.written() == inBefore && out.written() == outBefore + kBlock);
  AnalysisTap t3, t4, t5;
  CHECK(chain.addTap(1, false, &t3) && chain.addTap(1, true, &t4));
  CHECK(chain.addTap(2, false, &in));
  CHECK(!chain.addTap(2, false, &t5));  // kMaxAnalysisTaps (4) in use
}

void testEngineSlots() {
  Engine e;
  e.init(48000.0, 2, 8);
  CHECK(!e.setAnalysisTap(0, 1, 5, false));  // no such effect
  CHECK(e.addEffect(1, 5, kEq));
  CHECK(e.setAnalysisTap(0, 1, 5, false));
  CHECK(e.setAnalysisTap(1, 1, 5, true));    // a second slot on the same effect
  CHECK(!e.setAnalysisTap(kMaxAnalysisTaps, 1, 5, true));  // bad slot
  CHECK(e.setAnalysisTap(0, 1, 0, false));   // free slot 0
  CHECK(e.setAnalysisTap(0, 0, 0, false) && e.analysisWritten(0) == 0);
  CHECK(e.addEffect(0, 7, kEq));             // master bus
  CHECK(e.setAnalysisTap(2, 0, 7, true));    // including the master bus
  CHECK(e.setAnalysisTap(2, 0, 0, false));
  CHECK(e.setAnalysisTap(1, 0, 0, false));
}
}  // namespace

int main() {
  testTapRingOrdering();
  testChainTapsInputAndOutput();
  testEngineSlots();
  std::printf("analysis_tap_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
