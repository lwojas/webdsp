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

// Covers Edge (instruments/edge.h, dsp/edge_osc.h, dsp/dual_filter.h) through the Engine's
// instrument API plus the two new DSP blocks directly: registration, parameter table and
// propagation, lifecycle, the band-limited oscillator (harmonic count, shape, alias floor, constant
// loudness), the pitch transient, the tone envelope, detune, every filter mode and routing, the
// series-resonance headroom rule, stability under abrupt cutoff moves, bounds on random extreme
// patches, and allocation. DSP checks are sanity measurements (zero-crossing pitch, spectral bins,
// brightness), never exact samples.
// Set EDGE_WAV_DIR=<dir> to dump the cases as 16-bit WAVs for listening (docs/edge-instrument.md).
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
constexpr int32_t kEd = static_cast<int32_t>(InstrumentKind::Edge);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
using P = EdgeParam;
int32_t id(P p) { return static_cast<int32_t>(p); }
size_t at(double s) { return static_cast<size_t>(s * kRate); }

void run(Engine& e, int64_t& frame, double seconds, Buf* o) {
  const int32_t frames = static_cast<int32_t>(std::ceil(seconds * kRate / kBlock)) * kBlock;
  for (int32_t d = 0; d < frames; d += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    o->insert(o->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
}

void writeWav(const char* name, const Buf& b) {
  const char* dir = std::getenv("EDGE_WAV_DIR");
  if (!dir) return;
  std::string path = std::string(dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(b.size()), bytes = n * 2, rate = 48000;
  const uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
  const uint32_t riff = 36 + bytes, fmt = 16, byteRate = rate * 2;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    const int16_t v = static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, b[i])) * 32767);
    std::fwrite(&v, 2, 1, f);
  }
  std::fclose(f);
}

struct Rig {
  Engine e;
  int64_t f = 0;
  Buf o;
  explicit Rig(int32_t bus = 0) { e.init(kRate, 2, 8); e.createInstrument(1, kEd, bus); }
  void set(P p, float v, int32_t inst = 1) { e.setInstrumentParam(inst, id(p), v); }
  void go(double s) { run(e, f, s, &o); }
};

float peak(const Buf& v, size_t a = 0, size_t b = SIZE_MAX) {
  float p = 0;
  for (size_t i = a; i < v.size() && i < b; i++) p = std::fmax(p, std::fabs(v[i]));
  return p;
}
double rms(const Buf& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; i++) s += double(v[i]) * v[i];
  return std::sqrt(s / double(b - a));
}
double bin(const Buf& v, size_t a, size_t b, double hz) {
  double re = 0, im = 0;
  b = std::min(b, v.size());
  const double n = double(b - a);
  for (size_t i = a; i < b; i++) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * double(i - a) / n);
    const double ph = 2 * kPi * hz * double(i) / kRate;
    re += w * v[i] * std::cos(ph);
    im -= w * v[i] * std::sin(ph);
  }
  return std::sqrt(re * re + im * im) * 2.0 / n;
}
// First-difference rms over rms: a cheap brightness measure (grows with high-frequency content).
double bright(const Buf& v, size_t a, size_t b) {
  double d = 0, s = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) { const double x = v[i] - v[i - 1]; d += x * x; s += double(v[i]) * v[i]; }
  return std::sqrt(d / std::fmax(s, 1e-12));
}
bool finite(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}
double mean(const Buf& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; i++) s += v[i];
  return s / double(b - a);
}
// Rising zero crossings per second inside [a, b): the pitch of a simple tone.
double crossingHz(const Buf& v, size_t a, size_t b) {
  int n = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) if (v[i - 1] < 0.0f && v[i] >= 0.0f) n++;
  return n / (double(b - a) / kRate);
}
double db(double x) { return 20.0 * std::log10(std::fmax(x, 1e-12)); }

// Reference patch for measurements: held, flat, filter wide open and bypassed (single stage, LP 12
// at 16 kHz, no envelopes, no transient). Each test changes only what it examines.
using Params = std::vector<std::pair<P, float>>;
const Params kBase = {{P::Tune, 110.0f}, {P::Shape, 0.0f}, {P::Bright, 1.0f}, {P::BrightEnv, 0.0f},
                      {P::Detune, 0.0f}, {P::PitchEnv, 0.0f}, {P::Routing, 0.0f}, {P::FilterA, 0.0f},
                      {P::Cutoff, 16000.0f}, {P::Resonance, 0.0f}, {P::FilterEnv, 0.0f},
                      {P::LfoPitch, 0.0f}, {P::LfoFilter, 0.0f}, {P::Attack, 0.001f}, {P::Decay, 1.0f},
                      {P::Sustain, 1.0f}};

