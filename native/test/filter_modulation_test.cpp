#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>
#include "../src/dsp/adsr_envelope.h"
#include "../src/dsp/filter_modulation.h"
#include "../src/dsp/rhythm.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr double kSr = 48000.0;
constexpr int32_t kBlock = 16;  // the instruments' control-rate sub-block

FilterModulation make(double sr = kSr) {
  FilterModulation m;
  m.configure(sr);
  return m;
}

BiquadFilter makeFilter(double sr, float q) {
  BiquadFilter f(sr);
  f.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 24.0f);
  f.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), q);
  return f;
}

// Runs a noise-free saw-ish input through a filter driven once per block by `signal(block)`.
std::vector<float> render(double sr, float q, float base, float depth, int32_t blocks,
                          const std::function<float(int32_t)>& signal) {
  FilterModulation m = make(sr);
  BiquadFilter f = makeFilter(sr, q);
  std::vector<float> out(static_cast<size_t>(blocks) * kBlock);
  double ph = 0.0;
  for (int32_t b = 0; b < blocks; b++) {
    m.apply(f, base, depth, signal(b));
    for (int32_t k = 0; k < kBlock; k++) {
      ph += 110.0 / sr;
      ph -= std::floor(ph);
      out[static_cast<size_t>(b) * kBlock + k] = static_cast<float>(0.5 * (2.0 * ph - 1.0));
    }
    float* ch[1] = {out.data() + static_cast<size_t>(b) * kBlock};
    f.process(ch, 1, kBlock);
  }
  return out;
}

void testNoModulationIsBase() {
  const FilterModulation m = make();
  for (float base : {40.0f, 400.0f, 4000.0f}) {
    CHECK(m.cutoffHz(base, 0.0f, 1.0f) == base);   // zero depth
    CHECK(m.cutoffHz(base, 3.0f, 0.0f) == base);   // zero signal
  }
  // Applying at depth 0 is the same as setting the cutoff directly: identical samples.
  BiquadFilter direct = makeFilter(kSr, 4.0f);
  direct.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), 700.0f);
  const auto viaMod = render(kSr, 4.0f, 700.0f, 0.0f, 400, [](int32_t) { return 1.0f; });
  std::vector<float> ref(viaMod.size());
  double ph = 0.0;
  for (auto& s : ref) { ph += 110.0 / kSr; ph -= std::floor(ph); s = static_cast<float>(0.5 * (2.0 * ph - 1.0)); }
  float* ch[1] = {ref.data()};
  direct.process(ch, 1, static_cast<int32_t>(ref.size()));
  bool same = true;
  for (size_t i = 0; i < ref.size(); i++) same = same && ref[i] == viaMod[i];
  CHECK(same);
}

void testDirectionAndOctaves() {
  const FilterModulation m = make();
  CHECK_NEAR(m.cutoffHz(800.0f, 1.0f, 1.0f), 1600.0, 0.01);    // +1 octave
  CHECK_NEAR(m.cutoffHz(800.0f, 1.0f, -1.0f), 400.0, 0.01);    // -1 octave
  CHECK_NEAR(m.cutoffHz(800.0f, 2.0f, 0.5f), 1600.0, 0.01);    // depth x signal
  CHECK(m.cutoffHz(800.0f, 2.0f, 0.3f) > 800.0f);
  CHECK(m.cutoffHz(800.0f, 2.0f, -0.3f) < 800.0f);
}

void testDepthScalesMovementNotBase() {
  const FilterModulation m = make();
  double prev = 0.0;
  for (float depth : {0.5f, 1.0f, 2.0f, 3.0f}) {
    const double oct = std::log2(m.cutoffHz(500.0f, depth, 1.0f) / 500.0);
    CHECK_NEAR(oct, depth, 1e-4);   // movement is exactly depth octaves
    CHECK(oct > prev);
    prev = oct;
    CHECK(m.cutoffHz(500.0f, depth, 0.0f) == 500.0f);  // base unchanged at rest
  }
}

void testBoundaries() {
  for (double sr : {8000.0, 22050.0, 44100.0, 48000.0, 96000.0}) {
    const FilterModulation m = make(sr);
    const float hi = static_cast<float>(std::min(18000.0, sr * 0.45));
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (float base : {1.0f, 30.0f, 400.0f, 4000.0f, 17000.0f, 1e6f}) {
      for (float depth : {0.0f, 1.0f, 4.0f, 100.0f, inf, nan}) {
        for (float sig : {-1000.0f, -1.0f, 0.0f, 1.0f, 1000.0f, nan}) {
          const float hz = m.cutoffHz(base, depth, sig);
          CHECK(std::isfinite(hz));
          CHECK(hz >= FilterModulation::kMinHz);
          CHECK(hz <= hi);
          CHECK(hz < sr * 0.5);  // always below Nyquist
        }
      }
    }
    // Negative modulation bottoms out at the minimum; positive tops out below Nyquist.
    CHECK(m.cutoffHz(100.0f, 10.0f, -1.0f) == FilterModulation::kMinHz);
    CHECK(m.cutoffHz(1000.0f, 10.0f, 1.0f) == hi);
    // The instrument floor lifts a closed filter, but never past the ceiling.
    CHECK(m.cutoffHz(100.0f, 10.0f, -1.0f, 50.0f) == 50.0f);
    CHECK(m.cutoffHz(100.0f, 0.0f, 0.0f, 1e9f) == hi);
  }
}

