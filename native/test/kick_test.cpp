#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the synthesized Kick instrument (instruments/kick.h) through the Engine's instrument
// API, asserting on rendered buffers. Set KICK_WAV_DIR=<dir> to also write each rendered case
// as a 16-bit WAV for listening (see docs/kick-instrument.md).
using namespace webdsp;

// Allocation counter: lets a test prove the render path allocates nothing.
static long g_allocs = 0;
void* operator new(std::size_t n) {
  g_allocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kKick = static_cast<int32_t>(InstrumentKind::Kick);

struct Out {
  std::vector<float> l, r;
  size_t size() const { return l.size(); }
};

int32_t id(KickParam p) { return static_cast<int32_t>(p); }

void render(Engine& e, int64_t& frame, int32_t frames, Out* out) {
  for (int32_t done = 0; done < frames; done += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    out->l.insert(out->l.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
    out->r.insert(out->r.end(), e.outputChannel(1), e.outputChannel(1) + kBlock);
  }
}

void writeWav(const char* name, const Out& o) {
  const char* dir = std::getenv("KICK_WAV_DIR");
  if (!dir) return;
  std::string path = std::string(dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(o.size()), dataBytes = n * 4, rate = 48000;
  const uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
  const uint32_t riff = 36 + dataBytes, fmt = 16, byteRate = rate * 4;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    const int16_t s[2] = {static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, o.l[i])) * 32767),
                          static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, o.r[i])) * 32767)};
    std::fwrite(s, 2, 2, f);
  }
  std::fclose(f);
}

struct Setup {
  float tune = -1, drop = -1, pitchDecay = -1, decay = -1, punch = -1, drive = -1;
  float velocity = 1.0f, pan = 0.0f;
};

// One kick note (instrument id 1, master bus), `seconds` of output.
Out play(const Setup& s, double seconds, const char* wavName = nullptr) {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, 0);
  if (s.tune >= 0) e.setInstrumentParam(1, id(KickParam::Tune), s.tune);
  if (s.drop >= 0) e.setInstrumentParam(1, id(KickParam::PitchDrop), s.drop);
  if (s.pitchDecay >= 0) e.setInstrumentParam(1, id(KickParam::PitchDecay), s.pitchDecay);
  if (s.decay >= 0) e.setInstrumentParam(1, id(KickParam::Decay), s.decay);
  if (s.punch >= 0) e.setInstrumentParam(1, id(KickParam::Punch), s.punch);
  if (s.drive >= 0) e.setInstrumentParam(1, id(KickParam::Drive), s.drive);
  e.instrumentNoteOn(1, s.velocity, s.pan);
  int64_t f = 0;
  Out o;
  render(e, f, static_cast<int32_t>(seconds * kRate), &o);
  if (wavName) writeWav(wavName, o);
  return o;
}

size_t at(double sec) { return static_cast<size_t>(sec * kRate); }

float peak(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
  float p = 0;
  for (size_t i = a; i < v.size() && i < b; i++) p = std::fmax(p, std::fabs(v[i]));
  return p;
}
double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; i++) s += double(v[i]) * v[i];
  return std::sqrt(s / double(b - a));
}
// Mean frequency estimate (Hz) from upward zero crossings in [a, b).
double zcFreq(const std::vector<float>& v, size_t a, size_t b) {
  int n = 0;
  for (size_t i = a + 1; i < b && i < v.size(); i++) n += (v[i - 1] < 0 && v[i] >= 0) ? 1 : 0;
  return n / ((b - a) / kRate);
}

void testProducesFiniteBoundedAudio() {
  Out o = play({}, 1.0, "default");
  CHECK(peak(o.l) > 0.3f);
  CHECK(peak(o.l) <= 1.0f);
  bool finite = true;
  for (float v : o.l) finite = finite && std::isfinite(v);
  CHECK(finite);
  // Mono source, centre pan => identical channels.
  CHECK(std::memcmp(o.l.data(), o.r.data(), o.size() * sizeof(float)) == 0);
  // Extremes stay bounded too.
  Setup hot; hot.punch = 1; hot.drive = 1; hot.drop = 4; hot.tune = 120;
  CHECK(peak(play(hot, 0.5, "extreme").l) <= 1.0f);
}

void testDeterministic() {
  Out a = play({}, 0.5), b = play({}, 0.5);
  CHECK(std::memcmp(a.l.data(), b.l.data(), a.size() * sizeof(float)) == 0);
}

