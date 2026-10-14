#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers stereo panning (native/src/dsp/panner.h; Voice VoiceParam::Pan, Bus NodeParam::BusPan,
// trigger/schedule `pan`). Asserts on rendered output buffers, not parameter values. See
// docs/panning.md.
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr float kLevel = 0.5f;
constexpr float kSqrt2 = 1.41421356237f;

struct Out {
  std::vector<float> l, r;
};

// Constant-level sample. `stereo` makes a 2-channel one with distinct L/R levels.
void commit(Engine& e, int32_t id, int32_t frames, bool stereo, float lvlL = kLevel,
            float lvlR = kLevel) {
  const int32_t nch = stereo ? 2 : 1;
  float* ch[2] = {nullptr, nullptr};
  for (int32_t c = 0; c < nch; c++) {
    ch[c] = static_cast<float*>(std::calloc(frames, sizeof(float)));
    for (int32_t i = 0; i < frames; i++) ch[c][i] = c == 0 ? lvlL : lvlR;
  }
  e.commitSample(id, nch, frames, static_cast<int32_t>(kRate), ch);
}

void render(Engine& e, int64_t& frame, int32_t blocks, Out* out) {
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    if (!out) continue;
    out->l.insert(out->l.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
    out->r.insert(out->r.end(), e.outputChannel(1), e.outputChannel(1) + kBlock);
  }
}

float peak(const std::vector<float>& v, size_t from = 0) {
  float p = 0;
  for (size_t i = from; i < v.size(); i++) p = std::max(p, std::fabs(v[i]));
  return p;
}

// Plays one looping voice at `pan` through the trigger ABI and returns 4 blocks of output.
Out playMono(float pan, bool stereo = false, float lvlL = kLevel, float lvlR = kLevel) {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 4800, stereo, lvlL, lvlR);
  int64_t f = 0;
  Out o;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false, -1, pan);
  render(e, f, 4, &o);
  return o;
}

void testDefaultIsCentreAndBitIdentical() {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 4800, false);
  int64_t f = 0;
  Out o;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);  // pan omitted: default
  render(e, f, 4, &o);
  // Pre-pan behaviour: a mono sample duplicated unscaled onto both channels (envelope has
  // reached sustain well before the last block).
  const size_t tail = o.l.size() - kBlock;
  for (size_t i = tail; i < o.l.size(); i++) {
    CHECK(o.l[i] == kLevel);
    CHECK(o.r[i] == kLevel);
  }
  // Explicit pan 0 renders identically to the default.
  Out c = playMono(0.0f);
  for (size_t i = 0; i < o.l.size(); i++) CHECK(c.l[i] == o.l[i] && c.r[i] == o.r[i]);
}

void testMonoHardPanIsExactlySilentOnFarSide() {
  Out left = playMono(-1.0f);
  CHECK(peak(left.r) == 0.0f);
  CHECK_NEAR(peak(left.l), kLevel * kSqrt2, 1e-5);
  Out right = playMono(1.0f);
  CHECK(peak(right.l) == 0.0f);
  CHECK_NEAR(peak(right.r), kLevel * kSqrt2, 1e-5);
}

void testMonoConstantPowerLaw() {
  const float pans[] = {-0.75f, -0.5f, -0.25f, 0.1f, 0.5f, 0.9f};
  for (float p : pans) {
    Out o = playMono(p);
    const size_t i = o.l.size() - 1;
    const double theta = (p + 1.0) * 3.14159265358979 / 4.0;
    CHECK_NEAR(o.l[i], kLevel * kSqrt2 * std::cos(theta), 1e-5);
    CHECK_NEAR(o.r[i], kLevel * kSqrt2 * std::sin(theta), 1e-5);
    // Constant power: L^2 + R^2 equals the centred (unity) value 2 * level^2.
    CHECK_NEAR(o.l[i] * o.l[i] + o.r[i] * o.r[i], 2.0 * kLevel * kLevel, 1e-5);
  }
  // Symmetry.
  Out a = playMono(-0.3f), b = playMono(0.3f);
  CHECK_NEAR(a.l.back(), b.r.back(), 1e-6);
  CHECK_NEAR(a.r.back(), b.l.back(), 1e-6);
}

void testStereoSourceIsBalanced() {
  // Distinct channel levels so channel mix-up would be visible.
  Out centre = playMono(0.0f, true, 0.6f, 0.2f);
  CHECK(centre.l.back() == 0.6f && centre.r.back() == 0.2f);
  Out left = playMono(-1.0f, true, 0.6f, 0.2f);
  CHECK(peak(left.r) == 0.0f);
  CHECK_NEAR(left.l.back(), 0.6f, 1e-6);  // near side untouched: no boost, no cross-mix
  Out right = playMono(1.0f, true, 0.6f, 0.2f);
  CHECK(peak(right.l) == 0.0f);
  CHECK_NEAR(right.r.back(), 0.2f, 1e-6);
  Out half = playMono(0.5f, true, 0.6f, 0.2f);
  CHECK_NEAR(half.l.back(), 0.6 * std::cos(0.5 * 3.14159265358979 / 2.0), 1e-5);
  CHECK_NEAR(half.r.back(), 0.2f, 1e-6);
}

