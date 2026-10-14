#include <algorithm>
#include <cmath>
#include <vector>
#include "../src/dsp/biquad_filter.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int32_t kBlock = 128;

void configure(BiquadFilter& f, float cutoff, float q, FilterMode mode, float slope) {
  f.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), slope);
  f.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), q);
  f.setParam(static_cast<int32_t>(VoiceParam::FilterMode), mode == FilterMode::HighPass ? 1.0f : 0.0f);
  f.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), cutoff);
}

// Runs `buf` through the filter (mono) in kBlock-sized chunks.
void run(BiquadFilter& f, std::vector<float>& buf) {
  for (size_t i = 0; i < buf.size(); i += kBlock) {
    float* ch[1] = {buf.data() + i};
    f.process(ch, 1, static_cast<int32_t>(std::min<size_t>(kBlock, buf.size() - i)));
  }
}

// Steady-state gain in dB of a sine at `hz`, measured over the last half of a 1 s run.
double gainDb(float cutoff, float q, FilterMode mode, float slope, double hz) {
  BiquadFilter f(kSampleRate);
  configure(f, cutoff, q, mode, slope);
  const int32_t n = 48000;
  std::vector<float> buf(n);
  for (int32_t i = 0; i < n; i++) buf[i] = static_cast<float>(std::sin(2.0 * M_PI * hz * i / kSampleRate));
  run(f, buf);
  double sum = 0.0;
  for (int32_t i = n / 2; i < n; i++) sum += static_cast<double>(buf[i]) * buf[i];
  const double rms = std::sqrt(sum / (n / 2));
  return 20.0 * std::log10(rms * std::sqrt(2.0));
}

// Reference RBJ biquad (the pre-slope implementation) to pin default behavior.
struct RefBiquad {
  double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  RefBiquad(double fc, double q, bool hp) {
    const double w = 2.0 * M_PI * fc / kSampleRate;
    const double alpha = std::sin(w) / (2.0 * q), c = std::cos(w), a0 = 1.0 + alpha;
    b0 = (hp ? (1 + c) : (1 - c)) / 2.0 / a0;
    b1 = (hp ? -(1 + c) : (1 - c)) / a0;
    b2 = b0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
  }
  float tick(float x) {
    double out = b0 * x + z1;
    z1 = b1 * x + z2 - a1 * out;
    z2 = b2 * x - a2 * out;
    return static_cast<float>(out);
  }
};

void testDefaultMatchesOriginalFilter() {
  for (bool hp : {false, true}) {
    BiquadFilter f(kSampleRate);
    // No slope param set at all: must be the original single biquad.
    f.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), 2.5f);
    f.setParam(static_cast<int32_t>(VoiceParam::FilterMode), hp ? 1.0f : 0.0f);
    f.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), 1500.0f);
    RefBiquad ref(1500.0, 2.5, hp);
    std::vector<float> buf(4096), expect(4096);
    for (size_t i = 0; i < buf.size(); i++) {
      buf[i] = static_cast<float>(std::sin(i * 0.37) * 0.5 + std::sin(i * 0.011) * 0.3);
      expect[i] = ref.tick(buf[i]);
    }
    run(f, buf);
    double maxDiff = 0;
    for (size_t i = 0; i < buf.size(); i++) maxDiff = std::max(maxDiff, (double)std::fabs(buf[i] - expect[i]));
    CHECK(maxDiff == 0.0);
  }
  // Explicit 12 equals default too.
  CHECK_NEAR(gainDb(1000, 0.707f, FilterMode::LowPass, 12, 3000),
             gainDb(1000, 0.707f, FilterMode::LowPass, 12, 3000), 1e-12);
}

void testLowPassSlopes() {
  // Well above cutoff, one octave apart: 12 dB/oct drops ~12 dB, 24 dB/oct ~24 dB.
  const double d12 = gainDb(500, 0.707f, FilterMode::LowPass, 12, 2000) -
                     gainDb(500, 0.707f, FilterMode::LowPass, 12, 4000);
  const double d24 = gainDb(500, 0.707f, FilterMode::LowPass, 24, 2000) -
                     gainDb(500, 0.707f, FilterMode::LowPass, 24, 4000);
  CHECK_NEAR(d12, 12.0, 1.5);
  CHECK_NEAR(d24, 24.0, 2.5);
}

void testHighPassSlopes() {
  const double d12 = gainDb(4000, 0.707f, FilterMode::HighPass, 12, 500) -
                     gainDb(4000, 0.707f, FilterMode::HighPass, 12, 250);
  const double d24 = gainDb(4000, 0.707f, FilterMode::HighPass, 24, 500) -
                     gainDb(4000, 0.707f, FilterMode::HighPass, 24, 250);
  CHECK_NEAR(d12, 12.0, 1.5);
  CHECK_NEAR(d24, 24.0, 2.5);
}

void testCutoffIsMinus3dBInBothModes() {
  for (FilterMode m : {FilterMode::LowPass, FilterMode::HighPass}) {
    for (float slope : {12.0f, 24.0f}) {
      CHECK_NEAR(gainDb(2000, 0.70710678f, m, slope, 2000), -3.01, 0.3);
    }
  }
}

