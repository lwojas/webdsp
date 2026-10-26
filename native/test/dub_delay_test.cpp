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
constexpr int32_t kDubDelay = static_cast<int32_t>(FxKind::DubDelay);
constexpr int32_t kId = 1;

int32_t P(NodeParam p) { return static_cast<int32_t>(p); }
double framesPerBeat(double bpm) { return kRate * 60.0 / bpm; }

struct Rig {
  FxChain chain;
  Rig(double bpm, int32_t division, float feedback, float toneHz, float mix) {
    chain.configure(kRate);
    chain.setClock(0, 0.0, framesPerBeat(bpm));
    chain.add(kId, kDubDelay, -1);
    chain.setParam(kId, P(NodeParam::DubDelayDivision), static_cast<float>(division));
    chain.setParam(kId, P(NodeParam::DubDelayFeedback), feedback);
    chain.setParam(kId, P(NodeParam::DubDelayTone), toneHz);
    chain.setParam(kId, P(NodeParam::DubDelayMix), mix);
  }
};

// Runs `chain` over `in` (mono or stereo planar) in kBlock chunks. Returns the processed channels.
std::vector<std::vector<float>> runChannels(FxChain& chain, std::vector<std::vector<float>> data) {
  const int32_t nc = static_cast<int32_t>(data.size());
  const int32_t n = static_cast<int32_t>(data[0].size());
  for (int32_t at = 0; at < n; at += kBlock) {
    const int32_t len = std::min(kBlock, n - at);
    float* ch[2] = {data[0].data() + at, nc > 1 ? data[1].data() + at : nullptr};
    chain.process(ch, nc, len);
  }
  return data;
}

std::vector<float> impulse(int32_t n) {
  std::vector<float> x(n, 0.0f);
  x[0] = 1.0f;
  return x;
}

std::vector<float> noise(int32_t n, uint32_t seed = 12345) {
  std::vector<float> x(n);
  for (float& v : x) {
    seed = seed * 1664525u + 1013904223u;
    v = (static_cast<int32_t>(seed >> 8) / static_cast<float>(1 << 23) - 1.0f) * 0.5f;
  }
  return x;
}

double sumAbs(const std::vector<float>& x, int32_t from, int32_t to) {
  double s = 0;
  for (int32_t i = from; i < to; i++) s += std::fabs(x[i]);
  return s;
}
int32_t peakIndex(const std::vector<float>& x, int32_t from) {
  int32_t best = from;
  for (int32_t i = from; i < static_cast<int32_t>(x.size()); i++) {
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  }
  return best;
}

// Each division at a known BPM puts the echo at the expected frame: 120 BPM = 125/250/375/500/750/
// 1000 ms for 1/16, 1/8, 1/8., 1/4, 1/4., 1/2.
void testDivisionsAt120() {
  // Indices 6..9 are the triplets: 1/16T, 1/8T, 1/4T, 1/2T = 1/3 of 250 ms * 1, 2, 4, 8 -> exact frames.
  const double expectMs[] = {125, 250, 375, 500, 750, 1000, 1000.0 / 12, 1000.0 / 6, 1000.0 / 3, 2000.0 / 3};
  for (int32_t d = 0; d < 10; d++) {
    Rig rig(120.0, d, 0.0f, 16000.0f, 1.0f);
    const int32_t n = 60000;
    auto out = runChannels(rig.chain, {impulse(n)})[0];
    const int32_t expected = static_cast<int32_t>(std::lround(expectMs[d] * kRate / 1000.0));
    const int32_t at = peakIndex(out, 1);
    CHECK(at == expected);
    CHECK(std::fabs(out[at]) > 0.99f);  // mix 1, feedback 0: the echo is the input, unattenuated
    CHECK(sumAbs(out, at + 1, n) == 0.0);  // and there is exactly one of it
  }
}

// The delay follows the transport: halving the tempo doubles the delay, set through setClock
// (the same call the engine makes every block), not through any parameter.
void testBpmDrivesTime() {
  Rig rig(120.0, 1, 0.0f, 16000.0f, 1.0f);  // 1/8 = 250 ms
  const int32_t n = 48000;
  auto first = runChannels(rig.chain, {impulse(n)})[0];
  CHECK(peakIndex(first, 1) == 12000);

  // Change tempo while running, let the glide settle on silence, then fire a new impulse.
  rig.chain.setClock(0, 0.0, framesPerBeat(60.0));
  runChannels(rig.chain, {std::vector<float>(48000, 0.0f)});
  auto second = runChannels(rig.chain, {impulse(n)})[0];
  CHECK(peakIndex(second, 1) == 24000);  // 1/8 at 60 BPM = 500 ms

  // A node added after the tempo was set starts on that tempo (no wait for the next setClock).
  FxChain late;
  late.configure(kRate);
  late.setClock(0, 0.0, framesPerBeat(90.0));
  late.add(7, kDubDelay, -1);
  late.setParam(7, P(NodeParam::DubDelayDivision), 3.0f);  // 1/4
  late.setParam(7, P(NodeParam::DubDelayMix), 1.0f);
  late.setParam(7, P(NodeParam::DubDelayFeedback), 0.0f);
  auto out = runChannels(late, {impulse(n)})[0];
  CHECK(peakIndex(out, 1) == static_cast<int32_t>(std::lround(framesPerBeat(90.0))));
}