Buf render(Params params, double seconds = 1.0, float pitch = 0.0f, float velocity = 1.0f,
           const Params& base = kBase) {
  Rig r;
  for (auto& [p, v] : base) r.set(p, v);
  for (auto& [p, v] : params) r.set(p, v);
  r.e.instrumentNoteOn(1, velocity, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}

// ---- registration, table, validation ---------------------------------------------------------

void testRegistrationAndDefaults() {
  const InstrumentKindMeta* m = instrumentKindMeta(kEd);
  CHECK(m && std::strcmp(m->id, "edge") == 0 && std::strcmp(m->name, "Edge") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(kEd == 17 && instrumentParamCount(kEd) == kEdgeParamCount && kEdgeParamCount == 22);
  auto in = makeInstrument(kEd);
  CHECK(in && in->kind() == InstrumentKind::Edge && !in->isActive());
  for (int32_t i = 0; i < kEdgeParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kEd, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max && p->min < p->max);
  }
  CHECK(instrumentParamInfo(kEd, kEdgeParamCount) == nullptr);
  auto find = [](const char* n) { for (int32_t i = 0; i < kEdgeParamCount; i++) if (!std::strcmp(kEdgeParams[i].name, n)) return &kEdgeParams[i]; return (const InstrumentParamInfo*)nullptr; };
  CHECK_NEAR(find("tune")->def, 220.0, 1e-6);
  CHECK(find("sustain")->def == 0.0f);                       // a one-shot out of the box
  CHECK(find("resonance")->def <= 0.2f);                     // not a resonance-driven default
  CHECK(find("routing")->max == 2.0f && find("filtera")->max == 3.0f && find("filterb")->max == 3.0f);
  CHECK(find("pitchenv")->min < 0.0f && find("pitchenv")->max > 0.0f);   // signed transient
  CHECK(resolveModDomain(*find("cutoff")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("pitchdecay")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("spread")) == ModDomain::Linear && find("spread")->min < 0.0f);
  CHECK(resolveModDomain(*find("routing")) == ModDomain::None);    // switches are not LFO targets
  CHECK(resolveModDomain(*find("filtera")) == ModDomain::None);
  CHECK(resolveModDomain(*find("filterb")) == ModDomain::None);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kEd, 0));
}

// Every parameter reaches the DSP: moving it off its default changes the rendered note. The patch
// exercises all stages so no parameter is inert for lack of signal.
void testEveryParameterPropagates() {
  const Params patch = {{P::Detune, 15.0f}, {P::Shape, 0.3f}, {P::LfoPitch, 0.3f}, {P::LfoFilter, 0.3f},
                        {P::Sustain, 0.5f}, {P::Resonance, 0.3f}, {P::Cutoff, 2500.0f}};
  Rig ref;
  for (auto& [p, v] : patch) ref.set(p, v);
  ref.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
  ref.go(0.6);
  for (int32_t i = 0; i < kEdgeParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kEd, i);
    float moved = p->def;
    for (const auto& [pp, vv] : patch) if (id(pp) == i) moved = vv;
    const float target = (std::fabs(moved - p->min) > std::fabs(moved - p->max)) ? p->min : p->max;
    Rig r;
    for (auto& [pp, vv] : patch) r.set(pp, vv);
    r.e.setInstrumentParam(1, i, target);
    r.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
    r.go(0.6);
    CHECK(finite(r.o));
    if (r.o == ref.o) std::fprintf(stderr, "param %s has no effect\n", p->name);
    CHECK(r.o != ref.o);
  }
}

void testParamValidation() {
  auto in = makeInstrument(kEd);
  in->configure(kRate);
  in->setParam(id(P::Cutoff), 1e9f);     // clamped, not crashed
  in->setParam(id(P::Cutoff), std::nanf(""));   // ignored
  in->setParam(id(P::Routing), -5.0f);
  in->setParam(9999, 1.0f);              // unknown id: no-op
  in->noteOn(1.0f, 0, 0.0f, 0.0f);
  float a[kBlock] = {}, b[kBlock] = {}, s0[kBlock] = {}, s1[kBlock] = {};
  float* bus[2] = {a, b};
  float* scratch[2] = {s0, s1};
  CHECK(in->render(bus, 2, kBlock, scratch));
  for (int i = 0; i < kBlock; i++) CHECK(std::isfinite(a[i]));
}

void testLifecycle() {
  // A one-shot (sustain 0) ends by itself and the instance goes idle.
  {
    Rig r;
    r.set(P::Decay, 0.1f);
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
    r.go(0.05);
    CHECK(peak(r.o) > 0.1f);
    r.go(0.6);
    CHECK(peak(r.o, at(0.5)) < 1e-5f);
  }
  // Sustained: holds until note-off, then releases to silence within about `release`.
  {
    Rig r;
    r.set(P::Sustain, 1.0f);
    r.set(P::Release, 0.1f);
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
    r.go(0.5);
    const double held = rms(r.o, at(0.3), at(0.5));
    CHECK(held > 0.05);
    r.e.instrumentNoteOff(1);
    r.go(0.3);
    CHECK(rms(r.o, at(0.5), at(0.55)) > 0.2 * held);   // still ringing out at the start
    CHECK(peak(r.o, at(0.7)) < 1e-4f);
  }
  // A timed duration ends a sustained note by itself.
  {
    Rig r;
    r.set(P::Sustain, 1.0f);
    r.set(P::Release, 0.05f);
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.2 * kRate));
    r.go(0.6);
    CHECK(rms(r.o, at(0.1), at(0.2)) > 0.05);
    CHECK(peak(r.o, at(0.35)) < 1e-4f);
  }
}

