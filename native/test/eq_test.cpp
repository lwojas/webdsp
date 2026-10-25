#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../src/fx_chain.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kEq = static_cast<int32_t>(FxKind::Eq);
constexpr int32_t kId = 1;

int32_t P(NodeParam p) { return static_cast<int32_t>(p); }
// Param id of band b (0..3), field f (0 on, 1 type, 2 freq, 3 gain, 4 q, 5 shelfSlope, 6 slope).
int32_t BP(int32_t b, int32_t f) { return P(NodeParam::EqBand1On) + b * 7 + f; }
enum { On, Type, Freq, Gain, Q, ShelfSlope, Slope };
enum { Bell, LowShelf, HighShelf, LowPass, HighPass, Notch };

struct Rig {
  FxChain chain;
  Rig() {
    chain.configure(kRate);
    chain.add(kId, kEq, -1);
  }
  void band(int32_t b, int32_t type, float freq, float gain = 0.0f, float q = 0.707f) {
    chain.setParam(kId, BP(b, On), 1.0f);
    chain.setParam(kId, BP(b, Type), static_cast<float>(type));
    chain.setParam(kId, BP(b, Freq), freq);
    chain.setParam(kId, BP(b, Gain), gain);
    chain.setParam(kId, BP(b, Q), q);
  }
  void run(std::vector<std::vector<float>>& data) {
    const int32_t nc = static_cast<int32_t>(data.size());
    const int32_t n = static_cast<int32_t>(data[0].size());
    for (int32_t at = 0; at < n; at += kBlock) {
      const int32_t len = std::min(kBlock, n - at);
      float* ch[2] = {data[0].data() + at, nc > 1 ? data[1].data() + at : nullptr};
      chain.process(ch, nc, len);
    }
  }
  // Steady-state gain in dB for a sine at `hz` (fresh state: measured after a settle period).
  double gainDb(double hz) {
    chain.reset();
    const int32_t n = 24000;
    std::vector<std::vector<float>> d(1, std::vector<float>(n));
    for (int32_t i = 0; i < n; i++) d[0][i] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * hz * i / kRate));
    run(d);
    double in = 0, out = 0;
    for (int32_t i = n / 2; i < n; i++) {
      in += 0.25 * std::pow(std::sin(2.0 * M_PI * hz * i / kRate), 2.0);
      out += static_cast<double>(d[0][i]) * d[0][i];
    }
    return 10.0 * std::log10(out / in);
  }
};

std::vector<float> noise(int32_t n) {
  std::vector<float> x(n);
  uint32_t s = 12345;
  for (float& v : x) {
    s = s * 1664525u + 1013904223u;
    v = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.5f;
  }
  return x;
}

void testDefaultsAreNeutral() {
  Rig rig;
  std::vector<std::vector<float>> d = {noise(1000), noise(1000)};
  const auto orig = d;
  rig.run(d);
  CHECK(d == orig);  // all bands off: bit-exact
  CHECK(fxParamCount(kEq) == 28);
  CHECK(fxKindForNodeParam(BP(3, Slope)) == kEq);
  CHECK(fxKindForNodeParam(BP(3, Slope) + 1) == -1);
  CHECK_NEAR(rig.chain.effectiveParam(kId, BP(0, Freq)), 100.0, 0.0);
  CHECK_NEAR(rig.chain.effectiveParam(kId, BP(3, Type)), 2.0, 0.0);
}

void testEnabledZeroGainIsTransparent() {
  Rig rig;
  rig.band(0, LowShelf, 100.0f);
  rig.band(1, Bell, 500.0f);
  rig.band(2, Bell, 2000.0f);
  rig.band(3, HighShelf, 8000.0f);
  std::vector<std::vector<float>> d = {noise(4000)};
  const auto orig = d;
  rig.run(d);
  for (size_t i = 0; i < d[0].size(); i++) CHECK_NEAR(d[0][i], orig[0][i], 1e-5);
}