void testPitchFallsAndEnvelopeDecays() {
  Setup s; s.drive = 0; s.punch = 0;
  Out o = play(s, 1.0);
  const double early = zcFreq(o.l, at(0.002), at(0.022));
  const double late = zcFreq(o.l, at(0.12), at(0.32));
  CHECK(early > late * 1.8);
  CHECK_NEAR(late, 50.0, 6.0);  // settles on tune
  CHECK(rms(o.l, at(0.0), at(0.05)) > rms(o.l, at(0.2), at(0.25)) * 2.0);
  // Finishes completely: exact silence after decay (0.45 s default) + guard.
  CHECK(peak(o.l, at(0.5)) == 0.0f);
}

void testVoiceEndsAndIsReported() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, 0);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0;
  Out o;
  render(e, f, at(0.1), &o);
  CHECK(e.activeInstrumentCount() == 1);
  render(e, f, at(1.0), &o);
  CHECK(e.activeInstrumentCount() == 0);
  CHECK(e.instrumentCount() == 1);
}

void testParametersAffectExpectedAspects() {
  Setup base; base.drive = 0; base.punch = 0;
  Out ref = play(base, 1.0);

  Setup t = base; t.tune = 90;
  CHECK(zcFreq(play(t, 1.0).l, at(0.15), at(0.3)) > zcFreq(ref.l, at(0.15), at(0.3)) * 1.5);

  Setup d = base; d.decay = 1.2;
  CHECK(rms(play(d, 1.5).l, at(0.3), at(0.4)) > rms(ref.l, at(0.3), at(0.4)) * 3.0);

  Setup pd = base; pd.drop = 0;  // no sweep
  CHECK(zcFreq(play(pd, 1.0).l, at(0.002), at(0.022)) < zcFreq(ref.l, at(0.002), at(0.022)) * 0.6);

  Setup slow = base; slow.pitchDecay = 0.2;
  CHECK(zcFreq(play(slow, 1.0).l, at(0.05), at(0.1)) > zcFreq(ref.l, at(0.05), at(0.1)) * 1.3);

  // Punch: changes the first few ms only; the body after the click has decayed is unchanged.
  Setup p = base; p.punch = 1;
  Out punchy = play(p, 1.0, "punch");
  double headDiff = 0, tailDiff = 0;
  for (size_t i = 0; i < at(0.005); i++) headDiff += std::fabs(punchy.l[i] - ref.l[i]);
  for (size_t i = at(0.05); i < at(0.3); i++) tailDiff = std::fmax(tailDiff, std::fabs(punchy.l[i] - ref.l[i]));
  CHECK(headDiff > 0.5);
  CHECK(tailDiff < 1e-3);

  // Drive: flatter wave (higher rms for same/lower peak) with extra harmonics.
  Setup dr = base; dr.drive = 1;
  Out driven = play(dr, 1.0, "drive");
  CHECK(rms(driven.l, 0, at(0.2)) / peak(driven.l) > rms(ref.l, 0, at(0.2)) / peak(ref.l) * 1.15);
  CHECK(std::memcmp(driven.l.data(), ref.l.data(), 1000 * sizeof(float)) != 0);

  // Velocity scales level linearly (drive 0).
  Setup v = base; v.velocity = 0.5f;
  CHECK_NEAR(peak(play(v, 0.3).l) * 2.0, peak(ref.l), 1e-3);
}

void testParamValidation() {
  CHECK(instrumentParamCount(kKick) == 6 + 1);  // + the shared source gain
  CHECK(instrumentParamCount(99) == 0);
  CHECK(instrumentParamInfo(kKick, 7) == nullptr);
  for (int32_t i = 0; i < instrumentParamCount(kKick); i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kKick, i);
    CHECK(p->min < p->max && p->def >= p->min && p->def <= p->max);
  }
  // Out-of-range clamps (decay 99 s behaves as decay 2 s); NaN and unknown ids are ignored.
  Setup huge; huge.decay = 99;
  Setup max; max.decay = 2.0;
  Out a = play(huge, 0.3), b = play(max, 0.3);
  CHECK(std::memcmp(a.l.data(), b.l.data(), a.size() * sizeof(float)) == 0);

  Engine e;
  e.init(kRate, 2, 8);
  CHECK(!e.createInstrument(0, kKick, 0));
  CHECK(!e.createInstrument(1, 42, 0));
  CHECK(e.createInstrument(1, kKick, 0));
  CHECK(!e.createInstrument(1, kKick, 0));
  e.setInstrumentParam(1, id(KickParam::Tune), NAN);
  e.setInstrumentParam(1, 77, 1.0f);
  e.setInstrumentParam(555, id(KickParam::Tune), 1.0f);  // unknown instrument: no-op
  e.instrumentNoteOn(555, 1.0f);
  CHECK(e.removeInstrument(1));
  CHECK(!e.removeInstrument(1));
}