void testDeterministicAndRetrigger() {
  const Params p = {{P::Detune, 12.0f}, {P::Shape, 0.4f}, {P::Sustain, 0.4f}, {P::LfoPitch, 0.4f}};
  const Buf a = render(p, 0.4), b = render(p, 0.4);
  CHECK(a == b);
  // A retrigger on a sounding note is continuous: no large step at the retrigger frame.
  Rig r;
  r.set(P::Sustain, 1.0f);
  r.set(P::Bright, 0.4f);
  r.set(P::PitchEnv, 0.0f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(0.3);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(0.3);
  float worst = 0;
  for (size_t i = at(0.3) - 8; i < at(0.3) + 8; i++) worst = std::fmax(worst, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(worst < 0.35f);
  CHECK(finite(r.o));
}

void testVelocity() {
  const Params p = {{P::Sustain, 0.0f}, {P::Decay, 0.5f}};
  const Buf lo = render(p, 0.3, 0.0f, 0.3f), hi = render(p, 0.3, 0.0f, 1.0f);
  CHECK(peak(hi) > peak(lo) * 1.5f);
  // A hard hit is a little brighter than a soft one at the same level scaling.
  const Params q = {{P::BrightEnv, 0.5f}, {P::Bright, 0.5f}, {P::Decay, 0.5f}};
  const Buf s = render(q, 0.2, 0.0f, 0.3f), h = render(q, 0.2, 0.0f, 1.0f);
  CHECK(bright(h, 0, at(0.05)) > bright(s, 0, at(0.05)));
}

void testNotePitchSetsBase() {
  const Params p = {{P::Bright, 0.0f}};   // 1 harmonic: a sine
  const Buf a = render(p, 0.5), b = render(p, 0.5, 12.0f), c = render(p, 0.5, -12.0f);
  CHECK_NEAR(crossingHz(a, at(0.1), at(0.5)), 110.0, 3.5);
  CHECK_NEAR(crossingHz(b, at(0.1), at(0.5)), 220.0, 4.5);
  CHECK_NEAR(crossingHz(c, at(0.1), at(0.5)), 55.0, 3.5);
}

// ---- oscillator ------------------------------------------------------------------------------

void testOscillatorTables() {
  using namespace edge;
  const Tables& t = tables();
  for (int32_t j = 0; j < kLevels; j++) {
    const float* tb = t.level(j);
    double e = 0, mean = 0;
    for (int32_t i = 0; i < kTableSize; i++) { e += double(tb[i]) * tb[i]; mean += tb[i]; }
    CHECK_NEAR(std::sqrt(e / kTableSize), kTargetRms, 1e-3);   // constant RMS across brightness
    CHECK_NEAR(mean / kTableSize, 0.0, 1e-4);                  // zero mean
    CHECK(tb[kTableSize] == tb[0]);                            // guard point
    // No energy above the level's harmonic count (table DFT).
    const int32_t h = kLevelHarmonics[j];
    for (int32_t n = h + 1; n <= std::min(h + 6, kTableSize / 2 - 1); n++) {
      double re = 0, im = 0;
      for (int32_t i = 0; i < kTableSize; i++) {
        re += tb[i] * std::cos(2 * kPi * n * i / kTableSize);
        im += tb[i] * std::sin(2 * kPi * n * i / kTableSize);
      }
      CHECK(std::sqrt(re * re + im * im) * 2.0 / kTableSize < 1e-3);
    }
  }
}

void testHarmonicCountAndShape() {
  // Bright 0: a sine. Bright 1: a saw with 1/n harmonics. Square (shape 0.5): odd harmonics only.
  const Buf sine = render({{P::Bright, 0.0f}}, 0.6);
  CHECK(bin(sine, at(0.1), at(0.6), 220.0) < 0.01 * bin(sine, at(0.1), at(0.6), 110.0));
  const Buf saw = render({{P::Bright, 1.0f}}, 0.6);
  const double s1 = bin(saw, at(0.1), at(0.6), 110.0);
  CHECK_NEAR(bin(saw, at(0.1), at(0.6), 220.0) / s1, 0.5, 0.06);
  CHECK_NEAR(bin(saw, at(0.1), at(0.6), 330.0) / s1, 1.0 / 3.0, 0.05);
  const Buf sq = render({{P::Bright, 1.0f}, {P::Shape, 0.5f}}, 0.6);
  const double q1 = bin(sq, at(0.1), at(0.6), 110.0);
  CHECK(bin(sq, at(0.1), at(0.6), 220.0) < 0.03 * q1);           // even harmonics gone
  CHECK_NEAR(bin(sq, at(0.1), at(0.6), 330.0) / q1, 1.0 / 3.0, 0.05);
  // Narrow pulse is brighter than a square, which is brighter than a mid-bright saw.
  const Buf pulse = render({{P::Bright, 1.0f}, {P::Shape, 1.0f}}, 0.6);
  CHECK(bright(pulse, at(0.1), at(0.6)) > bright(sq, at(0.1), at(0.6)) * 0.9);
  // Brightness grows monotonically with the knob.
  double last = 0;
  for (float b : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const Buf x = render({{P::Bright, b}}, 0.4);
    const double br = bright(x, at(0.1), at(0.4));
    CHECK(br > last);
    last = br;
  }
}

// Sweeping brightness or shape changes the timbre, not the loudness.
void testConstantLoudness() {
  double lo = 1e9, hi = 0;
  for (float b : {0.0f, 0.3f, 0.6f, 1.0f}) {
    for (float s : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      const double r = rms(render({{P::Bright, b}, {P::Shape, s}}, 0.4), at(0.1), at(0.4));
      lo = std::fmin(lo, r);
      hi = std::fmax(hi, r);
    }
  }
  CHECK(hi / lo < 1.6);
}

// Band-limiting: even at the brightest setting on a high note nothing folds back below the signal.
void testAliasFloor() {
  const double f = 1760.0 * std::exp2(9.7 / 12.0);   // ~3100 Hz: Tune tops out at 1760, the note adds the rest
  const float note = 9.7f;
  const Buf x = render({{P::Tune, 1760.0f}, {P::Bright, 1.0f}}, 0.6, note);
  const double fund = bin(x, at(0.1), at(0.6), f);
  CHECK(fund > 0.05);
  // Aliases of the 8th, 9th and 10th harmonics (24.8k, 27.9k, 31k) land at these frequencies.
  for (double alias : {23200.0, 20100.0, 17000.0, 14000.0}) {
    CHECK(db(bin(x, at(0.1), at(0.6), alias) / fund) < -55.0);
  }
  // Same on a fast pitch transient (the cap follows the highest pitch of each step).
  const Buf y = render({{P::Tune, 1760.0f}, {P::Bright, 1.0f}, {P::PitchEnv, 12.0f}, {P::PitchDecay, 0.2f}}, 0.4, note);
  CHECK(finite(y) && peak(y) <= 1.0001f);
}

// ---- pitch transient, tone envelope, detune, LFO ---------------------------------------------

void testPitchTransient() {
  const Params base = {{P::Tune, 220.0f}, {P::Bright, 0.0f}, {P::PitchDecay, 0.05f}};
  auto with = [&](float st) { Params p = base; p.push_back({P::PitchEnv, st}); return render(p, 0.6); };
  const Buf up = with(12.0f), down = with(-12.0f), none = with(0.0f);
  CHECK(crossingHz(up, 0, at(0.02)) > 1.25 * 220.0);                    // starts well above
  CHECK(crossingHz(down, 0, at(0.02)) < 0.85 * 220.0);                  // starts below
  CHECK_NEAR(crossingHz(none, 0, at(0.02)), 220.0, 30.0);
  for (const Buf* b : {&up, &down}) CHECK_NEAR(crossingHz(*b, at(0.4), at(0.6)), 220.0, 6.0);   // lands on pitch
  // Faster decay lands sooner.
  Params fast = base; fast[2].second = 0.005f; fast.push_back({P::PitchEnv, 12.0f});
  Params slow = base; slow[2].second = 0.4f; slow.push_back({P::PitchEnv, 12.0f});
  CHECK(crossingHz(render(slow, 0.3), at(0.03), at(0.1)) > 1.15 * crossingHz(render(fast, 0.3), at(0.03), at(0.1)));
}

void testToneEnvelope() {
  // Brightness envelope alone (filter open and bypassed): a bright onset that settles.
  const Params env = {{P::Bright, 0.45f}, {P::BrightEnv, 0.55f}, {P::ToneDecay, 0.08f}};
  const Buf a = render(env, 0.6);
  CHECK(bright(a, 0, at(0.03)) > 1.6 * bright(a, at(0.4), at(0.6)));
  // Negative depth does the opposite.
  Params neg = env; neg[1].second = -0.4f; neg[0].second = 0.85f;
  const Buf b = render(neg, 0.6);
  CHECK(bright(b, 0, at(0.03)) < 0.8 * bright(b, at(0.4), at(0.6)));
  // The filter envelope does the same job through the filter (LP at 600 Hz opened by 4 octaves).
  const Params fe = {{P::Cutoff, 600.0f}, {P::FilterEnv, 0.7f}, {P::ToneDecay, 0.08f}};
  const Buf c = render(fe, 0.6);
  CHECK(bright(c, 0, at(0.03)) > 1.5 * bright(c, at(0.4), at(0.6)));
  // Longer tone decay keeps the brightness longer.
  Params slow = env; slow[2].second = 0.5f;
  CHECK(bright(render(slow, 0.6), at(0.15), at(0.25)) > bright(a, at(0.15), at(0.25)));
}

void testDetune() {
  const Params hiTune = {{P::Tune, 880.0f}, {P::Bright, 0.0f}};
  Params wide = hiTune; wide.push_back({P::Detune, 50.0f});
  const Buf one = render(hiTune, 0.6), two = render(wide, 0.6);
  const double f2 = 880.0 * std::pow(2.0, 50.0 / 1200.0);
  CHECK(bin(two, at(0.1), at(0.6), f2) > 0.3 * bin(two, at(0.1), at(0.6), 880.0));
  CHECK(bin(one, at(0.1), at(0.6), f2) < 0.05 * bin(one, at(0.1), at(0.6), 880.0));
  CHECK(peak(two) <= 1.0001f && rms(two, at(0.1), at(0.6)) < 1.6 * rms(one, at(0.1), at(0.6)));
  // The second oscillator fades in: 2 cents is a quiet shimmer, not a doubled level.
  const Buf small = render({{P::Bright, 0.0f}, {P::Detune, 2.0f}}, 0.6);
  CHECK(rms(small, at(0.1), at(0.6)) < 1.5 * rms(one, at(0.1), at(0.6)));
}

void testLfo() {
  // Vibrato: frequency wobbles at the LFO rate, centred on pitch.
  const Params v = {{P::Bright, 0.0f}, {P::LfoPitch, 1.0f}, {P::LfoRate, 4.0f}};
  const Buf a = render(v, 1.0);
  double lo = 1e9, hi = 0;
  for (size_t s = at(0.1); s + at(0.06) < a.size(); s += at(0.03)) {
    const double hz = crossingHz(a, s, s + at(0.06));
    lo = std::fmin(lo, hz);
    hi = std::fmax(hi, hz);
  }
  CHECK(hi > 110.0 * 1.04 && lo < 110.0 * 0.96);              // about +-1.5 st at full depth
  // Filter LFO changes brightness over time at the LFO rate, not the pitch.
  const Params fl = {{P::Cutoff, 1500.0f}, {P::LfoFilter, 1.0f}, {P::LfoRate, 3.0f}};
  const Buf b = render(fl, 1.2);
  double bl = 1e9, bh = 0;
  for (size_t s = at(0.1); s + at(0.05) < b.size(); s += at(0.04)) {
    const double br = bright(b, s, s + at(0.05));
    bl = std::fmin(bl, br);
    bh = std::fmax(bh, br);
  }
  CHECK(bh > 1.6 * bl);
  CHECK_NEAR(crossingHz(render({{P::Bright, 0.0f}, {P::LfoFilter, 1.0f}}, 0.6), at(0.1), at(0.6)), 110.0, 2.0);
}

// ---- filters: the block itself ----------------------------------------------------------------

// Steady-state gain of the DualFilter at `hz`.
double gainAt(DualFilter& f, double hz) {
  f.reset();
  const int32_t total = static_cast<int32_t>(0.4 * kRate);
  double acc = 0, ref = 0;
  float buf[16];
  double ph = 0;
  for (int32_t s = 0; s < total; s += 16) {
    for (int k = 0; k < 16; k++) { buf[k] = static_cast<float>(0.5 * std::sin(ph)); ph += 2 * kPi * hz / kRate; }
    float in[16];
    for (int k = 0; k < 16; k++) in[k] = buf[k];
    f.process(buf, 16);
    if (s > total / 2) for (int k = 0; k < 16; k++) { acc += double(buf[k]) * buf[k]; ref += double(in[k]) * in[k]; }
  }
  return std::sqrt(acc / ref);
}

DualFilter makeFilter(int32_t routing, int32_t a, int32_t b, float hzA, float hzB, float res = 0.0f) {
  DualFilter f;
  f.configure(kRate);
  f.setRouting(routing);
  f.setModes(a, b);
  f.setResonance(res);
  for (int i = 0; i < 40; i++) f.setCutoffs(hzA, hzB);   // cutoff moves are rate-limited per call: converge
  return f;
}

void testFilterModes() {
  // 12 dB low-pass: -3 dB at the cutoff (flat Q), about -12 dB an octave up.
  DualFilter lp12 = makeFilter(0, 0, 0, 1000, 1000);
  CHECK_NEAR(gainAt(lp12, 1000), 0.7071, 0.03);
  CHECK(db(gainAt(lp12, 2000)) < -10.0 && db(gainAt(lp12, 2000)) > -15.0);
  CHECK(db(gainAt(lp12, 100)) > -0.5);
  // 24 dB is steeper than 12 dB, with the same cutoff level.
  DualFilter lp24 = makeFilter(0, 1, 0, 1000, 1000);
  CHECK_NEAR(gainAt(lp24, 1000), 0.7071, 0.04);
  CHECK(gainAt(lp24, 4000) < 0.4 * gainAt(lp12, 4000));
  // High-pass blocks lows, passes highs.
  DualFilter hp = makeFilter(0, 2, 0, 1000, 1000);
  CHECK(db(gainAt(hp, 100)) < -18.0 && db(gainAt(hp, 8000)) > -1.0);
  // Band-pass: the make-up gain (1.5 when widest) at the centre, falling either side.
  DualFilter bp = makeFilter(0, 3, 0, 1000, 1000);
  const double bc = gainAt(bp, 1000);
  CHECK_NEAR(bc, 1.5, 0.08);
  CHECK(gainAt(bp, 100) < 0.3 * bc && gainAt(bp, 10000) < 0.3 * bc);
  // Narrower with resonance (relative to its own centre), and louder at the centre to compensate.
  DualFilter bpq = makeFilter(0, 3, 0, 1000, 1000, 1.0f);
  const double qc = gainAt(bpq, 1000);
  CHECK(gainAt(bpq, 1500) / qc < gainAt(bp, 1500) / bc && qc > 3.0 * bc);
}

void testRouting() {
  // Series LP (4 kHz) then HP (500 Hz): a window.
  DualFilter win = makeFilter(1, 0, 2, 4000, 500);
  const double mid = gainAt(win, 1500), lowEnd = gainAt(win, 60), highEnd = gainAt(win, 16000);
  CHECK(mid > 0.8 && lowEnd < 0.05 * mid && highEnd < 0.1 * mid);
  // Series is the product of the two stages.
  DualFilter a = makeFilter(0, 0, 0, 4000, 4000), b = makeFilter(0, 2, 0, 500, 500);
  CHECK_NEAR(gainAt(win, 1500), gainAt(a, 1500) * gainAt(b, 1500), 0.03);
  // Parallel LP (500 Hz) + HP (4 kHz): a scoop in between, both ends pass.
  DualFilter sc = makeFilter(2, 0, 2, 500, 4000);
  const double dip = gainAt(sc, 1500);
  CHECK(dip < 0.5 * gainAt(sc, 100) && dip < 0.5 * gainAt(sc, 16000));
  // At equal cutoffs LP + HP is the classic notch: the sum is (1 + s^2) / D, zero at the cutoff.
  DualFilter notch = makeFilter(2, 0, 2, 1000, 1000);
  CHECK(gainAt(notch, 1000) < 0.03 && gainAt(notch, 100) > 0.4 && gainAt(notch, 10000) > 0.4);
  // ...for any resonance (Q only sets the width).
  DualFilter notchQ = makeFilter(2, 0, 2, 1000, 1000, 1.0f);
  CHECK(gainAt(notchQ, 1000) < 0.03);
  // Single ignores stage B entirely.
  DualFilter single = makeFilter(0, 0, 2, 4000, 100);
  DualFilter ref = makeFilter(0, 0, 0, 4000, 4000);
  CHECK_NEAR(gainAt(single, 1500), gainAt(ref, 1500), 1e-4);
}

// Series resonance is tied, so two stacked resonant stages do not multiply into a runaway peak.
void testSeriesResonanceHeadroom() {
  auto peakGain = [&](DualFilter& f) {
    double pk = 0;
    for (double hz = 500; hz <= 2000; hz *= 1.03) pk = std::fmax(pk, gainAt(f, hz));
    return pk * f.gainComp();
  };
  DualFilter one = makeFilter(0, 0, 0, 1000, 1000, 1.0f);
  DualFilter ser = makeFilter(1, 0, 0, 1000, 1000, 1.0f);
  DualFilter par = makeFilter(2, 0, 0, 1000, 1000, 1.0f);
  const double p1 = peakGain(one), ps = peakGain(ser), pp = peakGain(par);
  CHECK(p1 < 7.0 && ps < 7.0 && pp < 7.0);          // compensated peaks stay modest
  CHECK(ps < 1.6 * p1);                              // series is nowhere near Q x Q
  // Untied it would be Q_A * Q_B ~ 187 x; confirm the actual uncompensated peak is far below that.
  double raw = 0;
  for (double hz = 500; hz <= 2000; hz *= 1.03) raw = std::fmax(raw, gainAt(ser, hz));
  CHECK(raw < 0.5 * 13.7 * 13.7);
  CHECK(ser.gainComp() < one.gainComp());           // series compensates for both stages
  CHECK(par.gainComp() > ser.gainComp());           // parallel peaks add, they do not multiply
  // Band-pass stages need none.
  DualFilter bp = makeFilter(0, 3, 0, 1000, 1000, 1.0f);
  CHECK_NEAR(bp.gainComp(), 1.0, 1e-6);
}

// Abrupt cutoff moves at maximum resonance on every mode pair and routing stay finite and bounded.
void testFilterStability() {
  unsigned seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1 << 24); };
  for (int32_t routing = 0; routing < 3; routing++) {
    for (int32_t a = 0; a < 4; a++) {
      for (int32_t b = 0; b < 4; b++) {
        DualFilter f = makeFilter(routing, a, b, 1000, 1000, 1.0f);
        float worst = 0;
        float buf[16];
        for (int blk = 0; blk < 3000; blk++) {
          for (int k = 0; k < 16; k++) buf[k] = rnd() * 2.0f - 1.0f;
          const float ca = 30.0f * std::exp2(rnd() * 9.0f);
          f.setCutoffs(ca, ca * std::exp2((rnd() - 0.5f) * 8.0f));
          f.process(buf, 16);
          for (int k = 0; k < 16; k++) {
            if (!std::isfinite(buf[k])) { worst = 1e30f; break; }
            worst = std::fmax(worst, std::fabs(buf[k]));
          }
          if (worst > 1e20f) break;
        }
        if (!(worst < 1e4f)) std::fprintf(stderr, "unstable: routing %d modes %d/%d peak %g\n", routing, a, b, double(worst));
        CHECK(worst < 1e4f);
      }
    }
  }
}

