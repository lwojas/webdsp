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

// Covers the Percussion Synth (instruments/percsynth.h) through the Engine's instrument API:
// registration, parameter table and propagation, lifecycle, velocity, the pitch envelope,
// oscillator mixing, FM, noise, filter, drive, bounds on extreme parameter sets, and allocation.
// DSP checks are sanity measurements (zero-crossing pitch, spectral bins, brightness), never exact samples.
// Set PERCSYNTH_WAV_DIR=<dir> to dump the cases as 16-bit WAVs for listening (docs/percussion-synth.md).
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
constexpr int32_t kPs = static_cast<int32_t>(InstrumentKind::PercSynth);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
using P = PercSynthParam;
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
  const char* dir = std::getenv("PERCSYNTH_WAV_DIR");
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
  explicit Rig(int32_t bus = 0) { e.init(kRate, 2, 8); e.createInstrument(1, kPs, bus); }
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


// Reference patch for measurements: oscillator 1 only, sine/triangle, no sweep, no noise, filter
// wide open, no drive, held (sustain 1). Each test changes only what it examines.
using Params = std::vector<std::pair<P, float>>;
const Params kBase = {{P::Tune, 110.0f}, {P::Osc1Wave, 0.0f}, {P::Osc1Level, 1.0f}, {P::Osc2Level, 0.0f},
                      {P::Osc2Fm, 0.0f}, {P::PitchEnv, 0.0f}, {P::Noise, 0.0f}, {P::Cutoff, 16000.0f},
                      {P::Resonance, 0.0f}, {P::FilterEnv, 0.0f}, {P::Drive, 0.0f}, {P::Attack, 0.001f},
                      {P::Decay, 1.0f}, {P::Sustain, 1.0f}};

Buf render(Params params, double seconds = 1.0, float pitch = 0.0f, float velocity = 1.0f,
           const Params& base = kBase) {
  Rig r;
  for (auto& [p, v] : base) r.set(p, v);
  for (auto& [p, v] : params) r.set(p, v);
  r.e.instrumentNoteOn(1, velocity, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}

// Rising zero crossings per second inside [a, b): the pitch of a simple tone.
double crossingHz(const Buf& v, size_t a, size_t b) {
  int n = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) if (v[i - 1] < 0.0f && v[i] >= 0.0f) n++;
  return n / (double(b - a) / kRate);
}
double mean(const Buf& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; i++) s += v[i];
  return s / double(b - a);
}

void testRegistrationAndDefaults() {
  const InstrumentKindMeta* m = instrumentKindMeta(kPs);
  CHECK(m && std::strcmp(m->id, "percsynth") == 0 && std::strcmp(m->name, "Percussion Synth") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(kPs == 16 && instrumentParamCount(kPs) == kPercSynthParamCount + 1 && kPercSynthParamCount == 23);
  auto in = makeInstrument(kPs);
  CHECK(in && in->kind() == InstrumentKind::PercSynth && !in->isActive());
  for (int32_t i = 0; i < kPercSynthParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kPs, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max && p->min < p->max);
  }
  CHECK(instrumentParamInfo(kPs, kPercSynthParamCount)->id == kInstrumentGainParam && instrumentParamInfo(kPs, kPercSynthParamCount + 1) == nullptr);
  auto find = [](const char* n) { for (int32_t i = 0; i < kPercSynthParamCount; i++) if (!std::strcmp(kPercSynthParams[i].name, n)) return &kPercSynthParams[i]; return (const InstrumentParamInfo*)nullptr; };
  CHECK_NEAR(find("tune")->def, 55.0, 1e-6);
  CHECK(find("osc1level")->def > 0.0f && find("osc2level")->def == 0.0f && find("noise")->def == 0.0f);
  CHECK(find("sustain")->def == 0.0f);                       // a one-shot out of the box
  CHECK(find("pitchenv")->max >= 6.0f && find("decay")->max >= 4.0f);   // not limited to short hits
  CHECK(resolveModDomain(*find("cutoff")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("pitchdecay")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("drive")) == ModDomain::Linear);
  CHECK(resolveModDomain(*find("filterenv")) == ModDomain::Linear && find("filterenv")->min < 0.0f);
  CHECK(resolveModDomain(*find("osc1wave")) == ModDomain::None);    // switches are not LFO targets
  CHECK(resolveModDomain(*find("osc2wave")) == ModDomain::None);
  CHECK(resolveModDomain(*find("filtertype")) == ModDomain::None);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kPs, 0));
}