void testBell() {
  Rig rig;
  rig.band(0, Bell, 1000.0f, 9.0f, 2.0f);
  CHECK_NEAR(rig.gainDb(1000.0), 9.0, 0.05);   // full gain at the centre
  CHECK(std::fabs(rig.gainDb(60.0)) < 0.3);    // untouched far away
  CHECK(std::fabs(rig.gainDb(15000.0)) < 0.3);
  rig.chain.setParam(kId, BP(0, Gain), -9.0f);
  CHECK_NEAR(rig.gainDb(1000.0), -9.0, 0.05);
  // Higher Q is narrower: an octave away the boost is smaller.
  rig.chain.setParam(kId, BP(0, Gain), 9.0f);
  rig.chain.setParam(kId, BP(0, Q), 0.5f);
  const double wide = rig.gainDb(2000.0);
  rig.chain.setParam(kId, BP(0, Q), 8.0f);
  CHECK(rig.gainDb(2000.0) < wide - 3.0);
}

void testShelves() {
  Rig rig;
  rig.band(0, LowShelf, 200.0f, 12.0f);
  CHECK_NEAR(rig.gainDb(30.0), 12.0, 0.3);
  CHECK(std::fabs(rig.gainDb(8000.0)) < 0.2);
  CHECK_NEAR(rig.gainDb(200.0), 6.0, 0.3);  // half the gain (in dB) at the corner
  rig.band(0, HighShelf, 4000.0f, -12.0f);
  CHECK_NEAR(rig.gainDb(18000.0), -12.0, 0.5);
  CHECK(std::fabs(rig.gainDb(100.0)) < 0.2);
  // A gentler slope reaches the plateau further from the corner.
  rig.chain.setParam(kId, BP(0, ShelfSlope), 0.2f);
  CHECK(rig.gainDb(8000.0) > -9.0);
}

void testLowAndHighPass() {
  Rig rig;
  rig.band(0, LowPass, 1000.0f);
  CHECK_NEAR(rig.gainDb(1000.0), -3.01, 0.1);  // Butterworth corner
  CHECK(std::fabs(rig.gainDb(100.0)) < 0.1);
  const double oct12 = rig.gainDb(4000.0);      // two octaves up
  CHECK(oct12 < -20.0 && oct12 > -28.0);        // ~12 dB/oct
  rig.chain.setParam(kId, BP(0, Slope), 24.0f);
  CHECK_NEAR(rig.gainDb(1000.0), -3.01, 0.1);
  CHECK(rig.gainDb(4000.0) < oct12 - 15.0);      // twice as steep
  rig.chain.setParam(kId, BP(0, Type), HighPass);
  CHECK_NEAR(rig.gainDb(1000.0), -3.01, 0.1);
  CHECK(rig.gainDb(250.0) < -30.0);
  CHECK(std::fabs(rig.gainDb(8000.0)) < 0.1);
  rig.chain.setParam(kId, BP(0, Slope), 12.0f);
  CHECK(rig.gainDb(250.0) > -28.0 && rig.gainDb(250.0) < -20.0);
}

void testNotch() {
  Rig rig;
  rig.band(0, Notch, 1000.0f, 0.0f, 4.0f);
  CHECK(rig.gainDb(1000.0) < -40.0);
  CHECK(std::fabs(rig.gainDb(100.0)) < 0.1);
  CHECK(std::fabs(rig.gainDb(10000.0)) < 0.1);
}

void testSeriesAddsInDb() {
  Rig rig;
  rig.band(0, Bell, 1000.0f, 6.0f, 1.0f);
  rig.band(1, Bell, 1000.0f, 5.0f, 1.0f);
  CHECK_NEAR(rig.gainDb(1000.0), 11.0, 0.1);
  rig.band(2, Bell, 1000.0f, -3.0f, 1.0f);
  CHECK_NEAR(rig.gainDb(1000.0), 8.0, 0.1);
}

void testEnableDisable() {
  Rig rig;
  rig.band(0, Bell, 1000.0f, 12.0f, 1.0f);
  CHECK_NEAR(rig.gainDb(1000.0), 12.0, 0.1);
  rig.chain.setParam(kId, BP(0, On), 0.0f);
  CHECK(std::fabs(rig.gainDb(1000.0)) < 1e-6);
  std::vector<std::vector<float>> d = {noise(500)};
  const auto orig = d;
  rig.run(d);
  CHECK(d == orig);
  rig.chain.setParam(kId, BP(0, On), 1.0f);
  CHECK_NEAR(rig.gainDb(1000.0), 12.0, 0.1);
}