// Feedback: 0 gives one echo; with feedback each repeat is `feedback` times the previous. The
// one-pole in the loop has unity DC gain, so the integral of an impulse echo is conserved by it.
void testFeedback() {
  const int32_t n = 12000 * 5;
  Rig none(120.0, 1, 0.0f, 16000.0f, 1.0f);
  auto a = runChannels(none.chain, {impulse(n)})[0];
  CHECK(sumAbs(a, 12001, n) == 0.0);

  Rig half(120.0, 1, 0.5f, 16000.0f, 1.0f);
  auto b = runChannels(half.chain, {impulse(n)})[0];
  const double e1 = sumAbs(b, 11990, 12010);
  const double e2 = sumAbs(b, 24000, 24100);
  const double e3 = sumAbs(b, 36000, 36200);
  CHECK_NEAR(e2 / e1, 0.5, 0.02);
  CHECK_NEAR(e3 / e2, 0.5, 0.02);

  // Out of range is clamped below unity: a runaway setting still decays.
  Rig hot(120.0, 0, 5.0f, 16000.0f, 1.0f);
  auto c = runChannels(hot.chain, {noise(96000)})[0];
  float peak = 0;
  for (float v : c) peak = std::max(peak, std::fabs(v));
  CHECK(peak < 20.0f);
}

// The tone filter sits INSIDE the loop: the first echo is untouched by it, every later repeat is
// progressively darker (a high-frequency measure — the sample-to-sample difference — falls).
void testToneIsInTheFeedbackLoop() {
  const int32_t n = 12000 * 4;
  std::vector<float> burst(n, 0.0f);
  const auto nz = noise(2000, 99);
  std::copy(nz.begin(), nz.end(), burst.begin());

  Rig bright(120.0, 1, 0.7f, 16000.0f, 1.0f);
  Rig dark(120.0, 1, 0.7f, 400.0f, 1.0f);
  auto b = runChannels(bright.chain, {burst})[0];
  auto d = runChannels(dark.chain, {burst})[0];

  auto hf = [](const std::vector<float>& x, int32_t from, int32_t to) {
    double s = 0;
    for (int32_t i = from + 1; i < to; i++) s += std::pow(x[i] - x[i - 1], 2);
    return s;
  };
  // Echo 1 (window 12000..14000): identical, the filter is not on the output.
  for (int32_t i = 12000; i < 14000; i++) CHECK_NEAR(b[i], d[i], 1e-6);
  // Echo 2 and 3: the dark setting has lost high frequencies relative to the bright one, and the
  // darkening compounds from one repeat to the next.
  const double r2 = hf(d, 24000, 26000) / hf(b, 24000, 26000);
  const double r3 = hf(d, 36000, 38000) / hf(b, 36000, 38000);
  CHECK(r2 < 0.3);
  CHECK(r3 < r2);
}

void testMix() {
  const int32_t n = 24000;
  const auto in = noise(n);

  Rig off(120.0, 1, 0.5f, 3000.0f, 0.0f);
  auto bypass = runChannels(off.chain, {in})[0];
  CHECK(bypass == in);  // mix 0: bit-exact pass-through

  Rig half(120.0, 1, 0.0f, 16000.0f, 0.5f);
  auto h = runChannels(half.chain, {in})[0];
  // Before the first echo only the dry half is there (after the ~10 ms mix glide).
  for (int32_t i = 8000; i < 12000; i++) CHECK_NEAR(h[i], in[i] * 0.5f, 1e-5);
  // Afterwards: dry/2 + echo/2 where the echo is the delayed (one-pole-free: feedback 0) input.
  for (int32_t i = 12000; i < 14000; i++) CHECK_NEAR(h[i], 0.5f * in[i] + 0.5f * in[i - 12000], 1e-5);

  // Mix back to 0 clears the memory: raising it again does not replay the old echoes.
  Rig re(120.0, 1, 0.9f, 16000.0f, 1.0f);
  runChannels(re.chain, {noise(n)});
  re.chain.setParam(kId, P(NodeParam::DubDelayMix), 0.0f);
  runChannels(re.chain, {std::vector<float>(4800, 0.0f)});  // glide to 0, then the one-time reset
  runChannels(re.chain, {std::vector<float>(128, 0.0f)});
  re.chain.setParam(kId, P(NodeParam::DubDelayMix), 1.0f);
  auto after = runChannels(re.chain, {std::vector<float>(24000, 0.0f)})[0];
  CHECK(sumAbs(after, 0, 24000) == 0.0);
}