// ---- filters inside the instrument -----------------------------------------------------------

void testInstrumentFilters() {
  const Params saw = {{P::Bright, 1.0f}, {P::Routing, 0.0f}};
  auto band = [&](const Buf& v, double hz) { return bin(v, at(0.1), at(0.6), hz); };
  const Buf open = render(saw, 0.6);
  // LP12 vs LP24 at the same cutoff: 24 is darker above it.
  const Buf l12 = render({{P::Bright, 1.0f}, {P::Cutoff, 800.0f}, {P::FilterA, 0.0f}}, 0.6);
  const Buf l24 = render({{P::Bright, 1.0f}, {P::Cutoff, 800.0f}, {P::FilterA, 1.0f}}, 0.6);
  CHECK(band(l12, 3300.0) < band(open, 3300.0) * 0.35);
  CHECK(band(l24, 3300.0) < band(l12, 3300.0) * 0.6);
  // HP removes the fundamental; BP keeps its neighbourhood only.
  const Buf hp = render({{P::Bright, 1.0f}, {P::Cutoff, 1500.0f}, {P::FilterA, 2.0f}}, 0.6);
  CHECK(band(hp, 110.0) < band(open, 110.0) * 0.05);
  const Buf bp = render({{P::Bright, 1.0f}, {P::Cutoff, 1100.0f}, {P::FilterA, 3.0f}}, 0.6);
  CHECK(band(bp, 1100.0) > band(bp, 110.0) * 0.5 && band(bp, 9900.0) < band(bp, 1100.0) * 0.2);
  CHECK(rms(bp, at(0.1), at(0.6)) > 0.25 * rms(l12, at(0.1), at(0.6)));    // not buried under the low-pass
  // Series window: lows and highs both removed.
  const Buf win = render({{P::Bright, 1.0f}, {P::Routing, 1.0f}, {P::FilterA, 0.0f}, {P::FilterB, 2.0f},
                          {P::Cutoff, 3000.0f}, {P::Spread, -3.0f}}, 0.6);
  CHECK(band(win, 110.0) < band(open, 110.0) * 0.1 && band(win, 1100.0) > band(win, 110.0));
  CHECK(band(win, 9900.0) < band(win, 1100.0) * 0.3);
  // Parallel scoop keeps both ends of a bright saw but dips the middle relative to single LP + HP.
  const Buf sc = render({{P::Bright, 1.0f}, {P::Routing, 2.0f}, {P::FilterA, 0.0f}, {P::FilterB, 2.0f},
                         {P::Cutoff, 400.0f}, {P::Spread, 3.0f}}, 0.6);
  CHECK(band(sc, 110.0) > band(sc, 1100.0) * 0.8 && band(sc, 9900.0) > band(sc, 1100.0) * 0.1);
  // Same oscillator and envelope, different filter modes: clearly different results.
  CHECK(l12 != l24 && hp != bp && win != sc);
  // Every mode and routing is audible at the default level (no silent combinations).
  for (int32_t r = 0; r < 3; r++) for (int32_t a = 0; a < 4; a++) for (int32_t b = 0; b < 4; b++) {
    const Buf x = render({{P::Bright, 0.8f}, {P::Routing, float(r)}, {P::FilterA, float(a)}, {P::FilterB, float(b)},
                          {P::Cutoff, 1500.0f}, {P::Spread, 0.0f}}, 0.4, 7.0f);
    CHECK(finite(x) && rms(x, at(0.1), at(0.4)) > 0.003 && peak(x) <= 1.0001f);
  }
}

