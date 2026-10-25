#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers Snare, Clap, Tom and Hi-Hat (instruments/*.h) through the Engine's instrument API:
// lifecycle, bounded/finite/deterministic output, per-parameter behaviour, note pitch,
// retrigger/choke, sample-accurate start and a zero-allocation render path. Set
// PERC_WAV_DIR=<dir> to write each default hit as a 16-bit WAV for listening.
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
constexpr int32_t kSnare = static_cast<int32_t>(InstrumentKind::Snare);
constexpr int32_t kClap = static_cast<int32_t>(InstrumentKind::Clap);
constexpr int32_t kTom = static_cast<int32_t>(InstrumentKind::Tom);
constexpr int32_t kHat = static_cast<int32_t>(InstrumentKind::HiHat);
const int32_t kAllPerc[] = {kSnare, kClap, kTom, kHat};

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

void writeWav(const char* name, const Out& o) {
  const char* dir = std::getenv("PERC_WAV_DIR");
  if (!dir) return;
  std::string path = std::string(dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(o.l.size()), dataBytes = n * 4, rate = 48000;
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

struct Note {
  float velocity = 1.0f, pan = 0.0f, pitch = 0.0f;
};
struct P {
  int32_t id;
  float v;
};

// One hit on a fresh engine, with the listed parameter overrides.
Out play(int32_t kind, std::initializer_list<P> params, double seconds, Note n = {},
         const char* wav = nullptr) {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kind, 0);
  for (const P& p : params) e.setInstrumentParam(1, p.id, p.v);
  e.instrumentNoteOn(1, n.velocity, n.pan, n.pitch);
  int64_t f = 0;
  Out o;
  render(e, f, static_cast<int32_t>(seconds * kRate), &o);
  if (wav) writeWav(wav, o);
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
double zcFreq(const std::vector<float>& v, size_t a, size_t b) {
  int n = 0;
  for (size_t i = a + 1; i < b && i < v.size(); i++) n += (v[i - 1] < 0 && v[i] >= 0) ? 1 : 0;
  return n / ((b - a) / kRate);
}
// Power at `hz` over [a, b) (Goertzel): "how much of this pitch is in the hit".
double tonePower(const std::vector<float>& v, double hz, size_t a, size_t b) {
  const double w = 6.283185307179586 * hz / kRate, c = 2.0 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (size_t i = a; i < b && i < v.size(); i++) {
    const double s = v[i] + c * s1 - s2;
    s2 = s1;
    s1 = s;
  }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / double(b - a);
}
bool allFinite(const std::vector<float>& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}
double maxStep(const std::vector<float>& v, size_t a, size_t b) {
  double m = 0;
  for (size_t i = a + 1; i < b && i < v.size(); i++) m = std::fmax(m, std::fabs(v[i] - v[i - 1]));
  return m;
}
const char* nameOf(int32_t kind) { return instrumentKindMeta(kind)->id; }

template <class E>
int32_t id(E p) { return static_cast<int32_t>(p); }

// --- generic, per kind ---

void testMetadata() {
  const char* ids[] = {"kick", "snare", "clap", "tom", "hihat"};
  for (int32_t k = 0; k < 5; k++) {
    const InstrumentKindMeta* m = instrumentKindMeta(k);
    CHECK(m != nullptr);
    CHECK(std::strcmp(m->id, ids[k]) == 0);
    CHECK(std::strlen(m->name) > 0);
    CHECK(std::strcmp(m->category, "drum") == 0);
    const int32_t n = kindParamCount(k);  // the kind's own table (the shared gain row follows it)
    CHECK(n >= 4 && n <= 6);
    for (int32_t i = 0; i < n; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(k, i);
      CHECK(p->id == i);
      CHECK(p->min < p->max && p->def >= p->min && p->def <= p->max);
    }
    auto inst = makeInstrument(k);
    CHECK(inst && inst->kind() == static_cast<InstrumentKind>(k));
  }
  CHECK(instrumentKindMeta(99) == nullptr);
  CHECK(instrumentParamCount(99) == 0);
  CHECK(makeInstrument(99) == nullptr);
}

void testAudibleFiniteBounded() {
  for (int32_t k : kAllPerc) {
    Out o = play(k, {}, 1.0, {}, nameOf(k));
    CHECK(peak(o.l) > 0.15f);
    CHECK(peak(o.l) <= 1.0f);
    CHECK(allFinite(o.l) && allFinite(o.r));
    // Mono source at centre pan => identical channels.
    CHECK(o.l == o.r);
    // Deterministic.
    CHECK(play(k, {}, 0.3).l == play(k, {}, 0.3).l);
    // It ends: silent well after the longest default tail.
    CHECK(peak(o.l, at(0.9)) < 1e-3f);
  }
}

void testExtremeParamsStayBounded() {
  for (int32_t k : kAllPerc) {
    const int32_t n = instrumentParamCount(k);
    for (int pass = 0; pass < 2; pass++) {
      Engine e;
      e.init(kRate, 2, 8);
      e.createInstrument(1, k, 0);
      for (int32_t i = 0; i < n; i++) {
        const InstrumentParamInfo* p = instrumentParamInfo(k, i);
        e.setInstrumentParam(1, p->id, pass ? p->max : p->min);
      }
      e.instrumentNoteOn(1, 1.0f, 0.0f, pass ? 36.0f : -36.0f);
      int64_t f = 0;
      Out o;
      render(e, f, at(0.8), &o);
      CHECK(allFinite(o.l));
      CHECK(peak(o.l) <= 1.0f);
    }
  }
}

void testParamsClampAndIgnoreGarbage() {
  for (int32_t k : kAllPerc) {
    Engine e;
    e.init(kRate, 2, 8);
    e.createInstrument(1, k, 0);
    e.setInstrumentParam(1, 0, 1e9f);
    e.setInstrumentParam(1, 0, std::nanf(""));
    e.setInstrumentParam(1, 99, 1.0f);  // unknown id: no-op
    e.instrumentNoteOn(1, 1.0f, 0.0f, std::nanf(""));
    int64_t f = 0;
    Out o;
    render(e, f, at(0.2), &o);
    CHECK(allFinite(o.l));
  }
}

void testVelocityScalesLevel() {
  for (int32_t k : kAllPerc) {
    const float full = peak(play(k, {}, 0.3, {1.0f, 0, 0}).l);
    const float half = peak(play(k, {}, 0.3, {0.25f, 0, 0}).l);
    CHECK(half < full);
    CHECK(half > 0.0f);
    CHECK(peak(play(k, {}, 0.1, {0.0f, 0, 0}).l) == 0.0f);
  }
}

void testPanning() {
  for (int32_t k : kAllPerc) {
    Out o = play(k, {}, 0.2, {1.0f, 1.0f, 0});
    CHECK(peak(o.r) > 4.0f * peak(o.l));
  }
}

void testLifecycleResetNoteOffAndActivity() {
  for (int32_t k : kAllPerc) {
    Engine e;
    e.init(kRate, 2, 8);
    e.createInstrument(1, k, 0);
    CHECK(e.activeInstrumentCount() == 0);
    e.instrumentNoteOn(1, 1.0f);
    int64_t f = 0;
    Out o;
    render(e, f, kBlock, &o);
    CHECK(e.activeInstrumentCount() == 1);
    // Choke: sound must end within a few ms (short release), well before the natural tail.
    e.setInstrumentParam(1, 1, 1.0f);  // longest decay for decay-at-id-1 kinds is fine either way
    e.instrumentNoteOff(1);
    Out tail;
    render(e, f, at(0.05), &tail);
    CHECK(peak(tail.l, at(0.04)) < 1e-3f);
    // Fully done => inactive again, and a re-trigger works after that.
    render(e, f, at(1.0), &tail);
    CHECK(e.activeInstrumentCount() == 0);
    e.instrumentNoteOn(1, 1.0f);
    Out again;
    render(e, f, kBlock, &again);
    CHECK(peak(again.l) > 0.0f);
    // Remove while sounding is safe.
    CHECK(e.removeInstrument(1));
  }
}

void testScheduledNoteIsSampleAccurate() {
  for (int32_t k : kAllPerc) {
    Engine e;
    e.init(kRate, 2, 8);
    e.createInstrument(1, k, 0);
    const int64_t at = 1000;
    e.scheduleInstrumentNote(at, 1, 1.0f, 0.0f, 0.0f);
    int64_t f = 0;
    Out o;
    render(e, f, 2048, &o);
    CHECK(peak(o.l, 0, static_cast<size_t>(at)) == 0.0f);
    CHECK(peak(o.l, static_cast<size_t>(at), static_cast<size_t>(at) + 400) > 0.0f);
  }
}

void testRetriggerIsContinuousAndRestarts() {
  for (int32_t k : kAllPerc) {
    Engine e;
    e.init(kRate, 2, 8);
    e.createInstrument(1, k, 0);
    e.instrumentNoteOn(1, 1.0f);
    int64_t f = 0;
    Out o;
    render(e, f, at(0.05), &o);
    const size_t join = o.l.size();
    e.instrumentNoteOn(1, 1.0f);
    render(e, f, at(0.2), &o);
    // No hard jump larger than what the steepest part of a normal hit produces.
    CHECK(maxStep(o.l, join - 2, join + 8) < 0.8);
    CHECK(rms(o.l, join, join + at(0.02)) > 0.01);
  }
}

void testRenderDoesNotAllocate() {
  for (int32_t k : kAllPerc) {
    Engine e;
    e.init(kRate, 2, 8);
    e.createInstrument(1, k, 0);
    e.instrumentNoteOn(1, 1.0f);
    e.process(0, kBlock);
    const long before = g_allocs;
    int64_t f = kBlock;
    for (int i = 0; i < 400; i++) {
      if (i % 50 == 0) e.instrumentNoteOn(1, 0.8f, 0.3f, 2.0f);
      e.setInstrumentParam(1, 0, instrumentParamInfo(k, 0)->def);
      e.process(f, kBlock);
      f += kBlock;
    }
    CHECK(g_allocs == before);
  }
}

// --- decay behaves broadly right (every kind's `decay` is parameter index 1 except Tom's 2) ---

void testDecayLengthensTail() {
  struct C { int32_t kind, decayId; float shortV, longV; double winA, winB; };
  const C cases[] = {
      {kSnare, id(SnareParam::Decay), 0.08f, 0.6f, 0.15, 0.45},
      {kClap, id(ClapParam::Decay), 0.08f, 0.6f, 0.15, 0.45},
      {kTom, id(TomParam::Decay), 0.15f, 1.2f, 0.3, 0.8},
      {kHat, id(HiHatParam::Decay), 0.03f, 0.6f, 0.1, 0.4},
  };
  for (const C& c : cases) {
    const Out s = play(c.kind, {{c.decayId, c.shortV}}, 1.0);
    const Out l = play(c.kind, {{c.decayId, c.longV}}, 1.0);
    const double rs = rms(s.l, at(c.winA), at(c.winB));
    const double rl = rms(l.l, at(c.winA), at(c.winB));
    CHECK(rl > 8.0 * rs);
  }
}

// --- instrument-specific ---

void testTomPitchFollowsTuneAndNote() {
  auto freq = [](float tune, float pitch) {
    Out o = play(kTom, {{id(TomParam::Tune), tune}, {id(TomParam::PitchDrop), 0.0f},
                         {id(TomParam::Body), 0.0f}, {id(TomParam::Damp), 0.0f},
                         {id(TomParam::Decay), 1.0f}},
                 0.6, {1.0f, 0, pitch});
    return zcFreq(o.l, at(0.05), at(0.45));
  };
  CHECK_NEAR(freq(100.0f, 0.0f), 100.0, 4.0);
  CHECK_NEAR(freq(200.0f, 0.0f), 200.0, 6.0);
  // Note pitch is a semitone offset on Tune: +12 doubles, -12 halves.
  CHECK_NEAR(freq(100.0f, 12.0f), 200.0, 6.0);
  CHECK_NEAR(freq(200.0f, -12.0f), 100.0, 4.0);
  CHECK_NEAR(freq(100.0f, 7.0f), 100.0 * std::exp2(7.0 / 12.0), 5.0);
}

void testTomPitchDropSweepsDownward() {
  Out o = play(kTom, {{id(TomParam::PitchDrop), 2.0f}, {id(TomParam::Body), 0.0f},
                       {id(TomParam::Decay), 1.0f}, {id(TomParam::Damp), 0.0f}},
               0.6);
  const double early = zcFreq(o.l, at(0.002), at(0.03));
  const double late = zcFreq(o.l, at(0.3), at(0.5));
  CHECK(early > 1.8 * late);
}

void testTomDampDarkensAndShortens() {
  Out open = play(kTom, {{id(TomParam::Damp), 0.0f}, {id(TomParam::Body), 1.0f}}, 1.0);
  Out damped = play(kTom, {{id(TomParam::Damp), 1.0f}, {id(TomParam::Body), 1.0f}}, 1.0);
  CHECK(rms(damped.l, at(0.3), at(0.6)) < 0.5 * rms(open.l, at(0.3), at(0.6)));
  CHECK(zcFreq(damped.l, 0, at(0.05)) < zcFreq(open.l, 0, at(0.05)));
}

void testKickNotePitchOffset() {
  auto freq = [](float pitch) {
    Out o = play(kKick, {{id(KickParam::Tune), 60.0f}, {id(KickParam::PitchDrop), 0.0f},
                          {id(KickParam::Punch), 0.0f}, {id(KickParam::Drive), 0.0f},
                          {id(KickParam::Decay), 1.0f}},
                 0.6, {1.0f, 0, pitch});
    return zcFreq(o.l, at(0.05), at(0.45));
  };
  CHECK_NEAR(freq(0.0f), 60.0, 3.0);
  CHECK_NEAR(freq(12.0f), 120.0, 5.0);
}

void testSnareParams() {
  // Tune moves the body: each hit holds more energy at its own tune than at the other's.
  auto hit = [](float tune) {
    return play(kSnare, {{id(SnareParam::Tune), tune}, {id(SnareParam::Noise), 0.0f},
                          {id(SnareParam::Snap), 0.0f}, {id(SnareParam::Decay), 0.5f}},
                0.3);
  };
  const Out low = hit(130.0f), high = hit(300.0f);
  CHECK(tonePower(low.l, 130.0, at(0.01), at(0.1)) > 2.0 * tonePower(high.l, 130.0, at(0.01), at(0.1)));
  CHECK(tonePower(high.l, 300.0, at(0.01), at(0.1)) > 2.0 * tonePower(low.l, 300.0, at(0.01), at(0.1)));
  // Noise raises the level of the noisy tail relative to no-noise.
  Out quiet = play(kSnare, {{id(SnareParam::Noise), 0.0f}}, 0.5);
  Out noisy = play(kSnare, {{id(SnareParam::Noise), 1.0f}}, 0.5);
  CHECK(rms(noisy.l, at(0.1), at(0.25)) > 1.5 * rms(quiet.l, at(0.1), at(0.25)));
  // Snap brightens the opening transient.
  Out soft = play(kSnare, {{id(SnareParam::Snap), 0.0f}}, 0.1);
  Out snappy = play(kSnare, {{id(SnareParam::Snap), 1.0f}}, 0.1);
  CHECK(rms(snappy.l, 0, at(0.01)) > rms(soft.l, 0, at(0.01)));
  CHECK(zcFreq(snappy.l, 0, at(0.05)) > zcFreq(soft.l, 0, at(0.05)));
}

// Number of separate energy bursts in the first 80 ms, from a 1 ms rectified-RMS envelope.
int countBursts(const std::vector<float>& v) {
  const size_t w = at(0.001);
  std::vector<double> env;
  for (size_t a = 0; a + w <= at(0.08); a += w) env.push_back(rms(v, a, a + w));
  double mx = 0;
  for (double e : env) mx = std::fmax(mx, e);
  // Smooth over 3 windows, then count rises that follow a real dip.
  int bursts = 0;
  double valley = 1e9;
  bool armed = true;
  for (size_t i = 1; i + 1 < env.size(); i++) {
    const double s = (env[i - 1] + env[i] + env[i + 1]) / 3.0;
    valley = std::fmin(valley, s);
    if (armed && s > 0.35 * mx && s > 1.35 * valley) {
      bursts++;
      armed = false;
    }
    if (!armed && s < 0.8 * valley + 0.0) armed = false;
    if (!armed && s < 0.6 * mx) { armed = true; valley = s; }
  }
  return bursts;
}

void testClapHasMultipleTransientsAndSpreadMovesThem() {
  Out wide = play(kClap, {{id(ClapParam::Spread), 1.0f}, {id(ClapParam::Decay), 0.3f}}, 0.2);
  Out tight = play(kClap, {{id(ClapParam::Spread), 0.0f}, {id(ClapParam::Decay), 0.3f}}, 0.2);
  CHECK(countBursts(wide.l) >= 3);
  // Tail (the loudest, longest burst) starts later when spread is larger: energy in the
  // 20-30 ms window is lower for tight (tail already decaying) than for wide.
  auto firstLoud = [](const std::vector<float>& v) {
    const float p = peak(v, 0, at(0.2));
    for (size_t i = 0; i < v.size(); i++) if (std::fabs(v[i]) > 0.9f * p) return i;
    return v.size();
  };
  CHECK(firstLoud(wide.l) > firstLoud(tight.l));
}

void testClapTuneMovesBand() {
  Out lo = play(kClap, {{id(ClapParam::Tune), 700.0f}}, 0.2);
  Out hi = play(kClap, {{id(ClapParam::Tune), 2800.0f}}, 0.2);
  CHECK(zcFreq(hi.l, 0, at(0.15)) > 1.5 * zcFreq(lo.l, 0, at(0.15)));
}

void testHatOpenVsClosedAndMetal() {
  Out closed = play(kHat, {{id(HiHatParam::Decay), 0.04f}}, 0.6);
  Out open = play(kHat, {{id(HiHatParam::Decay), 0.8f}}, 0.6);
  CHECK(rms(open.l, at(0.2), at(0.5)) > 10.0 * rms(closed.l, at(0.2), at(0.5)));
  // Bright: mostly energy above 3 kHz (zero crossings are a coarse proxy).
  CHECK(zcFreq(closed.l, 0, at(0.05)) > 3000.0);
  // Metal changes the sound (cluster vs noise) rather than just the level.
  Out noisy = play(kHat, {{id(HiHatParam::Metal), 0.0f}}, 0.3);
  Out metal = play(kHat, {{id(HiHatParam::Metal), 1.0f}}, 0.3);
  CHECK(noisy.l != metal.l);
  // Tone: higher is brighter.
  Out dark = play(kHat, {{id(HiHatParam::Tone), 0.0f}}, 0.2);
  Out bright = play(kHat, {{id(HiHatParam::Tone), 1.0f}}, 0.2);
  CHECK(zcFreq(bright.l, 0, at(0.1)) > zcFreq(dark.l, 0, at(0.1)));
  // Note pitch / tune shift the cluster.
  Out lowT = play(kHat, {{id(HiHatParam::Tune), -12.0f}, {id(HiHatParam::Metal), 1.0f}}, 0.2);
  Out highT = play(kHat, {{id(HiHatParam::Tune), 12.0f}, {id(HiHatParam::Metal), 1.0f}}, 0.2);
  CHECK(zcFreq(highT.l, 0, at(0.1)) > zcFreq(lowT.l, 0, at(0.1)));
}

void testOpenHatIsChokedByClosedRetrigger() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kHat, 0);
  e.setInstrumentParam(1, id(HiHatParam::Decay), 1.0f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0;
  Out o;
  render(e, f, at(0.1), &o);
  e.setInstrumentParam(1, id(HiHatParam::Decay), 0.03f);
  e.instrumentNoteOn(1, 1.0f);
  render(e, f, at(0.4), &o);
  CHECK(rms(o.l, at(0.3), at(0.5)) < 0.02 * rms(o.l, at(0.05), at(0.1)));
}

}  // namespace

int main() {
  testMetadata();
  testAudibleFiniteBounded();
  testExtremeParamsStayBounded();
  testParamsClampAndIgnoreGarbage();
  testVelocityScalesLevel();
  testPanning();
  testLifecycleResetNoteOffAndActivity();
  testScheduledNoteIsSampleAccurate();
  testRetriggerIsContinuousAndRestarts();
  testRenderDoesNotAllocate();
  testDecayLengthensTail();
  testTomPitchFollowsTuneAndNote();
  testTomPitchDropSweepsDownward();
  testTomDampDarkensAndShortens();
  testKickNotePitchOffset();
  testSnareParams();
  testClapHasMultipleTransientsAndSpreadMovesThem();
  testClapTuneMovesBand();
  testHatOpenVsClosedAndMetal();
  testOpenHatIsChokedByClosedRetrigger();
  std::printf("%d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
