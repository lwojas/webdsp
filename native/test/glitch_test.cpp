#include <cmath>
#include <cstdint>
#include <random>
#include <vector>
#include "../src/fx_chain.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr double kBpm = 120.0;
constexpr double kBeat = kRate * 60.0 / kBpm;  // 24000 frames
constexpr int32_t kGlitch = static_cast<int32_t>(FxKind::Glitch);
constexpr int32_t kId = 1;

int32_t P(NodeParam p) { return static_cast<int32_t>(p); }

struct Rig {
  FxChain chain;
  Rig() {
    chain.configure(kRate);
    chain.setClock(0, 0.0, kBeat);
    chain.add(kId, kGlitch, -1);
  }
  void set(NodeParam p, float v) { chain.setParam(kId, P(p), v); }
  // Processes `data` (planar, same length per channel) in place in kBlock chunks.
  void run(std::vector<std::vector<float>>& data) {
    const int32_t nc = static_cast<int32_t>(data.size());
    const int32_t n = static_cast<int32_t>(data[0].size());
    for (int32_t at = 0; at < n; at += kBlock) {
      const int32_t len = std::min(kBlock, n - at);
      float* ch[2] = {data[0].data() + at, nc > 1 ? data[1].data() + at : nullptr};
      chain.process(ch, nc, len);
    }
  }
};

std::vector<float> noise(int32_t n, uint32_t seed = 7) {
  std::mt19937 g(seed);
  std::uniform_real_distribution<float> d(-0.5f, 0.5f);
  std::vector<float> x(n);
  for (float& v : x) v = d(g);
  return x;
}

std::vector<float> sine(int32_t n, double hz) {
  std::vector<float> x(n);
  for (int32_t i = 0; i < n; i++) x[i] = static_cast<float>(0.5 * std::sin(6.283185307179586 * hz * i / kRate));
  return x;
}

double rms(const std::vector<float>& x, int32_t from, int32_t to) {
  double s = 0;
  for (int32_t i = from; i < to; i++) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / std::max(1, to - from));
}

int32_t zeroCrossings(const std::vector<float>& x, int32_t from, int32_t to) {
  int32_t n = 0;
  for (int32_t i = from + 1; i < to; i++) if ((x[i - 1] < 0) != (x[i] < 0)) n++;
  return n;
}

void bypassIsExact() {
  Rig r;
  auto in = noise(20000);
  std::vector<std::vector<float>> d{in, in};
  r.run(d);
  CHECK(d[0] == in);
  CHECK(d[1] == in);
}

void repeatLoopsTheLastDivision() {
  Rig r;
  r.set(NodeParam::GlitchMode, 0);
  r.set(NodeParam::GlitchDivision, 1);  // 1/4 = one beat = kBeat frames
  const int32_t L = static_cast<int32_t>(kBeat);
  auto in = noise(L * 12);
  // history first, then engage, then more (different) input that must NOT be heard
  const int32_t engageAt = L * 4;
  std::vector<std::vector<float>> a{std::vector<float>(in.begin(), in.begin() + engageAt)};
  r.run(a);
  r.set(NodeParam::GlitchMix, 1.0f);
  std::vector<std::vector<float>> b{std::vector<float>(in.begin() + engageAt, in.end())};
  r.run(b);
  const auto& out = b[0];
  // after the 8 ms mix glide the output is the loop: periodic with period L, apart from the seam fades
  const int32_t settle = static_cast<int32_t>(kRate * 0.15);  // the mix glide (8 ms time constant) has finished
  double maxDiff = 0;
  for (int32_t i = L + settle; i < 6 * L; i++) {
    const int32_t phase = i % L;
    if (phase < 200 || phase > L - 200) continue;  // seam window
    maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(out[i] - out[i - L])));
  }
  CHECK(maxDiff < 1e-4);
  // and it is the captured slice, not the new input: the loop equals the last L frames before the engage
  double match = 0;
  for (int32_t i = settle; i < L - 300; i++) match = std::max(match, static_cast<double>(std::fabs(out[i] - a[0][engageAt - L + i])));
  CHECK(match < 1e-3);
  CHECK(rms(out, settle, 6 * L) > 0.1);
}