void testSpreadMovesStageB() {
  const Params p = {{P::Bright, 1.0f}, {P::Routing, 1.0f}, {P::FilterA, 0.0f}, {P::FilterB, 2.0f}, {P::Cutoff, 6000.0f}};
  Params lowSpread = p, highSpread = p;
  lowSpread.push_back({P::Spread, -4.0f});
  highSpread.push_back({P::Spread, -1.0f});
  const Buf a = render(lowSpread, 0.5), b = render(highSpread, 0.5);
  // The higher the HP, the less low-mid energy survives.
  CHECK(bin(b, at(0.1), at(0.5), 880.0) < bin(a, at(0.1), at(0.5), 880.0) * 0.5);
}

void testResonanceStaysCleanAndLevelled() {
  // Max resonance on the default series pair is compensated: not much louder than no resonance.
  const Params p = {{P::Bright, 0.9f}, {P::Routing, 1.0f}, {P::FilterA, 0.0f}, {P::FilterB, 0.0f},
                    {P::Cutoff, 1200.0f}, {P::Spread, 0.0f}};
  Params hi = p; hi.push_back({P::Resonance, 1.0f});
  Params lo = p; lo.push_back({P::Resonance, 0.0f});
  const Buf a = render(hi, 0.6), b = render(lo, 0.6);
  CHECK(finite(a) && peak(a) <= 1.0001f);
  CHECK(rms(a, at(0.1), at(0.6)) < 3.0 * rms(b, at(0.1), at(0.6)));
}