// Every division at the slowest and fastest tempo stays inside the buffer: finite, bounded output,
// and the settled time is within [2, capacity). Sanitizers (make test with -fsanitize) catch OOB.
void testExtremes() {
  const double bpms[] = {1.0, 10.0, 30.0, 999.0};
  for (double bpm : bpms) {
    for (int32_t d = 0; d < 10; d++) {
      Rig rig(bpm, d, 0.9f, 8000.0f, 1.0f);
      auto out = runChannels(rig.chain, {noise(48000 * 2), noise(48000 * 2, 5)});
      bool ok = true;
      for (const auto& ch : out) {
        for (float v : ch) {
          if (!std::isfinite(v) || std::fabs(v) > 40.0f) ok = false;
        }
      }
      CHECK(ok);
    }
  }
  // 1/2 at 20 BPM would be 6 s > 4 s: folded to 3 s (an even division of the beat).
  Rig fold(20.0, 5, 0.0f, 16000.0f, 1.0f);
  auto out = runChannels(fold.chain, {impulse(48000 * 4)})[0];
  CHECK(peakIndex(out, 1) == 3 * 48000);
}

// Stereo: identical channels in -> identical channels out; a mono block behaves like channel 0 of a
// stereo one; different channels stay independent (no cross-talk).
void testStereo() {
  const auto a = noise(24000, 1);
  const auto b = noise(24000, 2);

  Rig st(120.0, 2, 0.6f, 2000.0f, 0.5f);
  auto same = runChannels(st.chain, {a, a});
  CHECK(same[0] == same[1]);

  Rig mono(120.0, 2, 0.6f, 2000.0f, 0.5f);
  auto m = runChannels(mono.chain, {a});
  CHECK(m[0] == same[0]);

  Rig st2(120.0, 2, 0.6f, 2000.0f, 0.5f);
  auto diff = runChannels(st2.chain, {a, b});
  CHECK(diff[0] == same[0]);  // left unaffected by what the right carries
  CHECK(diff[1] != diff[0]);
}

// A division change glides: the echo of a sine has no step discontinuity (a jump of the read
// pointer would put a click of up to ~2x amplitude between neighbouring samples).
void testTimeChangeIsClickFree() {
  Rig rig(120.0, 1, 0.0f, 16000.0f, 1.0f);
  const int32_t n = 48000 * 3;
  std::vector<float> sine(n);
  for (int32_t i = 0; i < n; i++) sine[i] = 0.5f * std::sin(2.0 * M_PI * 220.0 * i / kRate);
  std::vector<float> out(n);
  const int32_t half = n / 2;
  auto first = runChannels(rig.chain, {std::vector<float>(sine.begin(), sine.begin() + half)})[0];
  rig.chain.setParam(kId, P(NodeParam::DubDelayDivision), 5.0f);  // 1/8 -> 1/2
  auto second = runChannels(rig.chain, {std::vector<float>(sine.begin() + half, sine.end())})[0];
  std::copy(first.begin(), first.end(), out.begin());
  std::copy(second.begin(), second.end(), out.begin() + half);
  double maxStep = 0;
  for (int32_t i = 14000; i < n; i++) maxStep = std::max(maxStep, static_cast<double>(std::fabs(out[i] - out[i - 1])));
  // A 220 Hz sine at 0.5 steps at most 0.5*2*pi*220/48000 = 0.0144 per sample; the glide bends the
  // pitch by up to 40%, so allow for that but nowhere near a discontinuity.
  CHECK(maxStep < 0.03);
}

void testParamHandling() {
  Rig rig(120.0, 2, 0.0f, 16000.0f, 1.0f);
  // Division: rounded and clamped to the table.
  rig.chain.setParam(kId, P(NodeParam::DubDelayDivision), 99.0f);
  CHECK_NEAR(rig.chain.effectiveParam(kId, P(NodeParam::DubDelayDivision)), 9.0, 0.0);
  // Non-finite is ignored; foreign params are rejected by the chain.
  rig.chain.setParam(kId, P(NodeParam::DubDelayFeedback), NAN);
  CHECK(!rig.chain.setParam(kId, P(NodeParam::DelayTime), 0.5f));
  // Self-description: four parameters, division is a non-modulatable step.
  CHECK(fxParamCount(kDubDelay) == 4);
  CHECK(fxParamInfo(kDubDelay, 0)->id == P(NodeParam::DubDelayDivision));
  CHECK(resolveModDomain(*fxParamInfo(kDubDelay, 0)) == ModDomain::None);
  CHECK(resolveModDomain(*fxParamInfo(kDubDelay, 2)) == ModDomain::Log);  // tone, Hz
  CHECK(fxKindForNodeParam(P(NodeParam::DubDelayTone)) == kDubDelay);
}

}  // namespace

int main() {
  testDivisionsAt120();
  testBpmDrivesTime();
  testFeedback();
  testToneIsInTheFeedbackLoop();
  testMix();
  testExtremes();
  testStereo();
  testTimeChangeIsClickFree();
  testParamHandling();
  std::printf("dub_delay_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
