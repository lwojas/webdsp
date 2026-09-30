#include <algorithm>
#include <cmath>
#include <vector>
#include "../src/dsp/chorus_flanger.h"
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

float rms(const std::vector<float>& v) {
  double sum = 0.0;
  for (float x : v) sum += static_cast<double>(x) * x;
  return static_cast<float>(std::sqrt(sum / v.size()));
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
  ChorusFlanger cf(kSampleRate);
  std::vector<float> l = {0.1f, -0.9f, 0.5f, 0.0f, -0.3f};
  std::vector<float> r = {-0.1f, 0.9f, -0.5f, 0.0f, 0.3f};
  std::vector<float> origL = l, origR = r;
  float* channels[2] = {l.data(), r.data()};
  cf.process(channels, 2, static_cast<int32_t>(l.size()));
  for (size_t i = 0; i < l.size(); i++) {
    CHECK(l[i] == origL[i]);
    CHECK(r[i] == origR[i]);
  }
}

static void testSilenceInProducesSilenceOut() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.5f);
  std::vector<float> l(2048, 0.0f);
  float* channels[1] = {l.data()};
  cf.process(channels, 1, static_cast<int32_t>(l.size()));
  for (float v : l) CHECK(v == 0.0f);
}

static void testResetClearsRingBufferNotParams() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.7f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 5.0f);

  // Push a loud tone through so the ring buffer + feedback path hold real energy.
  auto loud = makeSine(300.0f, 0.9f, 4096, kSampleRate);
  float* channels[1] = {loud.data()};
  cf.process(channels, 1, static_cast<int32_t>(loud.size()));

  cf.reset();  // clears ring buffer + LFO phase, but mix/feedback/delay persist

  // Silence in, right after reset: if the ring buffer (or feedback loop) weren't actually
  // cleared, leftover energy would leak into this block as nonzero output.
  std::vector<float> silent(2048, 0.0f);
  float* silentChannels[1] = {silent.data()};
  cf.process(silentChannels, 1, static_cast<int32_t>(silent.size()));
  for (float v : silent) CHECK(v == 0.0f);
}

// --- parameter bounds ---

static void testOutOfRangeParamsStayFiniteAndBounded() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerRate), -100.0f);       // clamp to 0.01
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 9999.0f);      // clamp to 20ms
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), -50.0f);       // clamp to 0.1ms
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 5.0f);      // clamp to 0.95
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerStereoPhase), -3.0f);  // clamp to 0
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 5.0f);           // clamp to 1

  auto sig = makeSine(300.0f, 1.0f, 16384, kSampleRate);
  float* channels[1] = {sig.data()};
  cf.process(channels, 1, static_cast<int32_t>(sig.size()));
  CHECK(allFinite(sig));
  // Depth (20ms) clamped well above delay (0.1ms) is exactly the "reads into the future"
  // corner the min-delay-frames floor exists for (see chorus_flanger.h's class comment) —
  // this is the case that would misbehave first if that floor were missing.
  for (float v : sig) CHECK(std::fabs(v) < 20.0f);
}

static void testDelayDepthExceedsCenterDelayStaysFiniteAndBounded() {
  // Depth larger than the center delay is a valid, expected configuration (a shallow flanger
  // with a big sweep), not just an out-of-range accident — worth its own test independent of
  // testOutOfRangeParamsStayFiniteAndBounded's extreme values.
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 1.0f);    // 1ms center
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 20.0f);   // 20ms depth
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.9f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerRate), 5.0f);

  auto sig = makeSine(300.0f, 0.9f, 16384, kSampleRate);
  float* channels[1] = {sig.data()};
  cf.process(channels, 1, static_cast<int32_t>(sig.size()));
  CHECK(allFinite(sig));
  for (float v : sig) CHECK(std::fabs(v) < 20.0f);
}

// --- static-delay correctness (depth=0 reduces to a plain delay tap) ---