// ---- the point of the machine ------------------------------------------------------------------

// A convincing pluck and stab exist with the filter wide open, no resonance and no drive stage.
void testSoundDesignTargets() {
  // Pluck from oscillator + pitch transient + bright-then-pure, filter bypassed and flat.
  const Params pluck = {{P::Tune, 330.0f}, {P::Bright, 0.35f}, {P::BrightEnv, 0.6f}, {P::ToneDecay, 0.07f},
                        {P::PitchEnv, 4.0f}, {P::PitchDecay, 0.012f}, {P::Decay, 0.18f}, {P::Sustain, 0.0f},
                        {P::Routing, 0.0f}, {P::Cutoff, 16000.0f}, {P::FilterEnv, 0.0f}, {P::Resonance, 0.0f}};
  const Buf a = render(pluck, 0.6, 0.0f, 1.0f, {});
  writeWav("pluck_no_filter", a);
  CHECK(peak(a) > 0.2f && peak(a) <= 1.0001f);
  CHECK(bright(a, 0, at(0.02)) > 1.8 * bright(a, at(0.12), at(0.25)));        // bite then pure
  CHECK(rms(a, at(0.3), at(0.4)) < 0.05 * rms(a, 0, at(0.05)));               // a short note
  // Stab: window filter, quick tone envelope, no resonance.
  const Params stab = {{P::Tune, 220.0f}, {P::Shape, 0.5f}, {P::Bright, 0.7f}, {P::Detune, 12.0f},
                       {P::Routing, 1.0f}, {P::FilterA, 1.0f}, {P::FilterB, 2.0f}, {P::Cutoff, 2500.0f},
                       {P::Spread, -2.5f}, {P::FilterEnv, 0.6f}, {P::ToneDecay, 0.12f}, {P::Resonance, 0.0f},
                       {P::Decay, 0.22f}, {P::Sustain, 0.0f}};
  const Buf s = render(stab, 0.6, 0.0f, 1.0f, {});
  writeWav("stab_window", s);
  CHECK(finite(s) && peak(s) > 0.15f && peak(s) <= 1.0001f);
  CHECK(bright(s, 0, at(0.04)) > 1.4 * bright(s, at(0.2), at(0.3)));
  // Lead: held, vibrato, hard pulse, detune.
  const Params lead = {{P::Tune, 440.0f}, {P::Shape, 0.8f}, {P::Bright, 0.85f}, {P::BrightEnv, 0.0f},
                       {P::Detune, 14.0f}, {P::PitchEnv, 1.0f}, {P::PitchDecay, 0.03f}, {P::Routing, 0.0f},
                       {P::Cutoff, 9000.0f}, {P::FilterEnv, 0.0f}, {P::LfoPitch, 0.25f}, {P::LfoRate, 5.5f},
                       {P::Attack, 0.01f}, {P::Sustain, 1.0f}, {P::Release, 0.1f}};
  const Buf l = render(lead, 0.8, 0.0f, 1.0f, {});
  writeWav("lead", l);
  CHECK(finite(l) && rms(l, at(0.2), at(0.8)) > 0.1 && peak(l) <= 1.0001f);
  // The default patch is a clean pluck out of the box.
  const Buf d = render({}, 0.6, 0.0f, 1.0f, {});
  writeWav("default", d);
  CHECK(finite(d) && peak(d) > 0.25f && peak(d) <= 1.0001f && peak(d, at(0.5)) < 0.02f);
  // A sweep of routings and shapes for listening.
  const int32_t routes[3] = {0, 1, 2};
  for (int32_t r : routes) {
    const Buf x = render({{P::Routing, float(r)}, {P::FilterB, 3.0f}, {P::Spread, 1.0f}}, 0.6, 0.0f, 1.0f, {});
    writeWav(r == 0 ? "route_single" : r == 1 ? "route_series" : "route_parallel", x);
  }
}