void testScheduledNoteIsSampleAccurate() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, 0);
  e.scheduleInstrumentNote(300, 1, 1.0f);
  int64_t f = 0;
  Out o;
  render(e, f, 640, &o);
  CHECK(peak(o.l, 0, 300) == 0.0f);
  CHECK(peak(o.l, 300, 330) > 0.0f);

  // cancelScheduled drops pending instrument notes too.
  Engine e2;
  e2.init(kRate, 2, 8);
  e2.createInstrument(1, kKick, 0);
  e2.scheduleInstrumentNote(300, 1, 1.0f);
  e2.cancelScheduled(0);
  int64_t f2 = 0;
  Out o2;
  render(e2, f2, 640, &o2);
  CHECK(peak(o2.l) == 0.0f);
}

void testRoutesThroughBusAndFx() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, /*busId=*/1);
  e.setBusParam(1, static_cast<int32_t>(NodeParam::BusGain), 0.5f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0;
  Out o;
  render(e, f, at(0.3), &o);
  const float routed = peak(o.l);

  Out direct = play({}, 0.3);
  CHECK(routed > 0.0f);
  CHECK_NEAR(routed, peak(direct.l) * 0.5, 1e-3);

  // Track-bus FX chain applies to instrument audio: a saturator changes the waveform.
  Engine e2;
  e2.init(kRate, 2, 8);
  e2.createInstrument(1, kKick, 1);
  e2.setBusParam(1, static_cast<int32_t>(NodeParam::SatDrive), 30.0f);
  e2.setBusParam(1, static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  e2.instrumentNoteOn(1, 1.0f);
  int64_t f2 = 0;
  Out o2;
  render(e2, f2, at(0.3), &o2);
  CHECK(std::memcmp(o2.l.data(), direct.l.data(), 1000 * sizeof(float)) != 0);
}

void testPan() {
  Setup left; left.pan = -1.0f;
  Out o = play(left, 0.2);
  CHECK(peak(o.l) > 0.3f);
  CHECK(peak(o.r) == 0.0f);
}

void testRetriggerIsContinuous() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, 0);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0;
  Out o;
  render(e, f, at(0.1), &o);
  const size_t seam = o.size();
  e.instrumentNoteOn(1, 1.0f);
  render(e, f, at(0.2), &o);
  float worst = 0;
  for (size_t i = seam - 2; i < seam + 400; i++) worst = std::fmax(worst, std::fabs(o.l[i + 1] - o.l[i]));
  CHECK(worst < 0.2f);  // no step discontinuity at the retrigger (a hard restart would jump ~0.8)
  CHECK(e.activeInstrumentCount() == 1);
}

void testNoteOffChokes() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, 0);
  e.setInstrumentParam(1, id(KickParam::Decay), 2.0f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0;
  Out o;
  render(e, f, at(0.05), &o);
  e.instrumentNoteOff(1);
  render(e, f, at(0.1), &o);
  CHECK(peak(o.l, at(0.05) + at(0.02)) == 0.0f);
  CHECK(e.activeInstrumentCount() == 0);
}

void testRenderDoesNotAllocate() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, 1);
  e.setInstrumentParam(1, id(KickParam::Drive), 0.8f);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);  // warm: first block may lazily create bus state
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 400; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f);
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testProducesFiniteBoundedAudio();
  testDeterministic();
  testPitchFallsAndEnvelopeDecays();
  testVoiceEndsAndIsReported();
  testParametersAffectExpectedAspects();
  testParamValidation();
  testScheduledNoteIsSampleAccurate();
  testRoutesThroughBusAndFx();
  testPan();
  testRetriggerIsContinuous();
  testNoteOffChokes();
  testRenderDoesNotAllocate();
  std::printf("%d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
