#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// The shared source gain of every synthesized instrument (kInstrumentGainParam, instrument.h):
// owned by the Engine, applied to the instrument's output before it is summed into its bus — so
// before the track's FX chain and before the bus fader (BusGain). dB, default 0 = unity and
// bit-identical to an engine that never heard of the control.
using namespace webdsp;

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
constexpr int32_t kBass = static_cast<int32_t>(InstrumentKind::Bass);

struct Out {
  std::vector<float> l, r;
};

void render(Engine& e, int64_t& frame, int32_t frames, Out* out) {
  for (int32_t done = 0; done < frames; done += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    out->l.insert(out->l.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
    out->r.insert(out->r.end(), e.outputChannel(1), e.outputChannel(1) + kBlock);
  }
}

// One kick note, optional source gain (NaN = never touched), optional bus + saturation + fader.
struct Setup {
  float gainDb = std::numeric_limits<float>::quiet_NaN();
  float pan = 0.0f;
  int32_t bus = 0;
  bool saturate = false;
  float fader = 1.0f;
  float velocity = 0.15f;  // quiet enough that +12 dB stays under the output clip
};

Out playKick(const Setup& s, double seconds = 0.5) {
  Engine e;
  e.init(kRate, 2, 8);
  e.setBusParam(0, static_cast<int32_t>(NodeParam::LimiterEnable), 0.0f);  // measure the gain alone
  e.createInstrument(1, kKick, s.bus);
  if (s.saturate) {
    e.addEffect(s.bus, 1, static_cast<int32_t>(FxKind::Saturation), -1);
    e.setEffectParam(s.bus, 1, static_cast<int32_t>(NodeParam::SatDrive), 24.0f);
    e.setEffectParam(s.bus, 1, static_cast<int32_t>(NodeParam::SatMix), 1.0f);
  }
  if (s.fader != 1.0f) e.setBusParam(s.bus, static_cast<int32_t>(NodeParam::BusGain), s.fader);
  if (!std::isnan(s.gainDb)) e.setInstrumentParam(1, kInstrumentGainParam, s.gainDb);
  e.instrumentNoteOn(1, s.velocity, s.pan);
  int64_t f = 0;
  Out o;
  render(e, f, static_cast<int32_t>(seconds * kRate), &o);
  return o;
}

bool identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Max |a[i] * k - b[i]| — how far `b` is from `a` scaled by k.
float scaledError(const std::vector<float>& a, const std::vector<float>& b, float k) {
  float m = 0.0f;
  for (size_t i = 0; i < a.size(); i++) m = std::fmax(m, std::fabs(a[i] * k - b[i]));
  return m;
}
float peak(const std::vector<float>& v) {
  float p = 0.0f;
  for (float x : v) p = std::fmax(p, std::fabs(x));
  return p;
}

void testUnityIsBitIdentical() {
  const Out untouched = playKick({});
  Setup zero;
  zero.gainDb = 0.0f;
  const Out set0 = playKick(zero);
  CHECK(peak(untouched.l) > 0.05f);
  CHECK(identical(untouched.l, set0.l) && identical(untouched.r, set0.r));
}

void testAttenuatesAndBoosts() {
  const Out ref = playKick({});
  for (float db : {-24.0f, -12.0f, -6.0f, 6.0f, 12.0f}) {
    Setup s;
    s.gainDb = db;
    const Out o = playKick(s);
    const float k = std::pow(10.0f, db / 20.0f);
    CHECK(scaledError(ref.l, o.l, k) < 1e-5f * std::fmax(1.0f, k));
    CHECK(scaledError(ref.r, o.r, k) < 1e-5f * std::fmax(1.0f, k));
  }
  Setup s;
  s.gainDb = -6.0206f;
  CHECK_NEAR(peak(playKick(s).l) / peak(ref.l), 0.5, 1e-3);
  s.gainDb = 6.0206f;
  CHECK_NEAR(peak(playKick(s).l) / peak(ref.l), 2.0, 2e-3);
}

void testStereoStaysStereo() {
  Setup hard;
  hard.pan = -1.0f;  // hard left: the right channel carries nothing, gain must not leak into it
  const Out ref = playKick(hard);
  CHECK(peak(ref.l) > 0.1f && peak(ref.r) < 1e-6f);
  hard.gainDb = 9.0f;
  const Out o = playKick(hard);
  CHECK(peak(o.r) < 1e-6f);
  CHECK(scaledError(ref.l, o.l, std::pow(10.0f, 9.0f / 20.0f)) < 1e-5f);
  Setup centre;
  centre.gainDb = -7.0f;
  const Out c = playKick(centre);  // centre: channels stay identical, both scaled
  CHECK(identical(c.l, c.r));
  Setup half;
  half.pan = 0.5f;
  const Out p = playKick(half);
  half.gainDb = -7.0f;
  const Out pg = playKick(half);
  const float k = std::pow(10.0f, -7.0f / 20.0f);
  CHECK(scaledError(p.l, pg.l, k) < 1e-5f && scaledError(p.r, pg.r, k) < 1e-5f);  // balance preserved
}

// The gain sits before the bus chain: into a saturator it changes the *shape* (drive), which a gain
// after the chain (the fader) cannot; and the fader stays an independent, purely linear stage.
void testGainIsBeforeChainAndFaderIsIndependent() {
  Setup sat;
  sat.bus = 1;
  sat.saturate = true;
  sat.velocity = 0.6f;  // loud enough to drive the saturator into its curve
  const Out ref = playKick(sat);
  Setup quiet = sat;
  quiet.gainDb = -12.0f;
  const Out q = playKick(quiet);
  const float k = std::pow(10.0f, -12.0f / 20.0f);
  CHECK(scaledError(ref.l, q.l, k) > 0.02f);  // not just a scaled copy: the saturator saw less level
  Setup faded = sat;
  faded.fader = k;
  const Out f = playKick(faded);
  CHECK(scaledError(ref.l, f.l, k) < 1e-5f);  // fader after the chain: exactly a scaled copy
  CHECK(scaledError(f.l, q.l, 1.0f) > 0.02f);  // so the two controls are not interchangeable

  // Independent: source gain and fader multiply (linear chain, no FX).
  Setup plain;
  plain.bus = 1;
  const Out p = playKick(plain);
  plain.gainDb = -6.0f;
  plain.fader = 0.5f;
  const Out both = playKick(plain);
  CHECK(scaledError(p.l, both.l, 0.5f * std::pow(10.0f, -6.0f / 20.0f)) < 1e-5f);
}

void testClampAndInvalidValues() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kKick, 0);
  e.setInstrumentParam(1, kInstrumentGainParam, 100.0f);
  CHECK_NEAR(e.instrumentBaseParam(1, kInstrumentGainParam), kInstrumentGainMaxDb, 1e-6);
  e.setInstrumentParam(1, kInstrumentGainParam, -100.0f);
  CHECK_NEAR(e.instrumentBaseParam(1, kInstrumentGainParam), kInstrumentGainMinDb, 1e-6);
  e.setInstrumentParam(1, kInstrumentGainParam, -3.0f);
  e.setInstrumentParam(1, kInstrumentGainParam, std::numeric_limits<float>::quiet_NaN());
  e.setInstrumentParam(1, kInstrumentGainParam, std::numeric_limits<float>::infinity());
  CHECK_NEAR(e.instrumentBaseParam(1, kInstrumentGainParam), -3.0, 1e-6);
}