// Every parameter reaches the DSP: moving it off its default changes the rendered note. The patch
// exercises all stages so no parameter is inert for lack of signal.
void testEveryParameterPropagates() {
  const Params patch = {{P::Osc2Level, 0.6f}, {P::Osc2Fm, 0.3f}, {P::Noise, 0.4f}, {P::FilterEnv, 0.5f},
                        {P::Drive, 0.4f}, {P::Sustain, 0.5f}, {P::Cutoff, 3000.0f}, {P::Resonance, 0.4f}};
  Rig ref;
  for (auto& [p, v] : patch) ref.set(p, v);
  ref.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
  ref.go(0.6);
  for (int32_t i = 0; i < kPercSynthParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kPs, i);
    const float v0 = std::clamp(0.0f, p->min, p->max);
    float moved = p->def;
    for (const auto& [pp, vv] : patch) if (id(pp) == i) moved = vv;
    const float target = (std::fabs(moved - p->min) > std::fabs(moved - p->max)) ? p->min : p->max;
    (void)v0;
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
  Rig r;
  r.set(P::Drive, -1.0f); r.set(P::Cutoff, 1e9f); r.set(P::PitchEnv, 99.0f);   // clamped
  r.set(P::Tune, std::nanf("")); r.set(P::Resonance, INFINITY);                 // ignored
  r.e.setInstrumentParam(1, 99, 1.0f);                                            // unknown id: no-op
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.3);
  CHECK(finite(r.o) && peak(r.o) > 0.0f && peak(r.o) <= 1.0001f);
  // Out-of-range values are the range edge.
  CHECK(render({{P::PitchEnv, 99.0f}}, 0.2) == render({{P::PitchEnv, 6.0f}}, 0.2));
  CHECK(render({{P::Osc1Wave, 7.0f}}, 0.2) == render({{P::Osc1Wave, 2.0f}}, 0.2));
}

void testVoiceCreationNoteOnAndOff() {
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kPs, 0));
  CHECK(!e.createInstrument(1, kPs, 0));         // duplicate id
  CHECK(e.activeInstrumentCount() == 0);
  e.instrumentNoteOn(1, 1.0f);
  CHECK(e.activeInstrumentCount() == 1);
  Rig r;                                          // default patch: a one-shot that ends itself
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.05);
  CHECK(peak(r.o, 0, 8) < 0.2f);                  // opens from (near) silence
  CHECK(peak(r.o) > 0.1f);
  r.go(1.5);
  CHECK(r.e.activeInstrumentCount() == 0);        // decay 0.35 s, sustain 0
  CHECK(peak(r.o, at(1.2), r.o.size()) == 0.0f);  // exact silence once the voice is done
  CHECK(finite(r.o));
  writeWav("default_note", r.o);
}

void testReleaseAndSustain() {
  Rig r;
  for (auto& [p, v] : kBase) r.set(p, v);
  r.set(P::Release, 0.2f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.5);
  CHECK(r.e.activeInstrumentCount() == 1);        // sustain 1: held until released
  const double held = rms(r.o, at(0.3), at(0.5));
  CHECK(held > 0.1);
  r.e.instrumentNoteOff(1);
  r.go(0.05);
  CHECK(rms(r.o, at(0.5), at(0.55)) > 0.0);
  r.go(0.4);
  CHECK(r.e.activeInstrumentCount() == 0);
  CHECK(peak(r.o, at(0.8), r.o.size()) == 0.0f);
  // A timed note (the `duration` path) ends the same way.
  Rig t;
  for (auto& [p, v] : kBase) t.set(p, v);
  t.set(P::Release, 0.1f);
  t.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
  t.go(0.2);
  CHECK(t.e.activeInstrumentCount() == 1);
  t.go(0.4);
  CHECK(t.e.activeInstrumentCount() == 0);
}