// speed -1 plays the captured slice backwards; a division change shortens the loop at its wrap, keeping the newest slice
void repeatReverseAndRoll() {
  const int32_t L = static_cast<int32_t>(kBeat);
  {
    Rig r;
    r.set(NodeParam::GlitchDivision, 1);
    r.set(NodeParam::GlitchSpeed, -1.0f);
    auto hist = noise(L * 2);
    std::vector<std::vector<float>> a{hist};
    r.run(a);
    r.set(NodeParam::GlitchMix, 1.0f);
    std::vector<std::vector<float>> b{noise(L * 2, 5)};
    r.run(b);
    // the loop plays from its end to its start: engage-relative frame j is the slice's frame (2L - j), after the mix glide
    double maxDiff = 0;
    for (int32_t j = 7200; j < L - 600; j++) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(b[0][j] - hist[L * 2 - j])));
    CHECK(maxDiff < 1e-3);
    CHECK(rms(b[0], 8000, L - 600) > 0.1);
  }
  {
    Rig r;
    r.set(NodeParam::GlitchDivision, 1);  // 1/4
    auto hist = noise(L * 2);
    std::vector<std::vector<float>> a{hist};
    r.run(a);
    r.set(NodeParam::GlitchMix, 1.0f);
    std::vector<std::vector<float>> b{noise(L, 6)};
    r.run(b);  // one full loop of the 1/4
    r.set(NodeParam::GlitchDivision, 3);  // 1/16: from the next wrap the loop is the newest L/4 of the capture
    std::vector<std::vector<float>> c{noise(L * 2, 7)};
    r.run(c);
    const int32_t S = L / 4;
    const int32_t settle = static_cast<int32_t>(kRate * 0.15);
    // after the wrap: periodic with the short length, and equal to the newest quarter of the captured history
    double maxDiff = 0;
    for (int32_t k = 600; k < S - 600; k++) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(c[0][k + S] - c[0][k + 2 * S])));
    CHECK(maxDiff < 1e-3);
    double newest = 0;
    for (int32_t k = 600; k < S - 600; k++) newest = std::max(newest, static_cast<double>(std::fabs(c[0][k + S] - hist[L * 2 - S + k])));
    CHECK(newest < 1e-3);
    (void)settle;
  }
}

void tapeAtUnitySpeedIsTransparent() {
  Rig r;
  r.set(NodeParam::GlitchMode, 1);
  auto in = sine(30000, 440);
  std::vector<std::vector<float>> a{std::vector<float>(in.begin(), in.begin() + 10000)};
  r.run(a);
  r.set(NodeParam::GlitchMix, 1.0f);
  std::vector<std::vector<float>> b{std::vector<float>(in.begin() + 10000, in.end())};
  r.run(b);
  double maxDiff = 0;
  for (int32_t i = 0; i < static_cast<int32_t>(b[0].size()); i++) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(b[0][i] - in[10000 + i])));
  CHECK(maxDiff < 1e-4);
}

void tapeHalfSpeedHalvesThePitch() {
  Rig r;
  r.set(NodeParam::GlitchMode, 1);
  auto in = sine(40000, 1000);
  std::vector<std::vector<float>> a{std::vector<float>(in.begin(), in.begin() + 4000)};
  r.run(a);
  r.set(NodeParam::GlitchSpeed, 0.5f);
  r.set(NodeParam::GlitchMix, 1.0f);
  std::vector<std::vector<float>> b{std::vector<float>(in.begin() + 4000, in.end())};
  r.run(b);
  const int32_t from = 2000, to = 20000;
  const double ratio = static_cast<double>(zeroCrossings(b[0], from, to)) / zeroCrossings(in, 4000 + from, 4000 + to);
  CHECK_NEAR(ratio, 0.5, 0.03);
}

void tapeStopFadesToSilenceAndReleaseReturnsExact() {
  Rig r;
  r.set(NodeParam::GlitchMode, 1);
  auto in = sine(60000, 300);
  std::vector<std::vector<float>> a{std::vector<float>(in.begin(), in.begin() + 4000)};
  r.run(a);
  r.set(NodeParam::GlitchMix, 1.0f);
  r.set(NodeParam::GlitchSpeed, 0.0f);
  std::vector<std::vector<float>> b{std::vector<float>(in.begin() + 4000, in.begin() + 24000)};
  r.run(b);
  CHECK(rms(b[0], 12000, 20000) < 1e-3);
  r.set(NodeParam::GlitchMix, 0.0f);
  std::vector<std::vector<float>> c{std::vector<float>(in.begin() + 24000, in.end())};
  r.run(c);
  // after the release glide the node is the exact bypass again
  double maxDiff = 0;
  for (int32_t i = 6000; i < static_cast<int32_t>(c[0].size()); i++) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(c[0][i] - in[24000 + i])));
  CHECK(maxDiff == 0.0);
}