void testCatalogueExposesGainOnEveryKind() {
  for (int32_t kind = 0; kind <= static_cast<int32_t>(InstrumentKind::Edge); kind++) {
    const int32_t n = instrumentParamCount(kind);
    CHECK(n >= 2 && n <= kMaxModParams);
    const InstrumentParamInfo* g = instrumentParamInfo(kind, n - 1);
    CHECK(g && g->id == kInstrumentGainParam && std::strcmp(g->name, "gain") == 0 &&
          std::strcmp(g->unit, "dB") == 0 && g->def == 0.0f && g->min == -24.0f && g->max == 12.0f);
    CHECK(resolveModDomain(*g) == ModDomain::None);
    CHECK(instrumentParamInfo(kind, n) == nullptr);
    // Ids stay unique across the table (the kind's own ids never collide with the shared one).
    for (int32_t i = 0; i + 1 < n; i++) CHECK(instrumentParamInfo(kind, i)->id != kInstrumentGainParam);
  }
}

// A held (sustained) instrument whose gain changes mid-note must not step: the change ramps across
// the chunk, so the largest sample-to-sample move stays on the order of the signal's own.
void testChangeWhileSoundingDoesNotStep() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kBass, 0);
  e.instrumentNoteOn(1, 0.9f);
  int64_t f = 0;
  Out warm;
  render(e, f, kBlock * 200, &warm);
  const Out& w = warm;
  float ownJump = 0.0f;
  for (size_t i = w.l.size() - kBlock * 20 + 1; i < w.l.size(); i++)
    ownJump = std::fmax(ownJump, std::fabs(w.l[i] - w.l[i - 1]));
  CHECK(peak(w.l) > 0.05f);
  e.setInstrumentParam(1, kInstrumentGainParam, -24.0f);  // the biggest possible drop
  Out after;
  render(e, f, kBlock * 4, &after);
  float jump = std::fabs(after.l[0] - w.l.back());
  for (size_t i = 1; i < after.l.size(); i++) jump = std::fmax(jump, std::fabs(after.l[i] - after.l[i - 1]));
  CHECK(jump < ownJump * 1.5f + 1e-4f);
  CHECK(peak(std::vector<float>(after.l.end() - kBlock, after.l.end())) < peak(w.l) * 0.1f);  // settled
}

// A note starting after a gain change made while idle must not ramp up from the old value.
void testIdleChangeAppliesFromFirstFrame() {
  Setup s;
  s.gainDb = -12.0f;
  const Out ref = playKick({});
  const Out o = playKick(s);
  CHECK(scaledError(ref.l, o.l, std::pow(10.0f, -12.0f / 20.0f)) < 1e-5f);  // the very first frames too
}

void testRenderAllocatesNothing() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kBass, 1);
  e.setInstrumentParam(1, kInstrumentGainParam, -9.0f);
  e.instrumentNoteOn(1, 0.9f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 400; i++) {
    if (i % 50 == 0) e.setInstrumentParam(1, kInstrumentGainParam, -9.0f + float(i % 7));
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testUnityIsBitIdentical();
  testAttenuatesAndBoosts();
  testStereoStaysStereo();
  testGainIsBeforeChainAndFaderIsIndependent();
  testClampAndInvalidValues();
  testCatalogueExposesGainOnEveryKind();
  testChangeWhileSoundingDoesNotStep();
  testIdleChangeAppliesFromFirstFrame();
  testRenderAllocatesNothing();
  std::printf("instrument_gain_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