void testDeterministicAndRetrigger() {
  const Params n = {{P::Noise, 0.5f}, {P::Osc2Level, 0.5f}, {P::Osc2Fm, 0.4f}, {P::Drive, 0.5f}};
  CHECK(render(n, 0.5) == render(n, 0.5));
  Rig r;
  for (auto& [p, v] : kBase) r.set(p, v);
  r.set(P::Cutoff, 2000.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.5);
  const size_t t = r.o.size();
  r.e.instrumentNoteOn(1, 1.0f);                 // retrigger: phase, filter and level carry
  r.go(0.3);
  CHECK(finite(r.o));
  double steady = 0, around = 0;
  for (size_t i = at(0.3) + 1; i < t - 1; i++) steady = std::fmax(steady, std::fabs(r.o[i] - r.o[i - 1]));
  for (size_t i = t - 64; i < t + 64; i++) around = std::fmax(around, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(around <= steady * 1.3);
}

// A retrigger fades the amp out (~2 ms) and re-attacks from silence. The new note's noise, pitch
// and filter sweeps and its velocity must wait for that silence: applied at the retrigger frame they
// step the signal while the gain is still near full, which clicks.
void testRetriggerFadesWithoutStep() {
  Rig r;
  r.set(P::Noise, 1.0f); r.set(P::NoiseDecay, 0.05f); r.set(P::Osc1Level, 0.0f); r.set(P::Osc2Level, 0.0f);
  r.set(P::Sustain, 1.0f); r.set(P::Cutoff, 8000.0f); r.set(P::FilterEnv, 0.5f); r.set(P::Attack, 0.02f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.03);                                    // noise has decayed to a fraction of its start
  const size_t t = r.o.size();
  r.e.instrumentNoteOn(1, 0.4f);                 // softer retrigger: a velocity step would also click
  r.go(0.1);
  double pre = 0, fade = 0;
  for (size_t i = t - 100; i < t; i++) pre = std::fmax(pre, std::fabs(r.o[i] - r.o[i - 1]));
  for (size_t i = t; i < t + 90; i++) fade = std::fmax(fade, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(pre > 0.0);
  CHECK(fade <= pre * 1.3);                      // the fade steps no more than the tail it replaces
  CHECK(peak(r.o, t + 96, t + 110) < 0.05f);     // silent at the end of the fade, then the attack rises
  CHECK(rms(r.o, t + at(0.015), t + at(0.025)) > 3.0 * rms(r.o, t + 96, t + 150));
  CHECK(finite(r.o));
}

// Velocity scales level (roughly linearly) and a little of the character: harder hits are
// brighter and carry more noise, once the level difference is divided out.
void testVelocity() {
  const Params v = {{P::Noise, 0.5f}, {P::NoiseDecay, 0.2f}, {P::Osc2Level, 0.5f}, {P::Osc2Fm, 0.5f},
                    {P::Drive, 0.5f}, {P::Cutoff, 1500.0f}, {P::FilterEnv, 0.4f}, {P::Sustain, 0.0f},
                    {P::Decay, 0.4f}};
  const Buf lo = render(v, 0.6, 0.0f, 0.4f), hi = render(v, 0.6, 0.0f, 1.0f);
  const double rl = rms(lo, 0, at(0.4)), rh = rms(hi, 0, at(0.4));
  CHECK(rh > 1.8 * rl && rh < 3.5 * rl);                        // ~2.5 x for 2.5 x the velocity, plus character
  CHECK(bright(hi, 0, at(0.4)) > bright(lo, 0, at(0.4)));       // brighter, not just louder
  CHECK(finite(lo) && finite(hi) && peak(hi) <= 1.0001f);
  CHECK(peak(render(v, 0.3, 0.0f, 0.0f)) == 0.0f);              // velocity 0 is silent
  CHECK(finite(render(v, 0.3, 0.0f, 3.0f)) && peak(render(v, 0.3, 0.0f, 3.0f)) <= 1.0001f);
  // Monotonic: no jump in level between neighbouring velocities.
  double prev = 0;
  for (float vel : {0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
    const double r = rms(render(v, 0.5, 0.0f, vel), 0, at(0.4));
    CHECK(r > prev);
    prev = r;
  }
}

void testNotePitchSetsBase() {
  const Buf a = render({{P::Tune, 100.0f}}, 0.5), up = render({{P::Tune, 100.0f}}, 0.5, 12.0f);
  CHECK_NEAR(crossingHz(a, at(0.1), at(0.5)), 100.0, 3.0);
  CHECK_NEAR(crossingHz(up, at(0.1), at(0.5)), 200.0, 5.0);   // +12 st doubles it
}

// The pitch envelope starts the note high and settles on the base pitch; the amount sets how high,
// the decay how fast. Sine/triangle, so zero crossings read the pitch directly.
void testPitchEnvelope() {
  const Params base = {{P::Tune, 100.0f}, {P::PitchEnv, 3.0f}, {P::PitchDecay, 0.1f}};
  const Buf b = render(base, 1.0);
  // "Decay" is the time to fall ~40 dB, so the sweep is 1 % left by 0.1 s and halfway (in octaves) near 15 ms.
  const double early = crossingHz(b, at(0.0), at(0.01)), mid = crossingHz(b, at(0.02), at(0.04)),
               late = crossingHz(b, at(0.6), at(1.0));
  CHECK(early > 300.0);                       // 100 Hz * 2^3 at the start (averaged over 10 ms)
  CHECK(mid < early && mid > late);
  CHECK_NEAR(late, 100.0, 3.0);               // lands on the base pitch
  // Amount 0: flat. Larger amount: higher start.
  CHECK_NEAR(crossingHz(render({{P::Tune, 100.0f}, {P::PitchEnv, 0.0f}}, 0.3), at(0.0), at(0.01)), 100.0, 25.0);
  const double small = crossingHz(render({{P::Tune, 100.0f}, {P::PitchEnv, 1.0f}, {P::PitchDecay, 0.1f}}, 0.3), at(0.0), at(0.01));
  const double big = crossingHz(render({{P::Tune, 100.0f}, {P::PitchEnv, 5.0f}, {P::PitchDecay, 0.1f}}, 0.3), at(0.0), at(0.01));
  CHECK(small < early && early < big);
  // Decay: a long sweep is still high where a short one has landed.
  const double quick = crossingHz(render({{P::Tune, 100.0f}, {P::PitchEnv, 3.0f}, {P::PitchDecay, 0.02f}}, 0.4), at(0.1), at(0.15));
  const double slow = crossingHz(render({{P::Tune, 100.0f}, {P::PitchEnv, 3.0f}, {P::PitchDecay, 0.6f}}, 0.4), at(0.1), at(0.15));
  CHECK(quick < 110.0 && slow > 160.0);          // 40 dB points: 0.02 s vs 0.6 s
  // The same voice spans the range: zap-sized sweeps stay finite, bounded and audible.
  const Buf zap = render({{P::Tune, 200.0f}, {P::PitchEnv, 6.0f}, {P::PitchDecay, 0.4f}, {P::Osc1Wave, 1.0f}}, 0.6);
  CHECK(finite(zap) && peak(zap) <= 1.0001f && rms(zap, 0, at(0.2)) > 0.05);
  writeWav("zap", zap);
  // Osc 2 follows the sweep by its own factor: with osc2env 0 it holds its resting pitch.
  const Params two = {{P::Tune, 100.0f}, {P::PitchEnv, 3.0f}, {P::PitchDecay, 0.1f}, {P::Osc1Level, 0.0f},
                      {P::Osc2Level, 1.0f}, {P::Osc2Wave, 0.0f}, {P::Osc2Pitch, 0.0f}};
  Params follow = two, hold = two;
  follow.push_back({P::Osc2Env, 1.0f}); hold.push_back({P::Osc2Env, 0.0f});
  CHECK(crossingHz(render(follow, 0.3), at(0.0), at(0.01)) > 3.0 * crossingHz(render(hold, 0.3), at(0.0), at(0.01)));
}

void testOscillatorMixing() {
  const double a = at(0.2), b = at(1.0);
  const Params two = {{P::Tune, 110.0f}, {P::Osc2Pitch, 12.0f}, {P::Osc2Wave, 0.0f}};
  auto with = [&](float l1, float l2) {
    Params p = two; p.push_back({P::Osc1Level, l1}); p.push_back({P::Osc2Level, l2});
    return render(p, 1.0);
  };
  const Buf only1 = with(1.0f, 0.0f), only2 = with(0.0f, 1.0f), both = with(1.0f, 1.0f), none = with(0.0f, 0.0f);
  CHECK(peak(none) == 0.0f);                                           // nothing mixed in: silence
  CHECK(bin(only1, a, b, 110) > 0.2 && bin(only1, a, b, 220) < 0.05);
  CHECK(bin(only2, a, b, 220) > 0.2 && bin(only2, a, b, 110) < 0.05);
  CHECK(bin(both, a, b, 110) > 0.1 && bin(both, a, b, 220) > 0.1);     // both are present
  CHECK(bin(with(0.5f, 1.0f), a, b, 110) < 0.7 * bin(only1, a, b, 110));   // the faders scale
  CHECK(finite(both) && peak(both) <= 1.0001f);
  // Waveforms differ and keep their identity: saw has all harmonics, square only odd ones.
  auto w = [&](float wave) { return render({{P::Osc1Wave, wave}, {P::Tune, 110.0f}}, 1.0); };
  const Buf sn = w(0), sw = w(1), sq = w(2);
  CHECK(sn != sw && sw != sq && sn != sq);
  CHECK(bin(sn, a, b, 330) < 0.25 * bin(sn, a, b, 110));
  CHECK(bin(sw, a, b, 220) > 0.35 * bin(sw, a, b, 110));
  CHECK(bin(sq, a, b, 220) < 0.1 * bin(sq, a, b, 110) && bin(sq, a, b, 330) > 0.2 * bin(sq, a, b, 110));
  // Fractional osc2 pitch gives an inharmonic partial.
  Params inh = two; inh.push_back({P::Osc1Level, 0.0f}); inh.push_back({P::Osc2Level, 1.0f}); inh.push_back({P::Osc2Pitch, 8.5f});
  const Buf ib = render(inh, 1.0);
  const double f2 = 110.0 * std::pow(2.0, 8.5 / 12.0);
  CHECK(bin(ib, a, b, f2) > 0.2 && bin(ib, a, b, 220) < 0.05);
  writeWav("two_osc", both);
}

// Osc 1 pushes osc 2's frequency around: sidebands appear, grow with the amount, and it works with
// osc 1 muted (a pure modulator).
void testFm() {
  const double a = at(0.2), b = at(1.0);
  const Params base = {{P::Tune, 100.0f}, {P::Osc2Pitch, 12.0f}, {P::Osc2Wave, 0.0f}, {P::Osc1Level, 0.0f},
                       {P::Osc2Level, 1.0f}};
  auto fm = [&](float amt) { Params p = base; p.push_back({P::Osc2Fm, amt}); return render(p, 1.0); };
  const Buf f0 = fm(0.0f), f3 = fm(0.5f), f8 = fm(0.8f);
  const double c0 = bin(f0, a, b, 200);
  CHECK(c0 > 0.3);
  CHECK(bin(f3, a, b, 100) + bin(f3, a, b, 300) > 0.2);       // sidebands at carrier +- modulator
  CHECK(bright(f8, a, b) > bright(f3, a, b));                  // monotonic with the amount
  CHECK(bin(f8, a, b, 200) < 0.8 * c0);                        // energy leaves the carrier
  // Sidebands sit at the carrier +- n x modulator (100 Hz): 100 and 300 are absent without FM.
  CHECK(bin(f3, a, b, 100) + bin(f3, a, b, 300) > 5.0 * (bin(f0, a, b, 100) + bin(f0, a, b, 300)) + 0.01);
  // With osc 1 audible too, the mix still contains it.
  Params both = base; both.push_back({P::Osc2Fm, 0.5f}); both.push_back({P::Osc1Level, 1.0f});
  const Buf bb = render(both, 1.0);
  CHECK(bin(bb, a, b, 100) > bin(f3, a, b, 100) + 0.1);
  for (float wave : {0.0f, 1.0f, 2.0f}) {                      // every carrier waveform at full FM
    Params p = base; p.push_back({P::Osc2Fm, 1.0f}); p.push_back({P::Osc2Wave, wave}); p.push_back({P::Osc1Wave, 2.0f});
    p.push_back({P::PitchEnv, 6.0f}); p.push_back({P::Tune, 800.0f});
    const Buf e = render(p, 0.5, 12.0f);
    CHECK(finite(e) && peak(e) <= 1.0001f);
  }
  writeWav("fm_metal", fm(0.6f));
}

void testNoise() {
  const double a = at(0.0), b = at(0.3);
  const Params only = {{P::Osc1Level, 0.0f}, {P::Noise, 1.0f}, {P::NoiseDecay, 0.1f}};
  const Buf n = render(only, 1.0);
  CHECK(rms(n, a, at(0.03)) > 0.15);
  CHECK(std::fabs(mean(n, a, b)) < 0.02);                        // no DC
  // Tone moves the colour: dark -> white -> bright.
  auto tone = [&](float t) { Params p = only; p.push_back({P::NoiseTone, t}); return render(p, 1.0); };
  const Buf dark = tone(0.1f), white = tone(0.5f), light = tone(0.95f);
  CHECK(bright(dark, a, b) < 0.5 * bright(white, a, b));
  CHECK(bright(light, a, b) > 1.15 * bright(white, a, b));
  CHECK(rms(dark, a, at(0.03)) > 0.05 && rms(light, a, at(0.03)) > 0.05);   // dark/bright noise stay audible
  // Decay: a long noise tail is still there where a short one is gone.
  auto dec = [&](float d) { Params p = only; p.push_back({P::NoiseDecay, d}); p.push_back({P::Decay, 2.0f}); p.push_back({P::Sustain, 1.0f}); return render(p, 1.0); };
  const Buf shortN = dec(0.02f), longN = dec(0.6f);
  CHECK(rms(shortN, at(0.3), at(0.4)) < 0.1 * rms(shortN, at(0.0), at(0.02)));
  CHECK(rms(longN, at(0.3), at(0.4)) > 0.06 * rms(longN, at(0.0), at(0.02)));
  // Noise adds to a tone as a click: more energy in the first milliseconds than the tone alone.
  const Buf tonal = render({{P::Noise, 0.0f}}, 0.3), clicked = render({{P::Noise, 0.8f}, {P::NoiseDecay, 0.01f}}, 0.3);
  CHECK(bright(clicked, 0, at(0.01)) > 1.5 * bright(tonal, 0, at(0.01)));
  writeWav("noise_hat", render({{P::Osc1Level, 0.0f}, {P::Noise, 1.0f}, {P::NoiseTone, 1.0f}, {P::NoiseDecay, 0.05f}, {P::Decay, 0.1f}, {P::Sustain, 0.0f}}, 0.4));
}

void testFilter() {
  const double a = at(0.3), b = at(1.0);
  const Params saw = {{P::Osc1Wave, 1.0f}, {P::Tune, 110.0f}};
  auto with = [&](Params extra) { Params p = saw; for (auto& e : extra) p.push_back(e); return render(p, 1.0); };
  // Low-pass: closing the cutoff darkens; high-pass: opening the cutoff removes the low end.
  const Buf open = with({{P::Cutoff, 12000.0f}}), closed = with({{P::Cutoff, 300.0f}});
  CHECK(bright(closed, a, b) < 0.4 * bright(open, a, b));
  const Buf hp = with({{P::FilterType, 1.0f}, {P::Cutoff, 1500.0f}});
  CHECK(bin(hp, a, b, 110) < 0.15 * bin(open, a, b, 110));
  CHECK(bright(hp, a, b) > bright(open, a, b));
  const Buf lpOnly = with({{P::FilterType, 0.0f}, {P::Cutoff, 1500.0f}});
  CHECK(bin(lpOnly, a, b, 110) > 0.7 * bin(open, a, b, 110) && lpOnly != hp);
  // Resonance puts a peak at the cutoff (a saw harmonic: 6 x 110 = 660).
  const Buf flat = with({{P::Cutoff, 660.0f}, {P::Resonance, 0.0f}}), reson = with({{P::Cutoff, 660.0f}, {P::Resonance, 1.0f}});
  CHECK(bin(reson, a, b, 660) > 1.5 * bin(flat, a, b, 660));
  CHECK(finite(reson) && peak(reson) <= 1.0001f);
  // Envelope: positive opens the filter then closes; negative is the mirror; zero is static.
  auto e = [&](float amt) {
    Params p = {{P::Cutoff, amt < 0.0f ? 3000.0f : 400.0f}, {P::FilterDecay, 0.15f}, {P::FilterEnv, amt}};
    return with(p);
  };
  const Buf up = e(1.0f), dn = e(-1.0f), zero = e(0.0f);
  CHECK(bright(up, at(0.01), at(0.08)) > 2.0 * bright(up, at(0.6), at(1.0)));
  CHECK(bright(up, at(0.01), at(0.08)) > 1.8 * bright(zero, at(0.01), at(0.08)));
  CHECK(bright(dn, at(0.0), at(0.02)) < 0.6 * bright(dn, at(0.6), at(1.0)));
  CHECK_NEAR(bright(up, at(0.6), at(1.0)), bright(zero, at(0.6), at(1.0)), 0.1 * bright(zero, at(0.6), at(1.0)));
  // A faster filter decay closes sooner.
  auto d = [&](float s) { Params p = {{P::Cutoff, 400.0f}, {P::FilterEnv, 1.0f}, {P::FilterDecay, s}}; return with(p); };
  CHECK(bright(d(0.03f), at(0.1), at(0.2)) < bright(d(0.8f), at(0.1), at(0.2)));
  writeWav("filter_env", up);
}

void testDrive() {
  const double a = at(0.3), b = at(1.0);
  const Params base = {{P::Osc1Wave, 1.0f}, {P::Tune, 110.0f}};
  auto with = [&](float dr) { Params p = base; p.push_back({P::Drive, dr}); return render(p, 1.0); };
  const Buf d0 = with(0.0f), d3 = with(0.3f), d6 = with(0.6f), d10 = with(1.0f);
  CHECK(bright(d10, a, b) != bright(d0, a, b));
  CHECK(bin(d10, a, b, 110) < bin(d0, a, b, 110) * 1.7);          // compensated: density, not volume
  CHECK(rms(d10, a, b) > 0.4 * rms(d0, a, b) && rms(d10, a, b) < 1.9 * rms(d0, a, b));
  CHECK(d0 != d3 && d3 != d6 && d6 != d10);
  // Saturation adds even harmonics through its asymmetry (a sine has none).
  Params sine = {{P::Osc1Wave, 0.0f}, {P::Tune, 110.0f}, {P::Drive, 1.0f}};
  const Buf s = render(sine, 1.0), s0 = render({{P::Drive, 0.0f}}, 1.0);
  CHECK(bin(s, a, b, 220) > 5.0 * bin(s0, a, b, 220) + 0.01);
  // Drive 0 is transparent: identical to the stage not being there at all (checked via linearity).
  const Buf half = render({{P::Drive, 0.0f}}, 0.5, 0.0f, 0.5f), full = render({{P::Drive, 0.0f}}, 0.5, 0.0f, 1.0f);
  double worst = 0;
  for (size_t i = 0; i < half.size(); i++) worst = std::fmax(worst, std::fabs(full[i] - 2.0 * half[i]));
  CHECK(worst < 0.01 * peak(full));
  // Feedback is bounded: a hot, resonant, held patch settles to a steady level with no DC or growth.
  const Buf hot = render({{P::Drive, 1.0f}, {P::Resonance, 1.0f}, {P::Cutoff, 300.0f}, {P::Osc2Level, 1.0f},
                           {P::Noise, 1.0f}, {P::NoiseDecay, 2.0f}, {P::Osc1Wave, 2.0f}}, 3.0);
  CHECK(finite(hot) && peak(hot) <= 1.0001f);
  CHECK(std::fabs(mean(hot, at(1.0), at(3.0))) < 0.05);
  CHECK(rms(hot, at(2.0), at(3.0)) < 1.5 * rms(hot, at(1.0), at(2.0)) + 1e-3);
  writeWav("drive_full", d10);
}

// The categories the instrument is meant to cover, as sanity patches: each must produce an audible,
// bounded, characteristic signal with no architectural change.
void testSoundDesignTargets() {
  struct Target { const char* name; Params p; float pitch; double ms; };
  const Params off = {{P::Osc2Level, 0.0f}, {P::Noise, 0.0f}, {P::Drive, 0.0f}, {P::Osc2Fm, 0.0f}};
  const std::vector<Target> targets = {
      {"deep_kick", {{P::Tune, 45.0f}, {P::PitchEnv, 3.0f}, {P::PitchDecay, 0.05f}, {P::Decay, 0.4f}, {P::Cutoff, 400.0f}}, 0, 0.5},
      {"punchy_kick", {{P::Tune, 55.0f}, {P::PitchEnv, 4.5f}, {P::PitchDecay, 0.03f}, {P::Decay, 0.3f}, {P::Osc2Level, 0.4f}, {P::Osc2Wave, 2.0f}, {P::Osc2Pitch, 0.0f}, {P::Noise, 0.3f}, {P::NoiseDecay, 0.008f}, {P::Drive, 0.5f}}, 0, 0.5},
      {"tom", {{P::Tune, 130.0f}, {P::PitchEnv, 1.2f}, {P::PitchDecay, 0.12f}, {P::Decay, 0.6f}}, 0, 0.8},
      {"bass_perc", {{P::Tune, 55.0f}, {P::PitchEnv, 0.3f}, {P::PitchDecay, 0.05f}, {P::Decay, 1.2f}, {P::Osc1Wave, 1.0f}, {P::Cutoff, 600.0f}, {P::Drive, 0.3f}}, 0, 1.4},
      {"metal_hit", {{P::Tune, 300.0f}, {P::Osc1Wave, 2.0f}, {P::Osc2Wave, 2.0f}, {P::Osc2Pitch, 8.4f}, {P::Osc2Level, 1.0f}, {P::Osc2Fm, 0.7f}, {P::FilterType, 1.0f}, {P::Cutoff, 2500.0f}, {P::Decay, 0.15f}, {P::PitchEnv, 0.0f}}, 0, 0.3},
      {"zap", {{P::Tune, 120.0f}, {P::PitchEnv, 6.0f}, {P::PitchDecay, 0.3f}, {P::Attack, 0.0005f}, {P::Decay, 0.35f}, {P::Osc1Wave, 1.0f}}, 0, 0.4},
      {"noisy", {{P::Osc1Level, 0.0f}, {P::Noise, 1.0f}, {P::NoiseDecay, 0.3f}, {P::Cutoff, 500.0f}, {P::FilterEnv, 0.8f}, {P::FilterDecay, 0.1f}, {P::Decay, 0.12f}, {P::Resonance, 0.4f}}, 0, 0.3},
      {"experimental", {{P::Tune, 70.0f}, {P::Osc1Wave, 1.0f}, {P::Osc2Wave, 2.0f}, {P::Osc2Pitch, 19.7f}, {P::Osc2Level, 0.8f}, {P::Osc2Fm, 0.9f}, {P::PitchEnv, 3.0f}, {P::Resonance, 0.9f}, {P::Cutoff, 900.0f}, {P::FilterEnv, 0.7f}, {P::Drive, 1.0f}, {P::Noise, 0.3f}, {P::Decay, 0.8f}}, 0, 1.0},
  };
  for (const Target& t : targets) {
    const Buf b = render(t.p, t.ms + 0.2, t.pitch, 1.0f, [&] { Params q = kBase; for (auto& x : off) q.push_back(x); q.push_back({P::Sustain, 0.0f}); return q; }());
    CHECK(finite(b) && peak(b) <= 1.0001f);
    CHECK(rms(b, 0, at(0.15)) > 0.03);
    writeWav(t.name, b);
  }
  // The kick-like and zap-like patches are what they say: the sweep is audible and the body low.
  const Buf kick = render(targets[0].p, 0.7, 0.0f, 1.0f, [&] { Params q = kBase; q.push_back({P::Sustain, 0.0f}); return q; }());
  CHECK(crossingHz(kick, at(0.0), at(0.02)) > 1.6 * crossingHz(kick, at(0.1), at(0.2)));
  CHECK(bin(kick, at(0.0), at(0.3), 45) > 4.0 * bin(kick, at(0.0), at(0.3), 400));
}

// Valid-but-extreme parameter sets: every combination of the edges, plus deterministic scatter,
// across the keyboard, hard velocities, and with retriggers.
void testExtremesStayFiniteAndBounded() {
  const int32_t n = kPercSynthParamCount;
  unsigned s = 12345;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / float(1 << 24); };
  for (int trial = 0; trial < 80; trial++) {
    Rig r;
    for (int32_t i = 0; i < n; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kPs, i);
      float v;
      if (trial == 0) v = p->min;
      else if (trial == 1) v = p->max;
      else if (trial < 16) v = (rnd() < 0.5f) ? p->min : p->max;
      else v = p->min + rnd() * (p->max - p->min);
      r.e.setInstrumentParam(1, i, v);
    }
    const float pitch = trial % 3 == 0 ? -24.0f : (trial % 3 == 1 ? 0.0f : 40.0f);
    r.e.instrumentNoteOn(1, 1.5f, 0.0f, pitch);
    r.go(0.25);
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, pitch + 5.0f);
    r.go(0.25);
    r.e.instrumentNoteOff(1);
    r.go(0.3);
    CHECK(finite(r.o));
    CHECK(peak(r.o) <= 1.0001f);
  }
}

