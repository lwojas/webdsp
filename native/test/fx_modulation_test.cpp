#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "../src/engine.h"
#include "../src/fx_chain.h"
#include "../src/params.h"
#include "test_main.h"

// FX parameter tables, the Utility FX, and LFO modulation of effect instances (FxChain::setLfo):
// the same ParamModulation bank synths use, attached to a chain slot. See docs/modulation.md.
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kFilter = static_cast<int32_t>(FxKind::Filter);
constexpr int32_t kDelay = static_cast<int32_t>(FxKind::Delay);
constexpr int32_t kUtil = static_cast<int32_t>(FxKind::Utility);
constexpr int32_t kGain = static_cast<int32_t>(NodeParam::UtilityGain);
constexpr int32_t P(NodeParam p) { return static_cast<int32_t>(p); }

FxChain makeChain() {
  FxChain c;
  c.configure(kRate);
  return c;
}

LfoConfig lfo(int32_t target, LfoWaveform w, float amount, float hz = 1.0f) {
  LfoConfig c;
  c.enabled = true;
  c.target = target;
  c.waveform = w;
  c.amount = amount;
  c.rateHz = hz;
  return c;
}

// Runs `blocks` blocks of a 0.5-amplitude 220 Hz sine through the chain. `each(block)` runs
// before each block; samples (left channel) are appended to `out`.
template <typename F>
void run(FxChain& c, int32_t blocks, std::vector<float>* out, F&& each, int64_t startFrame = 0) {
  static int64_t phase = 0;
  for (int32_t b = 0; b < blocks; b++) {
    each(b);
    c.setClock(startFrame + static_cast<int64_t>(b) * kBlock, 0.0, kRate * 0.5);
    float l[kBlock], r[kBlock];
    for (int32_t i = 0; i < kBlock; i++) {
      const double t = static_cast<double>(startFrame + static_cast<int64_t>(b) * kBlock + i) / kRate;
      l[i] = r[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * t));
    }
    (void)phase;
    float* ch[2] = {l, r};
    c.process(ch, 2, kBlock);
    if (out) out->insert(out->end(), l, l + kBlock);
  }
}
template <typename... A>
void run(FxChain& c, int32_t blocks, std::vector<float>* out) {
  run(c, blocks, out, [](int32_t) {});
}

double peak(const std::vector<float>& v, size_t a, size_t b) {
  double m = 0;
  for (size_t i = a; i < b && i < v.size(); i++) m = std::max(m, static_cast<double>(std::fabs(v[i])));
  return m;
}

void testParamTables() {
  for (int32_t k = 0; k < kFxKindCount; k++) {
    const int32_t n = fxParamCount(k);
    CHECK(n > 0 && n <= kMaxModParams);
    std::set<std::string> names;
    for (int32_t i = 0; i < n; i++) {
      const InstrumentParamInfo* p = fxParamInfo(k, i);
      CHECK(p != nullptr);
      CHECK(fxKindForNodeParam(p->id) == k);  // table ids belong to their kind
      CHECK(p->min < p->max);
      CHECK(p->def >= p->min && p->def <= p->max);
      CHECK(names.insert(p->name).second);  // names unique within a kind
    }
  }
  CHECK(fxParamCount(kFxKindCount) == 0);
  CHECK(fxParamInfo(kFilter, 99) == nullptr);
  // Domains: Hz -> log, plain ranges -> linear, steps and delay time -> not targets.
  auto dom = [](int32_t kind, int32_t id) {
    for (int32_t i = 0; i < fxParamCount(kind); i++)
      if (fxParamInfo(kind, i)->id == id) return resolveModDomain(*fxParamInfo(kind, i));
    return ModDomain::Auto;
  };
  CHECK(dom(kFilter, P(NodeParam::FilterCutoff)) == ModDomain::Log);
  CHECK(dom(kFilter, P(NodeParam::FilterResonance)) == ModDomain::Linear);
  CHECK(dom(kFilter, P(NodeParam::FilterMode)) == ModDomain::None);
  CHECK(dom(kDelay, P(NodeParam::DelayTime)) == ModDomain::None);
  CHECK(dom(kDelay, P(NodeParam::DelayMix)) == ModDomain::Linear);
  CHECK(dom(kUtil, kGain) == ModDomain::Linear);
}