static void testStaticDelayMatchesExpectedLatencyWhenDepthIsZero() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 0.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 2.0f);  // exactly 96 frames @ 48kHz

  std::vector<float> sig(300, 0.0f);
  sig[0] = 1.0f;
  float* channels[1] = {sig.data()};
  cf.process(channels, 1, static_cast<int32_t>(sig.size()));

  // With depth=0 (no LFO excursion) and an exact-integer-frame delay, the wet tap is a plain
  // undelayed-interpolation copy of the dry impulse, mix=1 makes the output all-wet: a single
  // sharp copy of the impulse 96 frames later, not spread by interpolation (frac == 0 exactly).
  for (int32_t i = 0; i < 300; i++) {
    if (i == 96) {
      CHECK_NEAR(sig[i], 1.0, 1e-4);
    } else {
      CHECK_NEAR(sig[i], 0.0, 1e-4);
    }
  }
}

static void testFeedbackProducesRepeatedDecayingTaps() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 0.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.5f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 1.0f);  // 48 frames @ 48kHz

  std::vector<float> sig(300, 0.0f);
  sig[0] = 1.0f;
  float* channels[1] = {sig.data()};
  cf.process(channels, 1, static_cast<int32_t>(sig.size()));

  CHECK(allFinite(sig));
  CHECK_NEAR(sig[48], 1.0, 1e-4);    // first repeat: full impulse
  CHECK_NEAR(sig[96], 0.5, 1e-4);    // second repeat: scaled by feedback
  CHECK_NEAR(sig[144], 0.25, 1e-4);  // third repeat: scaled by feedback^2
}

// --- modulation actually modulates ---

static void testDepthChangesOutputVsStaticDelay() {
  ChorusFlanger modulated(kSampleRate), staticDelay(kSampleRate);
  for (ChorusFlanger* cf : {&modulated, &staticDelay}) {
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 15.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerRate), 2.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.0f);
  }
  modulated.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 5.0f);
  staticDelay.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 0.0f);

  auto sigA = makeSine(300.0f, 0.9f, 8192, kSampleRate);
  auto sigB = sigA;
  float* chA[1] = {sigA.data()};
  float* chB[1] = {sigB.data()};
  modulated.process(chA, 1, static_cast<int32_t>(sigA.size()));
  staticDelay.process(chB, 1, static_cast<int32_t>(sigB.size()));

  bool anyDifferent = false;
  for (size_t i = 0; i < sigA.size(); i++) {
    if (std::fabs(sigA[i] - sigB[i]) > 1e-3f) anyDifferent = true;
  }
  CHECK(anyDifferent);
  CHECK(allFinite(sigA));
}

static void testRateChangesOutput() {
  ChorusFlanger slow(kSampleRate), fast(kSampleRate);
  for (ChorusFlanger* cf : {&slow, &fast}) {
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 15.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 5.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.0f);
  }
  slow.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerRate), 0.2f);
  fast.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerRate), 6.0f);

  auto sigA = makeSine(300.0f, 0.9f, 8192, kSampleRate);
  auto sigB = sigA;
  float* chA[1] = {sigA.data()};
  float* chB[1] = {sigB.data()};
  slow.process(chA, 1, static_cast<int32_t>(sigA.size()));
  fast.process(chB, 1, static_cast<int32_t>(sigB.size()));

  bool anyDifferent = false;
  for (size_t i = 0; i < sigA.size(); i++) {
    if (std::fabs(sigA[i] - sigB[i]) > 1e-3f) anyDifferent = true;
  }
  CHECK(anyDifferent);
}

// --- stereo phase / channel behavior ---

static void testZeroStereoPhaseKeepsChannelsIdentical() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 15.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 5.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerStereoPhase), 0.0f);

  auto sig = makeSine(300.0f, 0.9f, 4096, kSampleRate);
  auto l = sig, r = sig;
  float* channels[2] = {l.data(), r.data()};
  cf.process(channels, 2, static_cast<int32_t>(sig.size()));
  for (size_t i = 0; i < l.size(); i++) CHECK(l[i] == r[i]);
}

static void testNonzeroStereoPhaseWidensChannels() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 15.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 5.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerStereoPhase), 0.25f);

  auto sig = makeSine(300.0f, 0.9f, 4096, kSampleRate);
  auto l = sig, r = sig;
  float* channels[2] = {l.data(), r.data()};
  cf.process(channels, 2, static_cast<int32_t>(sig.size()));

  bool anyDifferent = false;
  for (size_t i = 0; i < l.size(); i++) {
    if (std::fabs(l[i] - r[i]) > 1e-3f) anyDifferent = true;
  }
  CHECK(anyDifferent);
  CHECK(allFinite(l));
  CHECK(allFinite(r));
}