// ---- robustness -----------------------------------------------------------------------------------

void testExtremesStayFiniteAndBounded() {
  unsigned seed = 987654321u;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1 << 24); };
  for (int trial = 0; trial < 60; trial++) {
    Rig r;
    for (int32_t i = 0; i < kEdgeParamCount; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kEd, i);
      const float t = trial < 8 ? ((trial >> (i % 3)) & 1 ? 1.0f : 0.0f) : rnd();   // corners, then random
      r.set(static_cast<P>(i), p->min + t * (p->max - p->min));
    }
    r.set(P::Sustain, 1.0f);
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, (trial % 5) * 12.0f - 24.0f);
    r.go(0.8);
    r.e.instrumentNoteOn(1, 0.5f, 0.0f, 7.0f);
    r.go(0.5);
    r.e.instrumentNoteOff(1);
    r.go(0.4);
    CHECK(finite(r.o));
    CHECK(peak(r.o) <= 1.0001f);
    CHECK(std::fabs(mean(r.o, at(0.2), r.o.size())) < 0.1);
  }
}

void testLiveParameterChanges() {
  Rig r;
  r.set(P::Sustain, 1.0f);
  r.set(P::Resonance, 0.7f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  unsigned seed = 42u;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1 << 24); };
  for (int step = 0; step < 150; step++) {
    const int32_t i = static_cast<int32_t>(rnd() * kEdgeParamCount) % kEdgeParamCount;
    const InstrumentParamInfo* p = instrumentParamInfo(kEd, i);
    r.e.setInstrumentParam(1, i, p->min + rnd() * (p->max - p->min));
    r.go(0.02);
  }
  CHECK(finite(r.o) && peak(r.o) <= 1.0001f);
}