void gateChopsAtTheDivision() {
  Rig r;
  r.set(NodeParam::GlitchMode, 2);
  r.set(NodeParam::GlitchDivision, 3);  // 1/16: period kBeat/4 = 6000 frames, open for 3000 then closed for 3000
  r.set(NodeParam::GlitchMix, 1.0f);
  std::vector<std::vector<float>> d{noise(30000)};
  r.run(d);
  const int32_t P6 = static_cast<int32_t>(kBeat / 4);
  for (int32_t k = 1; k < 4; k++) {
    CHECK(rms(d[0], k * P6 + 300, k * P6 + P6 / 2 - 300) > 0.2);       // open
    CHECK(rms(d[0], k * P6 + P6 / 2 + 300, k * P6 + P6 - 300) < 1e-3);  // closed
  }
}

void gateDepthBlendsDry() {
  Rig r;
  r.set(NodeParam::GlitchMode, 2);
  r.set(NodeParam::GlitchDivision, 3);
  r.set(NodeParam::GlitchMix, 0.5f);
  std::vector<std::vector<float>> d{noise(30000)};
  auto in = d[0];
  r.run(d);
  const int32_t P6 = static_cast<int32_t>(kBeat / 4);
  // closed half: half the input remains
  const double closed = rms(d[0], P6 + P6 / 2 + 300, 2 * P6 - 300);
  const double dry = rms(in, P6 + P6 / 2 + 300, 2 * P6 - 300);
  CHECK_NEAR(closed / dry, 0.5, 0.02);
}

void followsTempo() {
  Rig r;
  r.set(NodeParam::GlitchMode, 2);
  r.set(NodeParam::GlitchDivision, 3);
  r.set(NodeParam::GlitchMix, 1.0f);
  r.chain.setClock(0, 0.0, kRate * 60.0 / 60.0);  // 60 BPM: twice as long
  std::vector<std::vector<float>> d{noise(60000)};
  r.run(d);
  const int32_t P12 = 12000;
  CHECK(rms(d[0], P12 + 500, P12 + P12 / 2 - 500) > 0.2);
  CHECK(rms(d[0], P12 + P12 / 2 + 500, 2 * P12 - 500) < 1e-3);
}

void stereoImageIsKept() {
  Rig r;
  r.set(NodeParam::GlitchMode, 0);
  r.set(NodeParam::GlitchDivision, 3);
  auto left = noise(24000, 1), right = noise(24000, 2);
  std::vector<std::vector<float>> a{left, right};
  r.run(a);
  r.set(NodeParam::GlitchMix, 1.0f);
  std::vector<std::vector<float>> b{noise(12000, 3), noise(12000, 4)};
  r.run(b);
  double diff = 0;
  for (int32_t i = 3000; i < 9000; i++) diff = std::max(diff, static_cast<double>(std::fabs(b[0][i] - b[1][i])));
  CHECK(diff > 0.1);  // the channels are still different signals
}

void paramsAreClampedAndOutputBounded() {
  Rig r;
  std::mt19937 g(99);
  std::uniform_real_distribution<float> u(-3.0f, 6.0f);
  for (int32_t round = 0; round < 60; round++) {
    r.set(NodeParam::GlitchMix, u(g));
    r.set(NodeParam::GlitchMode, u(g));
    r.set(NodeParam::GlitchDivision, u(g));
    r.set(NodeParam::GlitchSpeed, u(g));
    std::vector<std::vector<float>> d{noise(3000, round), noise(3000, round + 100)};
    r.run(d);
    for (const auto& ch : d) for (float v : ch) { CHECK(std::isfinite(v)); CHECK(std::fabs(v) <= 1.0f); }
  }
  r.set(NodeParam::GlitchMix, NAN);
}

void registration() {
  CHECK(fxKindForNodeParam(P(NodeParam::GlitchMix)) == kGlitch);
  CHECK(fxKindForNodeParam(P(NodeParam::GlitchSpeed)) == kGlitch);
  CHECK(fxKindForNodeParam(P(NodeParam::EqBand4Slope)) == static_cast<int32_t>(FxKind::Eq));
  CHECK(fxParamCount(kGlitch) == 4);
  CHECK(fxParamInfo(kGlitch, 0)->id == P(NodeParam::GlitchMix));
  CHECK(kFxKindCount == 12);
}

}  // namespace

int main() {
  bypassIsExact();
  repeatLoopsTheLastDivision();
  repeatReverseAndRoll();
  tapeAtUnitySpeedIsTransparent();
  tapeHalfSpeedHalvesThePitch();
  tapeStopFadesToSilenceAndReleaseReturnsExact();
  gateChopsAtTheDivision();
  gateDepthBlendsDry();
  followsTempo();
  stereoImageIsKept();
  paramsAreClampedAndOutputBounded();
  registration();
  std::printf("glitch: %d checks, %d failed\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