void testStatePersistsAcrossBlocks() {
  // The same signal processed in one go and in odd-sized blocks must be identical.
  Rig a, b;
  for (Rig* r : {&a, &b}) {
    r->band(0, LowShelf, 150.0f, 7.0f);
    r->band(1, Bell, 900.0f, -6.0f, 3.0f);
    r->band(2, LowPass, 6000.0f);
    r->chain.setParam(kId, BP(2, Slope), 24.0f);
  }
  const std::vector<float> x = noise(3000);
  std::vector<float> one = x, many = x;
  float* c1[1] = {one.data()};
  a.chain.process(c1, 1, 3000);
  for (int32_t at = 0, len; at < 3000; at += len) {
    len = std::min(37 + at % 11, 3000 - at);
    float* cm[1] = {many.data() + at};
    b.chain.process(cm, 1, len);
  }
  for (int32_t i = 0; i < 3000; i++) CHECK_NEAR(one[i], many[i], 1e-6);
}

void testParamChangeMidStreamIsFinite() {
  Rig rig;
  rig.band(0, Bell, 1000.0f, 6.0f, 1.0f);
  std::vector<std::vector<float>> d = {noise(kBlock)};
  for (int32_t blk = 0; blk < 200; blk++) {
    d[0] = noise(kBlock);
    rig.chain.setParam(kId, BP(0, Freq), 40.0f + 90.0f * (blk % 200));
    rig.chain.setParam(kId, BP(0, Type), static_cast<float>(blk % 6));
    rig.run(d);
    for (float v : d[0]) CHECK(std::isfinite(v) && std::fabs(v) < 20.0f);
  }
}

void testExtremes() {
  Rig rig;
  for (int32_t t = 0; t < 6; t++) {
    for (float f : {1.0f, 20.0f, 20000.0f, 1e9f}) {
      for (float g : {-100.0f, 100.0f}) {
        rig.band(0, t, f, g, 0.0f);
        rig.chain.setParam(kId, BP(0, Slope), 24.0f);
        rig.chain.setParam(kId, BP(0, ShelfSlope), 0.0f);
        std::vector<std::vector<float>> d = {noise(512)};
        rig.run(d);
        for (float v : d[0]) CHECK(std::isfinite(v));
      }
    }
  }
  // Clamped to the documented ranges.
  Rig r2;
  r2.band(0, Bell, 1000.0f, 100.0f, 1.0f);
  CHECK_NEAR(r2.gainDb(1000.0), 18.0, 0.2);
  // Non-finite and unknown params are ignored.
  r2.chain.setParam(kId, BP(0, Gain), NAN);
  CHECK_NEAR(r2.gainDb(1000.0), 18.0, 0.2);
  CHECK(!r2.chain.setParam(kId, P(NodeParam::DelayTime), 0.5f));
}

void testStereoAndSilence() {
  Rig rig;
  rig.band(0, Bell, 1000.0f, 10.0f, 1.0f);
  std::vector<std::vector<float>> d = {noise(2000), std::vector<float>(2000, 0.0f)};
  rig.run(d);
  for (float v : d[1]) CHECK(v == 0.0f);  // channels are independent; silence stays silent
  double e = 0;
  for (float v : d[0]) e += v * v;
  CHECK(e > 0.0);
  // The right channel gets the same filter as the left.
  Rig r2;
  r2.band(0, Bell, 1000.0f, 10.0f, 1.0f);
  const std::vector<float> n = noise(2000);
  std::vector<std::vector<float>> s = {n, n};
  r2.run(s);
  CHECK(s[0] == s[1]);
}

void testModulationMetadata() {
  CHECK(resolveModDomain(*fxParamInfo(kEq, 2)) == ModDomain::Log);     // freq
  CHECK(resolveModDomain(*fxParamInfo(kEq, 3)) == ModDomain::Linear);  // gain
  CHECK(resolveModDomain(*fxParamInfo(kEq, 0)) == ModDomain::None);    // on
  CHECK(resolveModDomain(*fxParamInfo(kEq, 1)) == ModDomain::None);    // type
  CHECK(resolveModDomain(*fxParamInfo(kEq, 6)) == ModDomain::None);    // slope
  CHECK(fxParamCount(kEq) <= kMaxModParams);
}

}  // namespace

int main() {
  testDefaultsAreNeutral();
  testEnabledZeroGainIsTransparent();
  testBell();
  testShelves();
  testLowAndHighPass();
  testNotch();
  testSeriesAddsInDb();
  testEnableDisable();
  testStatePersistsAcrossBlocks();
  testParamChangeMidStreamIsFinite();
  testExtremes();
  testStereoAndSilence();
  testModulationMetadata();
  std::printf("eq_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