static void testChannelsAreIndependent() {
  ChorusFlanger cf(kSampleRate);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 5.0f);
  cf.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.5f);

  auto loud = makeSine(300.0f, 0.9f, 2048, kSampleRate);
  std::vector<float> silent(2048, 0.0f);
  float* channels[2] = {loud.data(), silent.data()};
  cf.process(channels, 2, static_cast<int32_t>(loud.size()));

  for (float v : silent) CHECK(v == 0.0f);  // a silent channel must stay silent regardless
  CHECK(allFinite(loud));
}

// --- mix ---

static void testMixBlendsDryAndWet() {
  ChorusFlanger dry(kSampleRate), half(kSampleRate), wet(kSampleRate);
  for (ChorusFlanger* cf : {&dry, &half, &wet}) {
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 15.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 5.0f);
  }
  dry.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 0.0f);  // stays fully bypassed
  half.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 0.5f);
  wet.setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);

  auto sigDry = makeSine(300.0f, 0.9f, 4096, kSampleRate);
  auto sigHalf = sigDry;
  auto sigWet = sigDry;
  float* chDry[1] = {sigDry.data()};
  float* chHalf[1] = {sigHalf.data()};
  float* chWet[1] = {sigWet.data()};
  dry.process(chDry, 1, static_cast<int32_t>(sigDry.size()));
  half.process(chHalf, 1, static_cast<int32_t>(sigHalf.size()));
  wet.process(chWet, 1, static_cast<int32_t>(sigWet.size()));

  auto origDry = makeSine(300.0f, 0.9f, 4096, kSampleRate);
  CHECK_NEAR(rms(sigDry), rms(origDry), 0.001);  // mix=0 is an exact passthrough

  // half should sit strictly between dry and fully-wet at each sample, since it's a literal
  // linear blend of the two.
  bool anyStrictlyBetween = false;
  for (size_t i = 0; i < sigDry.size(); i++) {
    const float lo = std::min(sigDry[i], sigWet[i]);
    const float hi = std::max(sigDry[i], sigWet[i]);
    CHECK(sigHalf[i] >= lo - 1e-5f && sigHalf[i] <= hi + 1e-5f);
    if (std::fabs(sigWet[i] - sigDry[i]) > 1e-3f) anyStrictlyBetween = true;
  }
  CHECK(anyStrictlyBetween);
}

// --- determinism ---

static void testOutputIsDeterministic() {
  ChorusFlanger a(kSampleRate), b(kSampleRate);
  for (ChorusFlanger* cf : {&a, &b}) {
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerMix), 1.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDelay), 12.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerDepth), 4.0f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerFeedback), 0.3f);
    cf->setParam(static_cast<int32_t>(NodeParam::ChorusFlangerRate), 1.5f);
  }

  auto sigA = makeSine(300.0f, 0.7f, 8192, kSampleRate);
  auto sigB = makeSine(300.0f, 0.7f, 8192, kSampleRate);
  float* chA[1] = {sigA.data()};
  float* chB[1] = {sigB.data()};
  a.process(chA, 1, static_cast<int32_t>(sigA.size()));
  b.process(chB, 1, static_cast<int32_t>(sigB.size()));
  for (size_t i = 0; i < sigA.size(); i++) CHECK(sigA[i] == sigB[i]);
}

int main() {
  testBypassedByDefaultIsExactPassthrough();
  testSilenceInProducesSilenceOut();
  testResetClearsRingBufferNotParams();
  testOutOfRangeParamsStayFiniteAndBounded();
  testDelayDepthExceedsCenterDelayStaysFiniteAndBounded();
  testStaticDelayMatchesExpectedLatencyWhenDepthIsZero();
  testFeedbackProducesRepeatedDecayingTaps();
  testDepthChangesOutputVsStaticDelay();
  testRateChangesOutput();
  testZeroStereoPhaseKeepsChannelsIdentical();
  testNonzeroStereoPhaseWidensChannels();
  testChannelsAreIndependent();
  testMixBlendsDryAndWet();
  testOutputIsDeterministic();

  std::printf("chorus_flanger_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
