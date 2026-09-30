#include <algorithm>
#include <cmath>
#include <vector>
#include "../src/dsp/saturation.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr double kSampleRate = 48000.0;

bool allFinite(const std::vector<float>& v) {
  for (float x : v) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}

float rms(const float* data, int32_t n) {
  double sum = 0.0;
  for (int32_t i = 0; i < n; i++) sum += static_cast<double>(data[i]) * data[i];
  return static_cast<float>(std::sqrt(sum / n));
}

float mean(const float* data, int32_t n) {
  double sum = 0.0;
  for (int32_t i = 0; i < n; i++) sum += data[i];
  return static_cast<float>(sum / n);
}

std::vector<float> makeSine(float freqHz, float amplitude, int32_t numFrames, double sampleRate) {
  std::vector<float> data(numFrames);
  for (int32_t i = 0; i < numFrames; i++) {
    data[i] = amplitude * std::sin(2.0 * M_PI * freqHz * i / sampleRate);
  }
  return data;
}

}  // namespace

// --- lifecycle / bypass ---

static void testBypassedByDefaultIsExactPassthrough() {
  Saturation sat(kSampleRate);
  std::vector<float> l = {0.1f, -0.9f, 0.5f, 0.0f, -0.3f};
  std::vector<float> r = {-0.1f, 0.9f, -0.5f, 0.0f, 0.3f};
  std::vector<float> origL = l, origR = r;
  float* channels[2] = {l.data(), r.data()};
  sat.process(channels, 2, static_cast<int32_t>(l.size()));
  for (size_t i = 0; i < l.size(); i++) {
    CHECK(l[i] == origL[i]);
    CHECK(r[i] == origR[i]);
  }
}

static void testSilenceInProducesSilenceOut() {
  Saturation sat(kSampleRate);
  sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), 20.0f);
  // Asymmetry left at 0: a symmetric curve fed exact silence has no DC bias to introduce,
  // so this is the one case silence-in must produce exact silence-out (with asymmetry != 0,
  // the biased curve does introduce a DC offset even for silence — see
  // testAsymmetryStaysNearZeroMeanAfterDcBlocker for how that's handled instead).
  std::vector<float> l(512, 0.0f), r(512, 0.0f);
  float* channels[2] = {l.data(), r.data()};
  sat.process(channels, 2, 512);
  // tanh(0) == 0, and the DC blocker starts at rest, so true digital silence must stay
  // exactly silent — no self-generated noise floor.
  for (float v : l) CHECK(v == 0.0f);
  for (float v : r) CHECK(v == 0.0f);
}

static void testResetClearsDcBlockerNotMixOrParams() {
  Saturation sat(kSampleRate);
  sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), 24.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatAsymmetry), 0.8f);

  auto loud = makeSine(1000.0f, 0.9f, 4096, kSampleRate);
  float* channels[1] = {loud.data()};
  sat.process(channels, 1, static_cast<int32_t>(loud.size()));

  sat.reset();  // clears DC-blocker state, but mix/drive/asymmetry persist
  auto loud2 = makeSine(1000.0f, 0.9f, 4096, kSampleRate);
  channels[0] = loud2.data();
  sat.process(channels, 1, static_cast<int32_t>(loud2.size()));

  // Still saturating after reset (params unaffected): a heavily driven sine is pushed
  // toward a near-square wave, whose RMS/peak ratio (~1.0) is *higher* than an unclipped
  // sine's (0.707) — so the signature of active saturation here is RMS measurably *above*
  // the dry sine's RMS, not below (unlike a compressor, which reduces RMS).
  const float outRms = rms(loud2.data(), static_cast<int32_t>(loud2.size()));
  const float dryRms = 0.9f * 0.70710678f;
  CHECK(outRms > dryRms * 1.2f);
  CHECK(allFinite(loud2));
}

// --- parameter bounds ---

static void testOutOfRangeParamsStayFiniteAndBounded() {
  Saturation sat(kSampleRate);
  sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), -1000.0f);       // clamp to 0
  sat.setParam(static_cast<int32_t>(NodeParam::SatAsymmetry), 999.0f);     // clamp to 1
  sat.setParam(static_cast<int32_t>(NodeParam::SatOutputGain), 1000.0f);   // clamp to 24
  sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 5.0f);             // clamp to 1

  auto sig = makeSine(1000.0f, 1.0f, 8192, kSampleRate);
  float* channels[1] = {sig.data()};
  sat.process(channels, 1, static_cast<int32_t>(sig.size()));
  CHECK(allFinite(sig));
  // tanh output is bounded in (-1, 1); +24dB output gain (~15.85x) is the only thing that
  // can push it further, so a generous but real ceiling still catches a broken clamp.
  for (float v : sig) CHECK(std::fabs(v) < 20.0f);
}

// --- bounds / determinism on the shaper itself ---

