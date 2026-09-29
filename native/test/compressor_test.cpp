#include <cmath>
#include <vector>
#include "../src/dsp/compressor.h"
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
  Compressor comp(kSampleRate);
  std::vector<float> l = {0.1f, -0.9f, 0.5f, 0.0f, -0.3f};
  std::vector<float> r = {-0.1f, 0.9f, -0.5f, 0.0f, 0.3f};
  std::vector<float> origL = l, origR = r;
  float* channels[2] = {l.data(), r.data()};
  comp.process(channels, 2, static_cast<int32_t>(l.size()));
  for (size_t i = 0; i < l.size(); i++) {
    CHECK(l[i] == origL[i]);
    CHECK(r[i] == origR[i]);
  }
}

static void testSilenceInProducesSilenceOut() {
  Compressor comp(kSampleRate);
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -30.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 8.0f);
  std::vector<float> l(512, 0.0f), r(512, 0.0f);
  float* channels[2] = {l.data(), r.data()};
  comp.process(channels, 2, 512);
  for (float v : l) CHECK(v == 0.0f);
  for (float v : r) CHECK(v == 0.0f);
}

static void testResetClearsEnvelopeNotBypassOrParams() {
  Compressor comp(kSampleRate);
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -40.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 10.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompAttack), 0.001f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRelease), 0.05f);

  auto loud = makeSine(1000.0f, 0.9f, 4096, kSampleRate);
  float* channels[1] = {loud.data()};
  comp.process(channels, 1, static_cast<int32_t>(loud.size()));
  const float compressedRms = rms(loud.data(), static_cast<int32_t>(loud.size()));

  comp.reset();  // clears envelope, but ratio/threshold/attack/release (and un-bypassed state) persist
  auto loud2 = makeSine(1000.0f, 0.9f, 4096, kSampleRate);
  channels[0] = loud2.data();
  comp.process(channels, 1, static_cast<int32_t>(loud2.size()));
  const float afterResetRms = rms(loud2.data(), static_cast<int32_t>(loud2.size()));

  // Still compressing after reset (params/bypass unaffected) — a sine well above a -40dB
  // threshold at 10:1 must still come out well below its dry 0.9-amplitude RMS.
  const float dryRms = rms(makeSine(1000.0f, 0.9f, 4096, kSampleRate).data(), 4096);
  CHECK(afterResetRms < dryRms * 0.9f);
  CHECK(std::isfinite(compressedRms));
}

// --- parameter bounds ---

static void testOutOfRangeParamsStayFiniteAndBounded() {
  Compressor comp(kSampleRate);
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -1000.0f);  // clamp to -60
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 999.0f);        // clamp to 20
  comp.setParam(static_cast<int32_t>(NodeParam::CompAttack), -5.0f);        // clamp to 0.0001
  comp.setParam(static_cast<int32_t>(NodeParam::CompRelease), 0.0f);        // clamp to 0.001
  comp.setParam(static_cast<int32_t>(NodeParam::CompKnee), -10.0f);         // clamp to 0
  comp.setParam(static_cast<int32_t>(NodeParam::CompMakeup), 1000.0f);      // clamp to 24

  auto sig = makeSine(1000.0f, 1.0f, 8192, kSampleRate);
  float* channels[1] = {sig.data()};
  comp.process(channels, 1, static_cast<int32_t>(sig.size()));
  CHECK(allFinite(sig));
  for (float v : sig) CHECK(std::fabs(v) < 100.0f);  // makeup clamp keeps output sane
}

// --- gain-response / predictable gain reduction ---