void testScheduledEventCarriesPan() {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 4800, false);
  int64_t f = 0;
  e.scheduleEvent(256, 1, 1, 0, 1.0f, 1.0f, 0, -1, true, false, -1, -1.0f);
  Out o;
  render(e, f, 4, &o);
  CHECK(peak(o.l) > 0.1f);
  CHECK(peak(o.r) == 0.0f);  // hard left from the very first sample: no glide-in from centre
}

void testSetVoiceParamPansLiveVoiceWithoutClick() {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 48000, false);
  int64_t f = 0;
  Out o;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  render(e, f, 4, nullptr);
  e.setVoiceParam(1, static_cast<int32_t>(VoiceParam::Pan), 1.0f);
  render(e, f, 40, &o);  // ~107 ms: well past the ~5 ms smoother settling to exact target
  // Smoothed: no per-sample step anywhere near the full 0.5 swing.
  float maxStep = 0;
  for (size_t i = 1; i < o.l.size(); i++)
    maxStep = std::max(maxStep, std::fabs(o.l[i] - o.l[i - 1]));
  CHECK(maxStep < 0.05f);
  // Settles exactly: left silent, right at sqrt(2) * level.
  CHECK(o.l.back() == 0.0f);
  CHECK_NEAR(o.r.back(), kLevel * kSqrt2, 1e-5);
  // And back to centre restores the exact unpanned output (no residual DSP-state damage).
  e.setVoiceParam(1, static_cast<int32_t>(VoiceParam::Pan), 0.0f);
  Out back;
  render(e, f, 40, &back);
  CHECK_NEAR(back.l.back(), kLevel, 0);
  CHECK_NEAR(back.r.back(), kLevel, 0);
}

void testBusPanBalancesBusMix() {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 4800, true, 0.6f, 0.2f);
  e.setBusParam(1, static_cast<int32_t>(NodeParam::BusPan), -1.0f);
  int64_t f = 0;
  Out o;
  e.trigger(1, 1, /*busId=*/1, 1.0f, 1.0f, 0, -1, true, false);
  render(e, f, 4, &o);
  CHECK(peak(o.r) == 0.0f);
  CHECK_NEAR(o.l.back(), 0.6f, 1e-6);
}

void testBusPanDefaultAndOtherBusesUnaffected() {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 4800, false);
  e.setBusParam(2, static_cast<int32_t>(NodeParam::BusPan), 1.0f);  // pans bus 2 only
  int64_t f = 0;
  Out o;
  e.trigger(1, 1, 1, 1.0f, 1.0f, 0, -1, true, false);
  render(e, f, 4, &o);
  CHECK(o.l.back() == kLevel && o.r.back() == kLevel);
}

void testMasterBusPanAndBusGainCompose() {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 4800, false);
  e.setBusParam(0, static_cast<int32_t>(NodeParam::BusPan), 1.0f);
  e.setBusParam(0, static_cast<int32_t>(NodeParam::BusGain), 0.5f);
  int64_t f = 0;
  Out o;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  render(e, f, 8, &o);
  CHECK(o.l.back() == 0.0f);
  CHECK_NEAR(o.r.back(), kLevel * 0.5f, 1e-6);  // balance leaves the near side at unity
}

void testGainAndPanMultiply() {
  Engine e;
  e.init(kRate, 2, 8);
  commit(e, 1, 4800, false);
  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 0.5f);
  int64_t f = 0;
  Out o;
  e.trigger(1, 1, 0, 0.5f, 1.0f, 0, -1, true, false, -1, 1.0f);
  render(e, f, 4, &o);
  CHECK_NEAR(o.r.back(), kLevel * 0.5f * 0.5f * kSqrt2, 1e-5);
}

void testInvalidPanIsSafe() {
  Out nan = playMono(std::nanf(""));
  CHECK(nan.l.back() == kLevel && nan.r.back() == kLevel);
  Out big = playMono(5.0f);  // clamped to +1
  CHECK(peak(big.l) == 0.0f);
}

void testRecycledVoiceDoesNotInheritPan() {
  Engine e;
  e.init(kRate, 2, 1);  // one voice, guaranteed reuse
  commit(e, 1, 4800, false);
  int64_t f = 0;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false, -1, -1.0f);
  render(e, f, 2, nullptr);
  e.stop(1);
  render(e, f, 1, nullptr);
  Out o;
  e.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, true, false);  // no pan -> centre
  render(e, f, 4, &o);
  CHECK(o.l.back() == kLevel && o.r.back() == kLevel);
}

}  // namespace

int main() {
  testDefaultIsCentreAndBitIdentical();
  testMonoHardPanIsExactlySilentOnFarSide();
  testMonoConstantPowerLaw();
  testStereoSourceIsBalanced();
  testScheduledEventCarriesPan();
  testSetVoiceParamPansLiveVoiceWithoutClick();
  testBusPanBalancesBusMix();
  testBusPanDefaultAndOtherBusesUnaffected();
  testMasterBusPanAndBusGainCompose();
  testGainAndPanMultiply();
  testInvalidPanIsSafe();
  testRecycledVoiceDoesNotInheritPan();
  std::printf("pan_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