void testUtilityGain() {
  // Unity is a bit-exact pass-through.
  {
    FxChain c = makeChain();
    CHECK(c.add(1, kUtil, -1));
    std::vector<float> a, b;
    run(c, 10, &a);
    FxChain none = makeChain();
    run(none, 10, &b);
    CHECK(a == b);
  }
  struct Case { float db; double lin; };
  for (const Case& cs : {Case{-6.0206f, 0.5}, Case{6.0206f, 2.0}, Case{12.0f, 3.98107}}) {
    FxChain c = makeChain();
    c.add(1, kUtil, -1);
    c.setParam(1, kGain, cs.db);
    std::vector<float> out;
    run(c, 20, &out);
    CHECK_NEAR(peak(out, 128, out.size()), 0.5 * cs.lin, 0.002 * cs.lin);
  }
  // The bottom of the range is true silence, and out-of-range / NaN input is safe.
  {
    FxChain c = makeChain();
    c.add(1, kUtil, -1);
    c.setParam(1, kGain, -60.0f);
    std::vector<float> out;
    run(c, 20, &out);
    CHECK(peak(out, 128, out.size()) == 0.0);
    c.setParam(1, kGain, -200.0f);
    c.setParam(1, kGain, std::nanf(""));
    out.clear();
    run(c, 4, &out);
    CHECK(peak(out, 0, out.size()) == 0.0);  // NaN ignored, -200 clamped to silence
  }
  // A gain step is smoothed: no sample-to-sample jump larger than the signal's own slope allows.
  {
    FxChain c = makeChain();
    c.add(1, kUtil, -1);
    std::vector<float> out;
    run(c, 4, &out);
    c.setParam(1, kGain, -30.0f);
    run(c, 40, &out, [](int32_t) {}, 4 * kBlock);
    double maxStep = 0;
    for (size_t i = 1; i < out.size(); i++) maxStep = std::max(maxStep, static_cast<double>(std::fabs(out[i] - out[i - 1])));
    CHECK(maxStep < 0.04);  // a 0.5 sine at 220 Hz moves at most ~0.014/sample
    CHECK(peak(out, out.size() - 128, out.size()) < 0.5 * 0.035 * 1.2);
  }
}

void testLfoMovesParam() {
  FxChain c = makeChain();
  c.add(1, kUtil, -1);
  c.setParam(1, kGain, -12.0f);
  CHECK(c.setLfo(1, 0, lfo(kGain, LfoWaveform::Sine, 0.25f, 2.0f), 0.0));
  float lo = 1e9f, hi = -1e9f;
  std::vector<float> out;
  run(c, 750, &out, [&](int32_t) {
    const float e = c.effectiveParam(1, kGain);
    lo = std::min(lo, e);
    hi = std::max(hi, e);
  });
  // amount 0.25 of a 72 dB range = +-9 dB around the -12 dB base.
  CHECK_NEAR(hi, -3.0, 0.6);
  CHECK_NEAR(lo, -21.0, 0.6);
  CHECK_NEAR(c.baseParam(1, kGain), -12.0, 1e-6);
}

void testStackedLfosAdd() {
  auto effectiveAt = [&](bool two) {
    FxChain c = makeChain();
    c.add(1, kUtil, -1);
    c.setParam(1, kGain, -20.0f);
    if (two) {
      c.setLfo(1, 0, lfo(kGain, LfoWaveform::Square, 0.1f, 1.0f), 0.0);
      c.setLfo(1, 1, lfo(kGain, LfoWaveform::Square, 0.1f, 1.0f), 0.0);
    } else {
      c.setLfo(1, 0, lfo(kGain, LfoWaveform::Square, 0.2f, 1.0f), 0.0);
    }
    run(c, 100, nullptr);
    return c.effectiveParam(1, kGain);
  };
  CHECK_NEAR(effectiveAt(true), effectiveAt(false), 1e-3);
  CHECK(effectiveAt(true) > -20.0f + 5.0f);  // +0.2 * 36 dB = +7.2
}

void testAutomationBaseUnderLfo() {
  // The app keeps writing the base (automation does this) while an LFO is attached: the LFO
  // rides on the new base, and removing the LFO leaves the node at the latest base.
  FxChain c = makeChain();
  c.add(1, kUtil, -1);
  c.setParam(1, kGain, -20.0f);
  c.setLfo(1, 0, lfo(kGain, LfoWaveform::Square, 0.1f, 0.5f), 0.0);
  run(c, 40, nullptr);
  const float before = c.effectiveParam(1, kGain);
  CHECK_NEAR(std::fabs(before - (-20.0f)), 3.6, 0.05);
  c.setParam(1, kGain, -40.0f);
  CHECK_NEAR(c.baseParam(1, kGain), -40.0, 1e-6);
  run(c, 40, nullptr, [](int32_t) {}, 40 * kBlock);
  CHECK_NEAR(std::fabs(c.effectiveParam(1, kGain) - (-40.0f)), 3.6, 0.05);
  CHECK(c.setLfo(1, 0, LfoConfig{}, 0.0));  // disabled config removes it
  std::vector<float> out;
  run(c, 40, &out, [](int32_t) {}, 80 * kBlock);
  CHECK_NEAR(c.effectiveParam(1, kGain), -40.0, 1e-6);
  CHECK_NEAR(peak(out, out.size() - 128, out.size()), 0.5 * 0.01, 0.0005);
}

void testNoLfoBitIdenticalAndZeroAmount() {
  auto render = [&](bool withZeroLfo) {
    FxChain c = makeChain();
    c.add(1, kFilter, -1);
    c.setParam(1, P(NodeParam::FilterCutoff), 900.0f);
    c.setParam(1, P(NodeParam::FilterMode), 0.0f);
    if (withZeroLfo) c.setLfo(1, 0, lfo(P(NodeParam::FilterCutoff), LfoWaveform::Sine, 0.0f), 0.0);
    std::vector<float> out;
    run(c, 30, &out);
    return out;
  };
  CHECK(render(false) == render(true));
}

