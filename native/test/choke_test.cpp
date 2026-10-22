#include <cmath>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "test_main.h"

// Covers choke groups (VoiceManager::trigger / Voice::choke, Engine::trigger / scheduleEvent
// `chokeGroup`): a trigger into group g stops the other sources' active voices in g at its own
// frame, independent of voice allocation. Sample N holds the constant level kLevel[N], so the
// master output level identifies exactly which voices are sounding. Bus ids 1..3 are unmixed here
// (no createBus), so they sum straight into the master output, which is all these tests read.
using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kLen = 48000;
constexpr float kLevel[] = {0.0f, 0.1f, 0.2f, 0.4f};  // samples 1, 2, 3

std::vector<int32_t> g_ended;  // every voice id reported ended by render() so far

void commit(Engine& e, int32_t id) {
  float* ch[2] = {static_cast<float*>(std::calloc(kLen, sizeof(float))),
                  static_cast<float*>(std::calloc(kLen, sizeof(float)))};
  for (int32_t i = 0; i < kLen; i++) ch[0][i] = ch[1][i] = kLevel[id];
  e.commitSample(id, 2, kLen, static_cast<int32_t>(kRate), ch);
}

void setup(Engine& e, int32_t maxVoices = 16) {
  g_ended.clear();
  e.init(kRate, 2, maxVoices);
  for (int32_t id = 1; id <= 3; id++) commit(e, id);
}

std::vector<float> render(Engine& e, int64_t& frame, int32_t blocks) {
  std::vector<float> out;
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    g_ended.insert(g_ended.end(), e.endedVoices().begin(), e.endedVoices().end());
    frame += kBlock;
    out.insert(out.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
  return out;
}

void trig(Engine& e, int32_t voice, int32_t sample, int32_t bus, int32_t group) {
  e.trigger(voice, sample, bus, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, group);
}

bool ended(int32_t voice) {
  for (int32_t v : g_ended) if (v == voice) return true;
  return false;
}

void testNoGroupUnchanged() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 0);
  trig(e, 2, 2, 2, 0);
  auto out = render(e, frame, 4);
  CHECK_NEAR(out[kBlock * 3], 0.1 + 0.2, 1e-4);  // both keep sounding
}

void testChokesOtherSource() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);  // A
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1, 1e-4);
  trig(e, 2, 2, 2, 1);  // B chokes A
  out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.2, 1e-4);  // only B
  CHECK(ended(1));   // the choked voice is reported ended like any other
  CHECK(!ended(2));
  CHECK(e.activeVoiceCount() == 1);
}

void testReverseDirection() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 2, 2, 1);  // B
  render(e, frame, 2);
  trig(e, 2, 1, 1, 1);  // A chokes B
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1, 1e-4);
}

void testMultipleVoicesChoked() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  trig(e, 2, 2, 2, 1);
  trig(e, 3, 3, 3, 1);  // chokes 1 and 2 (the first two choke each other too: 2 chokes 1)
  auto out = render(e, frame, 3);
  CHECK_NEAR(out[kBlock * 2], 0.4, 1e-4);
  CHECK(e.activeVoiceCount() == 1);
}

void testOtherGroupAndNoGroupUnaffected() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 2);  // group 2
  trig(e, 2, 2, 2, 0);  // no group
  render(e, frame, 1);
  trig(e, 3, 3, 3, 1);  // group 1: touches neither
  auto out = render(e, frame, 3);
  CHECK_NEAR(out[kBlock * 2], 0.1 + 0.2 + 0.4, 1e-4);
  trig(e, 4, 1, 3, 0);  // an ungrouped trigger chokes nothing either
  out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1 + 0.2 + 0.4 + 0.1, 1e-4);
}

void testSameSourceNotChoked() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  render(e, frame, 1);
  trig(e, 2, 2, 1, 1);  // same bus: the track's own voices stack as before
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.1 + 0.2, 1e-4);
}