static void testOutputStaysWithinUnityRegardlessOfDrive() {
  Saturation sat(kSampleRate);
  sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), 40.0f);  // max drive, hard clipping
  auto sig = makeSine(200.0f, 1.0f, 4096, kSampleRate);
  float* channels[1] = {sig.data()};
  sat.process(channels, 1, static_cast<int32_t>(sig.size()));
  CHECK(allFinite(sig));
  // With 0dB output gain (the default), tanh's bounded range means output amplitude can
  // never exceed dry, however hard the drive: a runaway shaper would fail this.
  for (float v : sig) CHECK(std::fabs(v) <= 1.0001f);
}

static void testDriveIncreasesHarmonicSaturation() {
  // A harder-driven sine should look measurably more "clipped" — its RMS should approach
  // the RMS of a hard square wave (dry RMS * ~1.11 relative headroom aside, tanh saturation
  // pushes a sine's RMS up toward, not down from, its unclipped 0.707*peak ratio) faster
  // than a lightly driven one, i.e. higher-drive RMS/peak ratio should exceed lower-drive's.
  auto runDrive = [](float driveDb) {
    Saturation sat(kSampleRate);
    sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
    sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), driveDb);
    auto sig = makeSine(300.0f, 0.3f, 4096, kSampleRate);
    float* channels[1] = {sig.data()};
    sat.process(channels, 1, static_cast<int32_t>(sig.size()));
    return rms(sig.data(), static_cast<int32_t>(sig.size()));
  };
  const float lightRms = runDrive(0.0f);
  const float heavyRms = runDrive(30.0f);
  // A near-square wave's RMS/peak ratio approaches 1.0, vs. a sine's 0.707 — heavy drive
  // should measurably push RMS up relative to light drive at the same input amplitude.
  CHECK(heavyRms > lightRms);
}

static void testOutputGainScalesLevel() {
  Saturation quiet(kSampleRate), loud(kSampleRate);
  for (Saturation* s : {&quiet, &loud}) {
    s->setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
    s->setParam(static_cast<int32_t>(NodeParam::SatDrive), 6.0f);
  }
  loud.setParam(static_cast<int32_t>(NodeParam::SatOutputGain), 12.0f);

  auto sigA = makeSine(300.0f, 0.3f, 4096, kSampleRate);
  auto sigB = sigA;
  float* chA[1] = {sigA.data()};
  float* chB[1] = {sigB.data()};
  quiet.process(chA, 1, static_cast<int32_t>(sigA.size()));
  loud.process(chB, 1, static_cast<int32_t>(sigB.size()));

  const float quietRms = rms(sigA.data(), static_cast<int32_t>(sigA.size()));
  const float loudRms = rms(sigB.data(), static_cast<int32_t>(sigB.size()));
  const float expectedRatio = std::pow(10.0f, 12.0f / 20.0f);
  CHECK_NEAR(loudRms / quietRms, expectedRatio, expectedRatio * 0.05f);
}

static void testMixBlendsDryAndWet() {
  Saturation dry(kSampleRate), half(kSampleRate), wet(kSampleRate);
  for (Saturation* s : {&dry, &half, &wet}) {
    s->setParam(static_cast<int32_t>(NodeParam::SatDrive), 30.0f);
  }
  dry.setParam(static_cast<int32_t>(NodeParam::SatMix), 0.0f);  // stays fully bypassed
  half.setParam(static_cast<int32_t>(NodeParam::SatMix), 0.5f);
  wet.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);

  auto sigDry = makeSine(300.0f, 0.9f, 2048, kSampleRate);
  auto sigHalf = sigDry;
  auto sigWet = sigDry;
  float* chDry[1] = {sigDry.data()};
  float* chHalf[1] = {sigHalf.data()};
  float* chWet[1] = {sigWet.data()};
  dry.process(chDry, 1, static_cast<int32_t>(sigDry.size()));
  half.process(chHalf, 1, static_cast<int32_t>(sigHalf.size()));
  wet.process(chWet, 1, static_cast<int32_t>(sigWet.size()));

  const float dryRms = rms(sigDry.data(), static_cast<int32_t>(sigDry.size()));
  const float halfRms = rms(sigHalf.data(), static_cast<int32_t>(sigHalf.size()));
  const float wetRms = rms(sigWet.data(), static_cast<int32_t>(sigWet.size()));
  const float origDryRms = rms(makeSine(300.0f, 0.9f, 2048, kSampleRate).data(), 2048);

  CHECK_NEAR(dryRms, origDryRms, origDryRms * 0.001f);  // mix=0 is an exact passthrough
  // Heavy drive (30dB) measurably changes RMS vs. dry, and a half-wet blend should sit
  // strictly between the dry and fully-wet extremes, not equal either one.
  CHECK(std::fabs(wetRms - dryRms) > 0.01f);
  const float lo = std::min(dryRms, wetRms);
  const float hi = std::max(dryRms, wetRms);
  CHECK(halfRms > lo && halfRms < hi);
}

// --- asymmetry / DC handling ---

static void testSymmetricDriveProducesNoDcOffset() {
  Saturation sat(kSampleRate);
  sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), 20.0f);  // asymmetry left at 0
  auto sig = makeSine(300.0f, 0.9f, 8192, kSampleRate);
  float* channels[1] = {sig.data()};
  sat.process(channels, 1, static_cast<int32_t>(sig.size()));
  // Settled half only, to skip the DC blocker's own brief settling transient.
  CHECK_NEAR(mean(sig.data() + 4096, 4096), 0.0f, 0.01f);
}

