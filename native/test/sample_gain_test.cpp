#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers SampleParam::Gain (native/src/params.h, sample_store.h, voice.h#render) — the
// sample/instrument-level gain stage, upstream of a voice's own VoiceParam::Gain and of the
// bus/track chain it renders into. See ARCHITECTURE.md, "How samples are represented" /
// "How DSP is composed".
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr float kToneLevel = 0.5f;

void commitTone(Engine& e, int32_t id, int32_t frames, float level = kToneLevel) {
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = level;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

void render(Engine& e, int64_t& frame, int32_t blocks, std::vector<float>* out) {
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    if (!out) continue;
    for (int32_t c = 0; c < 2; c++) out->insert(out->end(), e.outputChannel(c), e.outputChannel(c) + kBlock);
  }
}

float peakOf(const std::vector<float>& v) {
  float peak = 0.0f;
  for (float x : v) peak = std::max(peak, std::fabs(x));
  return peak;
}

// A sample that's never had its gain touched must render exactly as it did before
// SampleParam::Gain existed — this is the "preserve existing behaviour" requirement.
void testDefaultGainIsUnity() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 4800);

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &out);

  CHECK(std::fabs(peakOf(out) - kToneLevel) < 1e-5f);
}

// Explicitly setting unity gain is a documented no-op, not just the default's accidental
// absence of one.
void testExplicitUnityGainMatchesDefault() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 4800);
  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 1.0f);

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &out);

  CHECK(std::fabs(peakOf(out) - kToneLevel) < 1e-5f);
}

// The gain actually scales what's heard.
void testGainScalesOutput() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 4800);
  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 0.25f);

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &out);

  CHECK(std::fabs(peakOf(out) - kToneLevel * 0.25f) < 1e-5f);
}

// Sample gain and per-voice (trigger) gain are independent stages that multiply, not one
// collapsing into the other — the whole point of keeping them as separate primitives (see
// voice.h#render's doc comment) is that a caller can drive one (velocity/event level) without
// having to know or recompute the other (the instrument's own fixed level).
void testMultipliesWithVoiceGain() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 4800);
  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 0.5f);

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 0.5f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &out);

  CHECK(std::fabs(peakOf(out) - kToneLevel * 0.25f) < 1e-5f);
}

// Changing a sample's gain takes effect immediately on a voice already playing that sample —
// not just on voices triggered afterward (an instrument-gain slider dragged during playback
// should be heard right away, not only on the next note).
void testLiveUpdateAffectsAlreadyPlayingVoice() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 48000);  // long tone — still sounding when gain changes mid-flight

  int64_t frame = 0;
  std::vector<float> before;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, true, false);
  render(e, frame, 2, &before);
  CHECK(std::fabs(peakOf(before) - kToneLevel) < 1e-5f);

  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 0.0f);
  std::vector<float> after;
  render(e, frame, 2, &after);
  CHECK(peakOf(after) == 0.0f);
}

// Sample gain is a property of the sample (SampleStore), not transient per-voice state — a
// second, later voice triggered on the same sample id picks up whatever gain is currently set,
// the same as the first.
void testGainPersistsAcrossRetrigger() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 4800);
  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 0.5f);

  int64_t frame = 0;
  std::vector<float> first;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &first);
  CHECK(std::fabs(peakOf(first) - kToneLevel * 0.5f) < 1e-5f);
  e.stop(1);  // otherwise voice 1 is still sounding and sums with voice 2 below

  std::vector<float> second;
  e.trigger(2, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &second);
  CHECK(std::fabs(peakOf(second) - kToneLevel * 0.5f) < 1e-5f);
}

// Sample gain renders upstream of the bus/track chain, not instead of it: it composes
// multiplicatively with the bus's own output stage (NodeParam::BusGain — the primitive the
// tracker's "track gain"/mixer-output control already uses), it doesn't bypass or replace it.
void testComposesWithBusGain() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 4800);
  e.setSampleParam(1, static_cast<int32_t>(SampleParam::Gain), 0.5f);
  e.setBusParam(1, static_cast<int32_t>(NodeParam::BusGain), 2.0f);  // track bus 1

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, /*busId=*/1, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &out);

  // 0.5 (sample) * 2.0 (bus) == 1.0: back to the raw tone level.
  CHECK(std::fabs(peakOf(out) - kToneLevel) < 1e-4f);
}

// A reference to an id that was never loaded (or already removed) is a harmless no-op,
// matching every other host-facing setter in this engine (a malformed/late command must not
// be able to corrupt render-thread state).
void testUnknownSampleIdIsNoOp() {
  Engine e;
  e.init(kRate, 2, 64);
  commitTone(e, 1, 4800);
  e.setSampleParam(42, static_cast<int32_t>(SampleParam::Gain), 0.0f);  // never loaded

  int64_t frame = 0;
  std::vector<float> out;
  e.trigger(1, 1, 0, 1.0f, 1.0f, 0, -1, false, false);
  render(e, frame, 4, &out);

  CHECK(std::fabs(peakOf(out) - kToneLevel) < 1e-5f);
}

}  // namespace

int main() {
  testDefaultGainIsUnity();
  testExplicitUnityGainMatchesDefault();
  testGainScalesOutput();
  testMultipliesWithVoiceGain();
  testLiveUpdateAffectsAlreadyPlayingVoice();
  testGainPersistsAcrossRetrigger();
  testComposesWithBusGain();
  testUnknownSampleIdIsNoOp();

  std::printf("sample_gain_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