void testChokesWithIdleVoicesAvailable() {
  Engine e;
  setup(e, 64);  // plenty of idle voices: this is not stealing
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  render(e, frame, 1);
  CHECK(e.activeVoiceCount() == 1);
  trig(e, 2, 2, 2, 1);
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.2, 1e-4);
}

void testChokeIsSampleAccurateAndDeclicked() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  render(e, frame, 2);
  const int64_t at = frame + 40;  // mid-block
  e.scheduleEvent(at, 2, 2, 2, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, 1);
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[39], 0.1, 1e-4);               // A untouched before the choke frame
  CHECK(out[40] < 0.1 + 0.2 - 1e-4 && out[40] > 0.2);  // A already fading while B starts
  CHECK(out[60] < out[40]);                     // ...and still fading (declicked, not a hard cut)
  CHECK_NEAR(out[40 + 200], 0.2, 1e-4);         // A fully gone (fade ~96 frames), B remains
  CHECK(e.activeVoiceCount() == 1);
}

void testMaxVoicesStillStealsIndependently() {
  Engine e;
  setup(e, 2);
  int64_t frame = 0;
  trig(e, 1, 1, 1, 0);
  trig(e, 2, 2, 2, 0);
  trig(e, 3, 3, 3, 0);  // pool of 2, no groups: steals the quietest (voice 1) as before
  auto out = render(e, frame, 2);
  CHECK(e.activeVoiceCount() == 2);
  CHECK_NEAR(out[kBlock + 5], 0.2 + 0.4, 1e-4);
}

void testChokedPendingVoiceNeverStarts() {
  Engine e;
  setup(e);
  int64_t frame = 0;
  // Same frame: the later-triggered voice wins deterministically.
  e.scheduleEvent(frame + 30, 1, 1, 1, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, 1);
  e.scheduleEvent(frame + 30, 2, 2, 2, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, 1);
  auto out = render(e, frame, 2);
  CHECK_NEAR(out[kBlock + 5], 0.2, 1e-4);
  CHECK(e.activeVoiceCount() == 1);
}

// --- instruments (synth drums / bass) ---

constexpr int32_t kBass = static_cast<int32_t>(InstrumentKind::Bass);
constexpr int32_t kKick = static_cast<int32_t>(InstrumentKind::Kick);

float maxAbs(const std::vector<float>& v, size_t from, size_t to) {
  float m = 0.0f;
  for (size_t i = from; i < to && i < v.size(); i++) m = std::max(m, std::fabs(v[i]));
  return m;
}

void testInstrumentChokedBySampleVoice() {
  Engine e;
  setup(e);
  CHECK(e.createInstrument(1, kBass, 1));
  int64_t frame = 0;
  e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, -1, /*chokeGroup=*/1);  // held bass, group 1
  auto out = render(e, frame, 4);
  CHECK(e.activeInstrumentCount() == 1);
  CHECK(maxAbs(out, kBlock * 3, kBlock * 4) > 0.01f);
  trig(e, 1, 2, 2, 1);  // sample on another bus, same group
  out = render(e, frame, 3);
  CHECK(e.activeInstrumentCount() == 0);           // cut and reset, not left ringing
  CHECK_NEAR(out[kBlock * 2 + 5], 0.2, 1e-4);      // only the sample remains
}

void testSampleVoiceChokedByInstrument() {
  Engine e;
  setup(e);
  CHECK(e.createInstrument(1, kKick, 2));
  int64_t frame = 0;
  trig(e, 1, 1, 1, 1);
  render(e, frame, 2);
  CHECK(e.activeVoiceCount() == 1);
  e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, -1, 1);
  render(e, frame, 2);
  CHECK(e.activeVoiceCount() == 0);
  CHECK(ended(1));
}