// Changing parameters under a sounding note (what automation does) never breaks the output.
void testLiveParameterChanges() {
  Rig r;
  r.set(P::Sustain, 1.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  unsigned s = 99;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / float(1 << 24); };
  for (int i = 0; i < 300; i++) {
    const int32_t p = static_cast<int32_t>(rnd() * kPercSynthParamCount) % kPercSynthParamCount;
    const InstrumentParamInfo* pi = instrumentParamInfo(kPs, p);
    r.e.setInstrumentParam(1, p, pi->min + rnd() * (pi->max - pi->min));
    r.go(0.01);
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
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kPs, 0);
  e.instrumentNoteOn(1, 1.0f, 1.0f);
  int64_t f = 0;
  double l = 0, rr = 0;
  for (int i = 0; i < 200; i++) {
    e.process(f, kBlock); f += kBlock;
    for (int k = 0; k < kBlock; k++) { l += std::fabs(e.outputChannel(0)[k]); rr += std::fabs(e.outputChannel(1)[k]); }
  }
  CHECK(rr > 4.0 * l);   // hard-right pan
}

void testDefaultPatchLevel() {
  const Buf b = render({}, 0.6, 0.0f, 1.0f, {});
  CHECK(peak(b) > 0.3f && peak(b) <= 1.0001f);
  CHECK(rms(b, 0, at(0.15)) > 0.1);
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kPs, 1);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 1200; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
    if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, 9);
    if (i == 250) e.setInstrumentParam(1, id(P::Drive), 0.9f);
    if (i == 300) e.instrumentNoteOn(1, 0.7f, 0, 2);
    if (i == 350) e.setInstrumentParam(1, id(P::FilterType), 1.0f);
    if (i == 400) e.setInstrumentParam(1, id(P::Resonance), 0.9f);
    if (i == 450) e.setInstrumentParam(1, id(P::Osc2Fm), 0.9f);
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
  testVoiceCreationNoteOnAndOff();
  testReleaseAndSustain();
  testDeterministicAndRetrigger();
  testRetriggerFadesWithoutStep();
  testVelocity();
  testNotePitchSetsBase();
  testPitchEnvelope();
  testOscillatorMixing();
  testFm();
  testNoise();
  testFilter();
  testDrive();
  testSoundDesignTargets();
  testExtremesStayFiniteAndBounded();
  testLiveParameterChanges();
  testScheduledNoteStartsOnItsFrame();
  testRoutesThroughBusAndPan();
  testDefaultPatchLevel();
  testRenderDoesNotAllocate();
  std::printf("percsynth_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
