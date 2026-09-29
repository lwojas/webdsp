#include <cmath>
#include <vector>
#include "../src/dsp/reverb.h"
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

// Runs `blocks` blocks of `blockSize` frames through a stereo Reverb, feeding `inL[i]`
// (zero elsewhere) as the very first sample and silence after. Returns the full stereo
// output, interleaved as [L0, R0, L1, R1, ...].
std::vector<float> renderImpulse(Reverb& reverb, int32_t blocks, int32_t blockSize) {
  std::vector<float> outL(static_cast<size_t>(blocks) * blockSize);
  std::vector<float> outR(static_cast<size_t>(blocks) * blockSize);
  std::vector<float> interleaved;
  interleaved.reserve(outL.size() * 2);

  std::vector<float> l(blockSize, 0.0f), r(blockSize, 0.0f);
  l[0] = 1.0f;
  r[0] = 1.0f;
  float* channels[2] = {l.data(), r.data()};
  reverb.process(channels, 2, blockSize);
  for (int32_t i = 0; i < blockSize; i++) {
    interleaved.push_back(l[i]);
    interleaved.push_back(r[i]);
  }

  for (int32_t b = 1; b < blocks; b++) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    reverb.process(channels, 2, blockSize);
    for (int32_t i = 0; i < blockSize; i++) {
      interleaved.push_back(l[i]);
      interleaved.push_back(r[i]);
    }
  }
  return interleaved;
}

}  // namespace

// --- lifecycle / bypass ---

static void testBypassedByDefaultIsExactPassthrough() {
  Reverb reverb(kSampleRate);
  std::vector<float> l = {0.1f, -0.2f, 0.3f, 0.0f, 0.5f};
  std::vector<float> r = {-0.1f, 0.2f, -0.3f, 0.0f, -0.5f};
  std::vector<float> origL = l, origR = r;
  float* channels[2] = {l.data(), r.data()};
  reverb.process(channels, 2, static_cast<int32_t>(l.size()));
  for (size_t i = 0; i < l.size(); i++) {
    CHECK_NEAR(l[i], origL[i], 1e-9);
    CHECK_NEAR(r[i], origR[i], 1e-9);
  }
}

static void testSilenceInProducesSilenceOut() {
  Reverb reverb(kSampleRate);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 1.0f);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.5f);
  std::vector<float> l(256, 0.0f), r(256, 0.0f);
  float* channels[2] = {l.data(), r.data()};
  for (int i = 0; i < 20; i++) reverb.process(channels, 2, 256);
  for (float v : l) CHECK(v == 0.0f);
  for (float v : r) CHECK(v == 0.0f);
}

static void testResetClearsAccumulatedTankState() {
  Reverb reverb(kSampleRate);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 1.0f);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.7f);

  std::vector<float> l(256, 0.0f), r(256, 0.0f);
  l[0] = 1.0f;
  r[0] = 1.0f;
  float* channels[2] = {l.data(), r.data()};
  reverb.process(channels, 2, 256);  // excite the tank
  // The diffusion chain + tank delays are thousands of samples long (Table 1's lengths,
  // scaled to this sample rate) — a single 256-frame block isn't enough for the impulse to
  // reach any output tap yet (see testImpulseProducesDecayingTail's block-0-silent check),
  // so keep feeding silence until the tail actually arrives before testing reset().
  bool anyNonZeroBeforeReset = false;
  for (int i = 0; i < 40 && !anyNonZeroBeforeReset; i++) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    reverb.process(channels, 2, 256);
    for (float v : l) anyNonZeroBeforeReset |= (v != 0.0f);
  }
  CHECK(anyNonZeroBeforeReset);

  reverb.reset();
  std::fill(l.begin(), l.end(), 0.0f);
  std::fill(r.begin(), r.end(), 0.0f);
  reverb.process(channels, 2, 256);
  for (float v : l) CHECK(v == 0.0f);
  for (float v : r) CHECK(v == 0.0f);
}

// --- parameter bounds ---

