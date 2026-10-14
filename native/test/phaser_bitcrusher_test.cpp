#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include "../src/dsp/bitcrusher.h"
#include "../src/dsp/phaser.h"
#include "../src/fx_chain.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr double kRate = 48000.0;
int32_t P(NodeParam p) { return static_cast<int32_t>(p); }

std::vector<float> sine(float hz, float amp, int32_t n) {
  std::vector<float> d(n);
  for (int32_t i = 0; i < n; i++) d[i] = amp * std::sin(2.0 * M_PI * hz * i / kRate);
  return d;
}

std::vector<float> noise(int32_t n) {
  std::vector<float> d(n);
  uint32_t s = 7;
  for (float& x : d) {
    s = s * 1664525u + 1013904223u;
    x = (static_cast<int32_t>(s >> 8) / static_cast<float>(1 << 23) - 1.0f) * 0.5f;
  }
  return d;
}

std::vector<float> run(DSPNode& node, std::vector<float> in) {
  std::vector<float> r = in;
  float* ch[2] = {in.data(), r.data()};
  for (size_t off = 0; off < in.size(); off += 128) {
    float* blk[2] = {ch[0] + off, ch[1] + off};
    node.process(blk, 2, static_cast<int32_t>(std::min<size_t>(128, in.size() - off)));
  }
  return in;
}

void testDefaultsAreBypass() {
  Phaser p(kRate);
  Bitcrusher b(kRate);
  std::vector<float> in = noise(1024);
  CHECK(run(p, in) == in);
  CHECK(run(b, in) == in);
}

void testPhaserColoursAndStaysFinite() {
  Phaser p(kRate);
  p.setParam(P(NodeParam::PhaserMix), 0.5f);
  p.setParam(P(NodeParam::PhaserFeedback), 0.9f);
  p.setParam(P(NodeParam::PhaserStages), 8);
  std::vector<float> in = noise(48000);
  std::vector<float> out = run(p, in);
  CHECK(out != in);
  float peak = 0;
  for (float x : out) {
    CHECK(std::isfinite(x));
    peak = std::max(peak, std::fabs(x));
  }
  CHECK(peak < 8.0f);
  // Silence in -> silence out (no self-oscillation from the feedback loop).
  Phaser q(kRate);
  q.setParam(P(NodeParam::PhaserMix), 1.0f);
  q.setParam(P(NodeParam::PhaserFeedback), 0.95f);
  std::vector<float> z = run(q, std::vector<float>(4096, 0.0f));
  CHECK(*std::max_element(z.begin(), z.end()) == 0.0f);
}

void testPhaserResetIsReproducible() {
  Phaser p(kRate);
  p.setParam(P(NodeParam::PhaserMix), 0.7f);
  p.setParam(P(NodeParam::PhaserFeedback), 0.5f);
  std::vector<float> in = noise(4096);
  std::vector<float> a = run(p, in);
  p.reset();
  CHECK(run(p, in) == a);
}

void testBitcrusherQuantizes() {
  Bitcrusher b(kRate);
  b.setParam(P(NodeParam::CrushBits), 3);
  b.setParam(P(NodeParam::CrushMix), 1.0f);
  std::vector<float> out = run(b, sine(440.0f, 0.9f, 4800));
  std::set<float> levels(out.begin(), out.end());
  CHECK(levels.size() <= 8);  // 3 bits -> at most 2^3 levels
  CHECK(levels.size() > 2);
}

void testBitcrusherDownsampleHolds() {
  Bitcrusher b(kRate);
  b.setParam(P(NodeParam::CrushDownsample), 4);
  b.setParam(P(NodeParam::CrushMix), 1.0f);
  std::vector<float> out = run(b, sine(440.0f, 0.5f, 400));
  for (size_t i = 0; i + 3 < out.size(); i += 4) {
    CHECK(out[i] == out[i + 1] && out[i] == out[i + 2] && out[i] == out[i + 3]);
  }
}

// Two phasers and two crushers in one chain stay independent and registered by kind.
void testChainIntegration() {
  FxChain c;
  c.configure(kRate);
  CHECK(c.add(1, static_cast<int32_t>(FxKind::Phaser), -1));
  CHECK(c.add(2, static_cast<int32_t>(FxKind::Phaser), -1));
  CHECK(c.add(3, static_cast<int32_t>(FxKind::Bitcrusher), -1));
  CHECK(c.setParam(1, P(NodeParam::PhaserMix), 0.5f));
  CHECK(!c.setParam(3, P(NodeParam::PhaserMix), 0.5f));  // wrong kind
  CHECK(c.setParam(3, P(NodeParam::CrushBits), 4));
  CHECK(fxKindForNodeParam(P(NodeParam::CrushMix)) == static_cast<int32_t>(FxKind::Bitcrusher));
  CHECK(fxKindForNodeParam(P(NodeParam::PhaserStages)) == static_cast<int32_t>(FxKind::Phaser));

  // Phaser 1 active, phaser 2 inert: chain == phaser 1 alone.
  FxChain solo;
  solo.configure(kRate);
  solo.add(1, static_cast<int32_t>(FxKind::Phaser), -1);
  solo.setParam(1, P(NodeParam::PhaserMix), 0.5f);
  c.remove(3);
  std::vector<float> in = noise(2048), a = in, b = in, a2 = in, b2 = in;
  float* ca[2] = {a.data(), a2.data()};
  float* cb[2] = {b.data(), b2.data()};
  c.process(ca, 2, 2048);
  solo.process(cb, 2, 2048);
  CHECK(a == b);
}

}  // namespace

int main() {
  testDefaultsAreBypass();
  testPhaserColoursAndStaysFinite();
  testPhaserResetIsReproducible();
  testBitcrusherQuantizes();
  testBitcrusherDownsampleHolds();
  testChainIntegration();
  std::printf("phaser_bitcrusher_test: %d/%d passed\n",
              webdsp::test::g_count - webdsp::test::g_failures, webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