void testFilterCutoffLfoSafe() {
  FxChain c = makeChain();
  c.add(1, kFilter, -1);
  c.setParam(1, P(NodeParam::FilterCutoff), 1000.0f);
  c.setParam(1, P(NodeParam::FilterResonance), 20.0f);
  c.setParam(1, P(NodeParam::FilterMode), 0.0f);
  c.setLfo(1, 0, lfo(P(NodeParam::FilterCutoff), LfoWaveform::Square, 1.0f, 40.0f), 0.0);
  c.setLfo(1, 1, lfo(P(NodeParam::FilterCutoff), LfoWaveform::Saw, 1.0f, 33.0f), 0.0);
  std::vector<float> out;
  float lo = 1e9f, hi = 0;
  run(c, 800, &out, [&](int32_t) {
    const float e = c.effectiveParam(1, P(NodeParam::FilterCutoff));
    lo = std::min(lo, e);
    hi = std::max(hi, e);
  });
  bool finite = true;
  for (float x : out) finite = finite && std::isfinite(x) && std::fabs(x) < 50.0f;
  CHECK(finite);
  CHECK(lo >= 40.0f && hi <= 18000.0f);
  CHECK(hi > 4.0f * lo);  // it really moved
}

void testRejections() {
  FxChain c = makeChain();
  c.add(1, kDelay, -1);
  c.add(2, kUtil, -1);
  CHECK(!c.setLfo(1, 0, lfo(P(NodeParam::DelayTime), LfoWaveform::Sine, 0.5f), 0.0));  // not a target
  CHECK(!c.setLfo(1, 0, lfo(kGain, LfoWaveform::Sine, 0.5f), 0.0));                    // wrong kind
  CHECK(!c.setLfo(9, 0, lfo(kGain, LfoWaveform::Sine, 0.5f), 0.0));                    // unknown id
  CHECK(!c.setLfo(2, kMaxLfos, lfo(kGain, LfoWaveform::Sine, 0.5f), 0.0));             // bad slot
  CHECK(!c.setLfo(2, -1, lfo(kGain, LfoWaveform::Sine, 0.5f), 0.0));
  CHECK(c.setLfo(1, 0, lfo(P(NodeParam::DelayMix), LfoWaveform::Sine, 0.5f), 0.0));
  // Removing an instance drops its modulation; a re-added id starts clean.
  CHECK(c.remove(1));
  CHECK(c.baseParam(1, P(NodeParam::DelayMix)) == 0.0f);
}

void testDisabledFxIsSkipped() {
  FxChain c = makeChain();
  c.add(1, kUtil, -1);
  c.setLfo(1, 0, lfo(kGain, LfoWaveform::Square, 1.0f, 5.0f), 0.0);
  c.setEnabled(1, false);
  std::vector<float> a, b;
  run(c, 20, &a);
  FxChain none = makeChain();
  run(none, 20, &b);
  CHECK(a == b);
}

void testSyncedLfoFollowsBeatGrid() {
  FxChain c = makeChain();
  c.add(1, kUtil, -1);
  c.setParam(1, kGain, -24.0f);
  LfoConfig l = lfo(kGain, LfoWaveform::Square, 0.25f);
  l.sync = true;
  l.division = 4;  // "1/2": one cycle = 2 beats = 1 s at 120 BPM
  c.setLfo(1, 0, l, 0.0);
  std::vector<float> e;
  run(c, 800, nullptr, [&](int32_t) { e.push_back(c.effectiveParam(1, kGain)); });
  // 800 blocks = 2.13 s: the square flips every 0.5 s => 187.5 blocks.
  CHECK(e[50] > -24.0f);
  CHECK(e[250] < -24.0f);
  CHECK(e[450] > -24.0f);
}

void testEngineLevel() {
  Engine e;
  e.init(kRate, 2, 8);
  CHECK(e.addEffect(0, 1, kUtil));
  CHECK(e.setEffectParam(0, 1, kGain, -18.0f));
  LfoConfig l = lfo(kGain, LfoWaveform::Sine, 0.2f, 4.0f);
  CHECK(e.setEffectLfo(0, 1, 0, l));
  CHECK(!e.setEffectLfo(0, 7, 0, l));  // unknown effect
  CHECK(!e.setEffectLfo(99, 1, 0, l));          // unknown bus
  float lo = 1e9f, hi = -1e9f;
  int64_t frame = 0;
  for (int32_t b = 0; b < 600; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    const float v = e.effectEffectiveParam(0, 1, kGain);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  CHECK(hi - lo > 10.0f);
  CHECK_NEAR(e.effectBaseParam(0, 1, kGain), -18.0, 1e-6);
}

}  // namespace

int main() {
  testParamTables();
  testUtilityGain();
  testLfoMovesParam();
  testStackedLfosAdd();
  testAutomationBaseUnderLfo();
  testNoLfoBitIdenticalAndZeroAmount();
  testFilterCutoffLfoSafe();
  testRejections();
  testDisabledFxIsSkipped();
  testSyncedLfoFollowsBeatGrid();
  testEngineLevel();
  std::printf("fx_modulation_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