void testInstrumentVsInstrument() {
  Engine e;
  setup(e);
  CHECK(e.createInstrument(1, kBass, 1));
  CHECK(e.createInstrument(2, kBass, 2));
  CHECK(e.createInstrument(3, kBass, 3));
  CHECK(e.createInstrument(4, kBass, 4));
  int64_t frame = 0;
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1, 1);  // chokable
  e.instrumentNoteOn(3, 0.5f, 0.0f, 0.0f, -1, 2);  // other group
  e.instrumentNoteOn(4, 0.5f, 0.0f, 0.0f, -1, 0);  // no group
  render(e, frame, 2);
  CHECK(e.activeInstrumentCount() == 3);
  e.instrumentNoteOn(2, 0.5f, 0.0f, 0.0f, -1, 1);  // chokes only instrument 1
  render(e, frame, 3);
  CHECK(e.activeInstrumentCount() == 3);           // 2, 3 and 4 sound; 1 is gone
}

void testSameBusInstrumentsDoNotChoke() {
  Engine e;
  setup(e);
  CHECK(e.createInstrument(1, kBass, 1));
  CHECK(e.createInstrument(2, kBass, 1));  // a second instance of the same source
  int64_t frame = 0;
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1, 1);
  render(e, frame, 1);
  e.instrumentNoteOn(2, 0.5f, 0.0f, 0.0f, -1, 1);
  render(e, frame, 3);
  CHECK(e.activeInstrumentCount() == 2);
}

// The cut lands on the exact frame of the scheduled note: everything before it is bit-identical
// to the same run with an ungrouped (non-choking) note.
std::vector<float> runScheduledChoke(int32_t chokerGroup, bool withBass = true) {
  Engine e;
  setup(e);
  e.createInstrument(1, kBass, 1);
  e.createInstrument(2, kKick, 2);
  int64_t frame = 0;
  if (withBass) e.instrumentNoteOn(1, 0.6f, 0.0f, 0.0f, -1, 1);
  render(e, frame, 2);
  e.scheduleInstrumentNote(frame + 40, 2, 0.0f, 0.0f, 0.0f, -1, chokerGroup);  // silent velocity: output stays the bass
  return render(e, frame, 3);
}

void testInstrumentChokeIsSampleAccurate() {
  const auto ref = runScheduledChoke(0);
  const auto cut = runScheduledChoke(1);
  for (size_t i = 0; i < 40; i++) CHECK(ref[i] == cut[i]);   // untouched up to the choke frame
  CHECK(cut[41] != ref[41]);                                  // fading from it
  const auto kickOnly = runScheduledChoke(1, /*withBass=*/false);
  for (size_t i = 40 + 120; i < cut.size(); i++) CHECK_NEAR(cut[i], kickOnly[i], 1e-6);  // bass fully gone after the ~2 ms fade
  CHECK(maxAbs(ref, 40 + 120, ref.size()) > maxAbs(kickOnly, 40 + 120, kickOnly.size()) + 0.01f);  // while the reference keeps sounding
}

void testInstrumentOwnNextNoteSurvives() {
  Engine e;
  setup(e);
  e.createInstrument(1, kBass, 1);
  int64_t frame = 0;
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1, 1);
  render(e, frame, 1);
  trig(e, 1, 2, 2, 1);  // cuts the bass
  render(e, frame, 3);
  CHECK(e.activeInstrumentCount() == 0);
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1, 1);  // its next note chokes the sample, and plays
  render(e, frame, 2);
  CHECK(e.activeInstrumentCount() == 1);
  CHECK(e.activeVoiceCount() == 0);
}

}  // namespace

int main() {
  testNoGroupUnchanged();
  testChokesOtherSource();
  testReverseDirection();
  testMultipleVoicesChoked();
  testOtherGroupAndNoGroupUnaffected();
  testSameSourceNotChoked();
  testChokesWithIdleVoicesAvailable();
  testChokeIsSampleAccurateAndDeclicked();
  testMaxVoicesStillStealsIndependently();
  testChokedPendingVoiceNeverStarts();
  testInstrumentChokedBySampleVoice();
  testSampleVoiceChokedByInstrument();
  testInstrumentVsInstrument();
  testSameBusInstrumentsDoNotChoke();
  testInstrumentChokeIsSampleAccurate();
  testInstrumentOwnNextNoteSurvives();
  std::printf("choke_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
