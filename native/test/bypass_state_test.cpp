#include <cmath>
#include <vector>
#include "../src/dsp/chorus_flanger.h"
#include "../src/dsp/delay.h"
#include "../src/dsp/reverb.h"
#include "../src/params.h"
#include "test_main.h"

// An effect whose mix is 0 skips its processing. Its delay-line / tank memory must not survive
// that bypass: otherwise raising the mix again, later, replays audio from before the bypass (a
// ghost echo or a loud old reverb tail) even though the input is silent. This is the same
// "re-enabling starts clean" rule FxChain::setEnabled already gives a disabled instance. While the
// mix stays above 0 the tail must still ring on silent input, exactly as before.
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;

template <typename Gen>
float runPeak(DSPNode& n, int32_t blocks, Gen&& gen, int64_t& t) {
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  float peak = 0.0f;
  for (int32_t b = 0; b < blocks; b++) {
    for (int32_t i = 0; i < kBlock; i++) l[i] = r[i] = gen(t++);
    n.process(ch, 2, kBlock);
    for (float x : l) peak = std::max(peak, std::fabs(x));
  }
  return peak;
}

float burst(int64_t t) { return t < 4800 ? 0.8f * std::sin(t * 0.1f) : 0.0f; }
float silence(int64_t) { return 0.0f; }

// mix on -> burst + silence, mix 0 for a long silent stretch, mix on again: input silent throughout
// the last stage, so any output is stale audio.
// `ringBlocks` is how long silence follows the burst before the bypass: long enough that the tail
// is audible, short enough that it has not decayed away (a long-delay echo needs longer).
template <typename Node, typename SetMix>
void expectCleanReenable(Node& n, SetMix&& setMix, int32_t ringBlocks) {
  int64_t t = 0;
  setMix(0.5f);
  runPeak(n, 40, burst, t);
  // The tail does ring on silent input while the mix is on (this is what must be preserved).
  CHECK(runPeak(n, ringBlocks, silence, t) > 0.01f);
  setMix(0.0f);
  runPeak(n, 400, silence, t);
  setMix(0.5f);
  CHECK(runPeak(n, 200, silence, t) == 0.0f);
}

void testDelay() {
  Delay n(kRate);
  n.setParam(static_cast<int32_t>(NodeParam::DelayTime), 0.3f);
  n.setParam(static_cast<int32_t>(NodeParam::DelayFeedback), 0.5f);
  expectCleanReenable(n, [&](float m) { n.setParam(static_cast<int32_t>(NodeParam::DelayMix), m); }, 150);
}

void testChorusFlanger() {
  ChorusFlanger n(kRate);
  n.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.6f);
  expectCleanReenable(n, [&](float m) { n.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), m); }, 8);
}

void testReverb() {
  Reverb n(kRate);
  n.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.9f);
  expectCleanReenable(n, [&](float m) { n.setParam(static_cast<int32_t>(NodeParam::ReverbMix), m); }, 8);
}

// A brief dip to mix 0 between two blocks (an LFO clamped at zero) must not break an unrelated,
// never-bypassed path: the effect that was never bypassed is untouched by the clearing.
void testNeverBypassedUnaffected() {
  Delay a(kRate), b(kRate);
  for (Delay* d : {&a, &b}) {
    d->setParam(static_cast<int32_t>(NodeParam::DelayTime), 0.1f);
    d->setParam(static_cast<int32_t>(NodeParam::DelayFeedback), 0.4f);
    d->setParam(static_cast<int32_t>(NodeParam::DelayMix), 0.5f);
  }
  int64_t ta = 0, tb = 0;
  const float pa = runPeak(a, 60, burst, ta);
  const float pb = runPeak(b, 60, burst, tb);
  CHECK(pa == pb);
  // Fresh effect that is only ever bypassed stays a pass-through (and costs no clear).
  Delay c(kRate);
  int64_t tc = 0;
  CHECK(runPeak(c, 4, burst, tc) > 0.0f);
}

}  // namespace

int main() {
  testDelay();
  testChorusFlanger();
  testReverb();
  testNeverBypassedUnaffected();
  std::printf("bypass_state_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