void testStableAtExtremes() {
  // Slam the signal between full open and full closed every block at high Q and depth, at a low
  // and a high sample rate: output stays finite and bounded.
  for (double sr : {8000.0, 48000.0, 96000.0}) {
    const auto out = render(sr, 9.7f, 200.0f, 8.0f, 20000, [](int32_t b) { return (b & 1) ? 1.0f : -1.0f; });
    bool ok = true;  // bounded by resonance gain, not diverging
    for (float v : out) ok = ok && std::isfinite(v) && std::fabs(v) < 200.0f;
    CHECK(ok);
  }
}

void testAbruptStepsAreRateLimited() {
  FilterModulation m = make();
  BiquadFilter f = makeFilter(kSr, 1.0f);
  const float first = m.apply(f, 200.0f, 4.0f, -1.0f);  // first call jumps straight to target
  CHECK_NEAR(first, FilterModulation::kMinHz, 0.01);  // 12.5 Hz target, clamped to the minimum
  float prev = first;
  for (int i = 0; i < 40; i++) {                        // then a hard step to +4 octaves
    const float hz = m.apply(f, 200.0f, 4.0f, 1.0f);
    const double moved = std::log2(hz / prev);
    CHECK(moved <= FilterModulation::kMaxOctavesPerApply + 1e-3);
    if (std::log2(3200.0f / prev) > FilterModulation::kMaxOctavesPerApply) {
      CHECK_NEAR(moved, FilterModulation::kMaxOctavesPerApply, 1e-3);  // still travelling: at the limit
    }
    prev = hz;
    if (prev >= 3200.0f) break;
  }
  CHECK(prev >= 3200.0f * 0.999f);                       // ...and arrives at the target
  // A smooth signal is never altered by the limit.
  FilterModulation m2 = make();
  BiquadFilter f2 = makeFilter(kSr, 1.0f);
  bool untouched = true;
  for (int b = 0; b < 2000; b++) {
    const float s = 0.5f + 0.5f * std::sin(2.0f * 3.14159265f * b / 400.0f);
    const float hz = m2.apply(f2, 500.0f, 3.0f, s);
    const float want = m2.cutoffHz(500.0f, 3.0f, s);
    untouched = untouched && std::fabs(hz - want) <= 0.002f * want * 2.0f;
  }
  CHECK(untouched);
}

void testSmoothSweepNoZipper() {
  // A slow full sweep must not produce steps bigger than the signal itself moves per sample.
  const auto out = render(kSr, 2.0f, 500.0f, 3.0f, 3000, [](int32_t b) {
    return std::sin(2.0f * 3.14159265f * static_cast<float>(b) / 1500.0f);
  });
  float worst = 0.0f;
  for (size_t i = 1000; i < out.size(); i++) worst = std::max(worst, std::fabs(out[i] - out[i - 1]));
  CHECK(worst < 0.3f);  // the 110 Hz saw's own reset is 0.5; a zipper would add to it
}

void testHysteresisOnlyWritesOnChange() {
  // A constant signal writes once; repeated applies with the same result don't touch the filter
  // (observable as identical output to a filter that was set once).
  FilterModulation m = make();
  BiquadFilter f = makeFilter(kSr, 1.0f);
  const float a = m.apply(f, 600.0f, 2.0f, 0.5f);
  const float b = m.apply(f, 600.0f, 2.0f, 0.5f);
  CHECK(a == b);
  m.reset();  // forgets the last write: next apply writes again, same value
  CHECK(m.apply(f, 600.0f, 2.0f, 0.5f) == a);
}

void testSourceIndependence() {
  // The mechanism sees only a number. The same signal values from three different producers give
  // the same cutoffs; and any producer yields a valid cutoff.
  const FilterModulation m = make();
  AdsrEnvelope env;
  env.configure(kSr);
  env.noteOn(AdsrParams{0.01f, 0.2f, 0.3f, 0.1f});
  const double period = 24000.0;
  std::vector<float> adsr, repeater, slow;
  for (int32_t b = 0; b < 600; b++) {
    for (int32_t k = 0; k < kBlock; k++) env.next();
    adsr.push_back(env.level());
    repeater.push_back(rhythm::repeatContour(static_cast<float>(rhythm::phaseAt(b * kBlock, 0.0, period))));
    slow.push_back(0.5f + 0.5f * std::sin(2.0f * 3.14159265f * b / 600.0f));
  }
  for (const auto* src : {&adsr, &repeater, &slow}) {
    float lo = 1e9f, hi = 0.0f;
    for (float s : *src) {
      const float hz = m.cutoffHz(500.0f, 2.0f, s);
      CHECK(hz >= 500.0f - 1e-3f);                        // unipolar sources only open
      CHECK_NEAR(std::log2(hz / 500.0f), 2.0f * std::clamp(s, 0.0f, 1.0f), 1e-3);
      lo = std::min(lo, hz);
      hi = std::max(hi, hz);
    }
    CHECK(hi > lo * 1.5f);  // each source actually moves the cutoff
  }
}

}  // namespace

int main() {
  testNoModulationIsBase();
  testDirectionAndOctaves();
  testDepthScalesMovementNotBase();
  testBoundaries();
  testStableAtExtremes();
  testAbruptStepsAreRateLimited();
  testSmoothSweepNoZipper();
  testHysteresisOnlyWritesOnChange();
  testSourceIndependence();
  std::printf("filter_modulation_test: %d checks, %d failures\n", webdsp::test::g_count,
              webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