static void testOutOfRangeParamsStayFiniteAndBounded() {
  Reverb reverb(kSampleRate);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 5.0f);       // should clamp to 1.0
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 100.0f);   // should clamp below 1.0
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDamping), -3.0f);  // should clamp to 0.0

  auto interleaved = renderImpulse(reverb, /*blocks=*/40, /*blockSize=*/256);
  CHECK(allFinite(interleaved));
  for (float v : interleaved) CHECK(std::fabs(v) < 4.0f);  // never blows up past a sane bound
}

// --- impulse / decay behavior ---

static void testImpulseProducesDecayingTail() {
  Reverb reverb(kSampleRate);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 1.0f);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.5f);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDamping), 0.2f);

  auto interleaved = renderImpulse(reverb, /*blocks=*/188, /*blockSize=*/256);  // ~1s @ 48kHz
  CHECK(allFinite(interleaved));

  // De-interleave into per-block RMS to check the shape of the tail.
  const int32_t blockSize = 256;
  const int32_t blocks = static_cast<int32_t>(interleaved.size() / 2 / blockSize);
  std::vector<float> blockRms(blocks);
  for (int32_t b = 0; b < blocks; b++) {
    std::vector<float> block;
    block.reserve(blockSize * 2);
    for (int32_t i = 0; i < blockSize * 2; i++) block.push_back(interleaved[b * blockSize * 2 + i]);
    blockRms[b] = rms(block);
  }

  CHECK(blockRms[0] == 0.0f);  // wet tail hasn't reached the taps yet, on the very first block
  bool sawEnergy = false;
  for (int32_t b = 1; b < blocks; b++) sawEnergy |= (blockRms[b] > 1e-4f);
  CHECK(sawEnergy);

  // Tail must actually decay: the last 10% of blocks should be much quieter than the loudest
  // block, not sustained or growing (a runaway tank would fail this).
  float peak = 0.0f;
  for (float v : blockRms) peak = std::max(peak, v);
  float tailAvg = 0.0f;
  const int32_t tailStart = blocks - blocks / 10;
  for (int32_t b = tailStart; b < blocks; b++) tailAvg += blockRms[b];
  tailAvg /= static_cast<float>(blocks - tailStart);
  CHECK(peak > 0.001f);
  CHECK(tailAvg < peak * 0.5f);
}

static void testOutputIsDeterministic() {
  Reverb a(kSampleRate);
  Reverb b(kSampleRate);
  a.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 1.0f);
  b.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 1.0f);
  a.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.6f);
  b.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.6f);

  auto outA = renderImpulse(a, /*blocks=*/20, /*blockSize=*/128);
  auto outB = renderImpulse(b, /*blocks=*/20, /*blockSize=*/128);
  CHECK(outA.size() == outB.size());
  for (size_t i = 0; i < outA.size(); i++) CHECK(outA[i] == outB[i]);
}

// --- channel behavior ---

static void testStereoOutputsDifferForMonoImpulse() {
  Reverb reverb(kSampleRate);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 1.0f);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.6f);

  auto interleaved = renderImpulse(reverb, /*blocks=*/60, /*blockSize=*/256);
  bool sawDifference = false;
  for (size_t i = 0; i + 1 < interleaved.size(); i += 2) {
    if (interleaved[i] != interleaved[i + 1]) sawDifference = true;
  }
  CHECK(sawDifference);  // the L/R tap structure (Table 2) must produce a genuinely stereo tail
}

static void testMonoChannelCountDoesNotCrashAndStaysFinite() {
  Reverb reverb(kSampleRate);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbMix), 1.0f);
  reverb.setParam(static_cast<int32_t>(NodeParam::ReverbDecay), 0.6f);

  std::vector<float> mono(256, 0.0f);
  mono[0] = 1.0f;
  float* channels[1] = {mono.data()};
  for (int i = 0; i < 40; i++) reverb.process(channels, 1, 256);
  CHECK(allFinite(mono));
}

int main() {
  testBypassedByDefaultIsExactPassthrough();
  testSilenceInProducesSilenceOut();
  testResetClearsAccumulatedTankState();
  testOutOfRangeParamsStayFiniteAndBounded();
  testImpulseProducesDecayingTail();
  testOutputIsDeterministic();
  testStereoOutputsDifferForMonoImpulse();
  testMonoChannelCountDoesNotCrashAndStaysFinite();

  std::printf("reverb_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