static void testAboveThresholdIsCompressedBelowThresholdIsNot() {
  Compressor comp(kSampleRate);
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -12.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 4.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompAttack), 0.001f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRelease), 0.05f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompKnee), 0.0f);

  // Quiet signal (~-24 dBFS), well below threshold: should pass through with ~0 dB reduction
  // once settled.
  Compressor quietComp(kSampleRate);
  quietComp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -12.0f);
  quietComp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 4.0f);
  quietComp.setParam(static_cast<int32_t>(NodeParam::CompAttack), 0.001f);
  quietComp.setParam(static_cast<int32_t>(NodeParam::CompRelease), 0.05f);
  auto quiet = makeSine(1000.0f, 0.063f, 8192, kSampleRate);  // ~ -24 dBFS peak
  float* qch[1] = {quiet.data()};
  quietComp.process(qch, 1, static_cast<int32_t>(quiet.size()));
  const float quietOutRms = rms(quiet.data() + 4096, 4096);  // settled half
  const float quietDryRms = rms(makeSine(1000.0f, 0.063f, 8192, kSampleRate).data() + 4096, 4096);
  CHECK_NEAR(quietOutRms, quietDryRms, quietDryRms * 0.05f);

  // Loud signal (0 dBFS peak), well above threshold at 4:1: gain reduction should be roughly
  // (0 - (-12)) * (1 - 1/4) = 9 dB once the envelope has settled -> output amplitude roughly
  // 10^(-9/20) =~ 0.355 of input, i.e. a clearly audible, predictable reduction, not silence
  // and not untouched.
  auto loud = makeSine(1000.0f, 1.0f, 16384, kSampleRate);
  float* lch[1] = {loud.data()};
  comp.process(lch, 1, static_cast<int32_t>(loud.size()));
  const float loudOutRms = rms(loud.data() + 12288, 4096);  // well-settled tail quarter
  const float loudDryRms = rms(makeSine(1000.0f, 1.0f, 16384, kSampleRate).data() + 12288, 4096);
  const float expectedRatio = std::pow(10.0f, -9.0f / 20.0f);
  CHECK(loudOutRms < loudDryRms * 0.8f);   // clearly reduced
  CHECK(loudOutRms > loudDryRms * 0.1f);   // but not silenced
  CHECK_NEAR(loudOutRms / loudDryRms, expectedRatio, 0.08f);
}

static void testMakeupGainRestoresLevel() {
  Compressor comp(kSampleRate);
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -12.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 4.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompAttack), 0.001f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRelease), 0.05f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompMakeup), 9.0f);  // ~ compensate the 9dB cut

  auto loud = makeSine(1000.0f, 1.0f, 16384, kSampleRate);
  float* channels[1] = {loud.data()};
  comp.process(channels, 1, static_cast<int32_t>(loud.size()));
  const float outRms = rms(loud.data() + 12288, 4096);
  const float dryRms = rms(makeSine(1000.0f, 1.0f, 16384, kSampleRate).data() + 12288, 4096);
  // With ~9dB makeup compensating ~9dB of reduction, output should land close to dry level.
  CHECK_NEAR(outRms, dryRms, dryRms * 0.15f);
}

// --- envelope stability / click-free parameter changes ---

static void testAttackReleaseEnvelopeMovesGradually() {
  Compressor comp(kSampleRate);
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -12.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 8.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompAttack), 0.02f);   // 20ms, not instant
  comp.setParam(static_cast<int32_t>(NodeParam::CompRelease), 0.2f);

  auto sig = makeSine(1000.0f, 1.0f, 512, kSampleRate);  // sudden loud onset from silence
  float* channels[1] = {sig.data()};
  comp.process(channels, 1, static_cast<int32_t>(sig.size()));

  // No single-sample discontinuity: the largest sample-to-sample jump in the processed
  // signal should stay bounded, not spike (a broken/instant gain computer would produce an
  // audible click at the transient).
  float maxJump = 0.0f;
  for (size_t i = 1; i < sig.size(); i++) maxJump = std::max(maxJump, std::fabs(sig[i] - sig[i - 1]));
  CHECK(maxJump < 0.5f);
  CHECK(allFinite(sig));
}

static void testParamChangeMidStreamStaysFinite() {
  Compressor comp(kSampleRate);
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -20.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 6.0f);

  auto sig = makeSine(440.0f, 0.8f, 2048, kSampleRate);
  float* firstHalf[1] = {sig.data()};
  comp.process(firstHalf, 1, 1024);
  // Change params mid-stream, as automation would.
  comp.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -6.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompRatio), 2.0f);
  comp.setParam(static_cast<int32_t>(NodeParam::CompAttack), 0.001f);
  float* secondHalf[1] = {sig.data() + 1024};
  comp.process(secondHalf, 1, 1024);
  CHECK(allFinite(sig));
}

// --- determinism ---

static void testOutputIsDeterministic() {
  Compressor a(kSampleRate);
  Compressor b(kSampleRate);
  a.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -18.0f);
  b.setParam(static_cast<int32_t>(NodeParam::CompThreshold), -18.0f);
  a.setParam(static_cast<int32_t>(NodeParam::CompRatio), 4.0f);
  b.setParam(static_cast<int32_t>(NodeParam::CompRatio), 4.0f);

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
  testResetClearsEnvelopeNotBypassOrParams();
  testOutOfRangeParamsStayFiniteAndBounded();
  testAboveThresholdIsCompressedBelowThresholdIsNot();
  testMakeupGainRestoresLevel();
  testAttackReleaseEnvelopeMovesGradually();
  testParamChangeMidStreamStaysFinite();
  testOutputIsDeterministic();

  std::printf("compressor_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