static void testAsymmetryStaysNearZeroMeanAfterDcBlocker() {
  Saturation sat(kSampleRate);
  sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), 20.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatAsymmetry), 1.0f);  // maximum bias
  auto sig = makeSine(300.0f, 0.9f, 16384, kSampleRate);
  float* channels[1] = {sig.data()};
  sat.process(channels, 1, static_cast<int32_t>(sig.size()));
  CHECK(allFinite(sig));
  // A biased tanh curve fed a symmetric sine is heavily asymmetric *before* the DC
  // blocker — with no correction at all, a silent (x=0) input at this asymmetry would sit
  // at tanh(1.5) =~ 0.905, i.e. a huge fraction of full scale. After the DC blocker, the
  // settled-window mean must be pulled back much closer to zero than that (not necessarily
  // exactly zero: heavy drive keeps re-saturating the signal every sample, so the blocker's
  // state-clamped tracking — see saturation.h's class comment — only approximates true
  // zero-mean here, not eliminates the offset outright). This is what "DC handling" per the
  // ticket means and is worth testing explicitly, not just asserting finiteness.
  CHECK_NEAR(mean(sig.data() + 12288, 4096), 0.0f, 0.15f);
}

static void testAsymmetryChangesWaveshapeVsSymmetric() {
  Saturation symmetric(kSampleRate), asymmetric(kSampleRate);
  for (Saturation* s : {&symmetric, &asymmetric}) {
    s->setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
    s->setParam(static_cast<int32_t>(NodeParam::SatDrive), 20.0f);
  }
  asymmetric.setParam(static_cast<int32_t>(NodeParam::SatAsymmetry), 1.0f);

  auto sigA = makeSine(300.0f, 0.9f, 2048, kSampleRate);
  auto sigB = sigA;
  float* chA[1] = {sigA.data()};
  float* chB[1] = {sigB.data()};
  symmetric.process(chA, 1, static_cast<int32_t>(sigA.size()));
  asymmetric.process(chB, 1, static_cast<int32_t>(sigB.size()));

  // The two params must produce audibly different output, not the same curve regardless of
  // the asymmetry setting.
  bool anyDifferent = false;
  for (size_t i = 0; i < sigA.size(); i++) {
    if (std::fabs(sigA[i] - sigB[i]) > 1e-4f) anyDifferent = true;
  }
  CHECK(anyDifferent);
}

// --- channel behavior ---

static void testChannelsAreIndependent() {
  Saturation sat(kSampleRate);
  sat.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  sat.setParam(static_cast<int32_t>(NodeParam::SatDrive), 20.0f);

  auto loud = makeSine(300.0f, 0.9f, 2048, kSampleRate);
  std::vector<float> silent(2048, 0.0f);
  float* channels[2] = {loud.data(), silent.data()};
  sat.process(channels, 2, static_cast<int32_t>(loud.size()));

  for (float v : silent) CHECK(v == 0.0f);  // a silent channel must stay silent regardless
  CHECK(allFinite(loud));
  const float loudRms = rms(loud.data(), static_cast<int32_t>(loud.size()));
  CHECK(loudRms > 0.0f);
}

// --- determinism ---

static void testOutputIsDeterministic() {
  Saturation a(kSampleRate);
  Saturation b(kSampleRate);
  a.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  b.setParam(static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  a.setParam(static_cast<int32_t>(NodeParam::SatDrive), 18.0f);
  b.setParam(static_cast<int32_t>(NodeParam::SatDrive), 18.0f);
  a.setParam(static_cast<int32_t>(NodeParam::SatAsymmetry), 0.4f);
  b.setParam(static_cast<int32_t>(NodeParam::SatAsymmetry), 0.4f);

  auto sigA = makeSine(300.0f, 0.7f, 4096, kSampleRate);
  auto sigB = makeSine(300.0f, 0.7f, 4096, kSampleRate);
  float* chA[1] = {sigA.data()};
  float* chB[1] = {sigB.data()};
  a.process(chA, 1, static_cast<int32_t>(sigA.size()));
  b.process(chB, 1, static_cast<int32_t>(sigB.size()));
  for (size_t i = 0; i < sigA.size(); i++) CHECK(sigA[i] == sigB[i]);
}

int main() {
  testBypassedByDefaultIsExactPassthrough();
  testSilenceInProducesSilenceOut();
  testResetClearsDcBlockerNotMixOrParams();
  testOutOfRangeParamsStayFiniteAndBounded();
  testOutputStaysWithinUnityRegardlessOfDrive();
  testDriveIncreasesHarmonicSaturation();
  testOutputGainScalesLevel();
  testMixBlendsDryAndWet();
  testSymmetricDriveProducesNoDcOffset();
  testAsymmetryStaysNearZeroMeanAfterDcBlocker();
  testAsymmetryChangesWaveshapeVsSymmetric();
  testChannelsAreIndependent();
  testOutputIsDeterministic();

  std::printf("saturation_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