void testResonanceGainAtCutoffMatchesQ() {
  // Resonance q means "gain at cutoff ~= q" in both slopes.
  const double expected = 20.0 * std::log10(4.0);
  for (FilterMode m : {FilterMode::LowPass, FilterMode::HighPass}) {
    CHECK_NEAR(gainDb(2000, 4.0f, m, 12, 2000), expected, 0.5);
    CHECK_NEAR(gainDb(2000, 4.0f, m, 24, 2000), expected, 0.5);
  }
  // Passband stays flat (unity) at default resonance in 24 dB mode.
  CHECK_NEAR(gainDb(8000, 0.70710678f, FilterMode::LowPass, 24, 100), 0.0, 0.1);
  CHECK_NEAR(gainDb(500, 0.70710678f, FilterMode::HighPass, 24, 10000), 0.0, 0.1);
}

void testSlopeSwitchIsClickFreeAndStable() {
  BiquadFilter f(kSampleRate);
  configure(f, 2000, 0.707f, FilterMode::LowPass, 12);
  const int32_t n = 48000;
  std::vector<float> buf(n);
  for (int32_t i = 0; i < n; i++) buf[i] = static_cast<float>(0.8 * std::sin(2.0 * M_PI * 300.0 * i / kSampleRate));
  double maxStep = 0.0, maxAbs = 0.0;
  float prev = 0.0f;
  for (int32_t start = 0; start < n; start += kBlock) {
    // Toggle slope every 25 blocks.
    if ((start / kBlock) % 25 == 0)
      f.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), ((start / kBlock) / 25) % 2 ? 12.0f : 24.0f);
    float* ch[1] = {buf.data() + start};
    f.process(ch, 1, std::min(kBlock, n - start));
    for (int32_t i = start; i < std::min(n, start + kBlock); i++) {
      CHECK(std::isfinite(buf[i]));
      maxStep = std::max(maxStep, (double)std::fabs(buf[i] - prev));
      maxAbs = std::max(maxAbs, (double)std::fabs(buf[i]));
      prev = buf[i];
    }
  }
  // A 300 Hz sine at 0.8 has max sample step ~0.8*2pi*300/48000 = 0.031; allow generous margin
  // but far below what a state-reset step (~0.8) would produce.
  CHECK(maxStep < 0.06);
  CHECK(maxAbs < 1.0);
}

void testExtremeCutoffsStayFinite() {
  for (float cutoff : {0.0f, 1.0f, 20.0f, 24000.0f, 100000.0f}) {
    for (FilterMode m : {FilterMode::LowPass, FilterMode::HighPass}) {
      for (float q : {0.05f, 0.707f, 20.0f}) {
        BiquadFilter f(kSampleRate);
        configure(f, cutoff, q, m, 24);
        std::vector<float> buf(8192);
        for (size_t i = 0; i < buf.size(); i++) buf[i] = (i % 97 < 48) ? 1.0f : -1.0f;
        run(f, buf);
        bool ok = true;
        for (float x : buf) ok = ok && std::isfinite(x) && std::fabs(x) < 1000.0f;
        CHECK(ok);
      }
    }
  }
}

void testResetClearsState() {
  BiquadFilter f(kSampleRate);
  configure(f, 500, 4.0f, FilterMode::LowPass, 24);
  std::vector<float> loud(2048, 1.0f);
  run(f, loud);
  f.reset();
  std::vector<float> silence(256, 0.0f);
  run(f, silence);
  bool zero = true;
  for (float x : silence) zero = zero && x == 0.0f;
  CHECK(zero);

  // After reset, output equals a fresh filter's output for the same input.
  BiquadFilter fresh(kSampleRate);
  configure(fresh, 500, 4.0f, FilterMode::LowPass, 24);
  std::vector<float> a(1024), b(1024);
  for (size_t i = 0; i < a.size(); i++) a[i] = b[i] = static_cast<float>(std::sin(i * 0.05));
  run(f, a);
  run(fresh, b);
  CHECK(a == b);
}

void testSlopeAloneDoesNotEngageFilter() {
  BiquadFilter f(kSampleRate);
  f.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 24.0f);
  std::vector<float> buf(256), orig(256);
  for (size_t i = 0; i < buf.size(); i++) buf[i] = orig[i] = static_cast<float>(std::sin(i * 0.9));
  run(f, buf);
  CHECK(buf == orig);
}

void testNonStandardSlopeValuesSnap() {
  const double g24 = gainDb(1000, 0.707f, FilterMode::LowPass, 24, 4000);
  CHECK_NEAR(gainDb(1000, 0.707f, FilterMode::LowPass, 30, 4000), g24, 1e-9);
  const double g12 = gainDb(1000, 0.707f, FilterMode::LowPass, 12, 4000);
  CHECK_NEAR(gainDb(1000, 0.707f, FilterMode::LowPass, 0, 4000), g12, 1e-9);
}

}  // namespace

int main() {
  testDefaultMatchesOriginalFilter();
  testLowPassSlopes();
  testHighPassSlopes();
  testCutoffIsMinus3dBInBothModes();
  testResonanceGainAtCutoffMatchesQ();
  testSlopeSwitchIsClickFreeAndStable();
  testExtremeCutoffsStayFinite();
  testResetClearsState();
  testSlopeAloneDoesNotEngageFilter();
  testNonStandardSlopeValuesSnap();
  std::printf("filter_slope_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