void testScheduledNoteStartsOnItsFrame() {
  Rig r;
  const int64_t at0 = 10000 + 37;
  r.e.scheduleInstrumentNote(at0, 1, 1.0f);
  r.go(0.4);
  CHECK(peak(r.o, 0, static_cast<size_t>(at0)) == 0.0f);
  CHECK(peak(r.o, static_cast<size_t>(at0), at(0.4)) > 0.01f);
}

void testRoutesThroughBusAndPan() {
  Rig r(0);
  r.set(P::Sustain, 1.0f);
  r.e.instrumentNoteOn(1, 1.0f, -1.0f, 0.0f);   // hard left
  Buf left, right;
  for (int i = 0; i < 100; i++) {
    r.e.process(r.f, kBlock);
    r.f += kBlock;
    left.insert(left.end(), r.e.outputChannel(0), r.e.outputChannel(0) + kBlock);
    right.insert(right.end(), r.e.outputChannel(1), r.e.outputChannel(1) + kBlock);
  }
  CHECK(rms(left, at(0.05), left.size()) > 4.0 * rms(right, at(0.05), right.size()));
}

void testDefaultPatchLevel() {
  const Buf b = render({}, 0.6, 0.0f, 1.0f, {});
  CHECK(peak(b) > 0.25f && peak(b) <= 1.0001f);
  CHECK(rms(b, 0, at(0.15)) > 0.05);
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kEd, 1);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 1200; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
    if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, 9);
    if (i == 250) e.setInstrumentParam(1, id(P::Routing), 2.0f);
    if (i == 300) e.instrumentNoteOn(1, 0.7f, 0, 2);
    if (i == 350) e.setInstrumentParam(1, id(P::FilterA), 3.0f);
    if (i == 400) e.setInstrumentParam(1, id(P::Resonance), 0.9f);
    if (i == 450) e.setInstrumentParam(1, id(P::Detune), 25.0f);
    if (i == 500) e.instrumentNoteOff(1);
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testRegistrationAndDefaults();
  testEveryParameterPropagates();
  testParamValidation();
  testLifecycle();
  testDeterministicAndRetrigger();
  testVelocity();
  testNotePitchSetsBase();
  testOscillatorTables();
  testHarmonicCountAndShape();
  testConstantLoudness();
  testAliasFloor();
  testPitchTransient();
  testToneEnvelope();
  testDetune();
  testLfo();
  testFilterModes();
  testRouting();
  testSeriesResonanceHeadroom();
  testFilterStability();
  testInstrumentFilters();
  testSpreadMovesStageB();
  testResonanceStaysCleanAndLevelled();
  testSoundDesignTargets();
  testExtremesStayFiniteAndBounded();
  testLiveParameterChanges();
  testScheduledNoteStartsOnItsFrame();
  testRoutesThroughBusAndPan();
  testDefaultPatchLevel();
  testRenderDoesNotAllocate();
  std::printf("edge_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
