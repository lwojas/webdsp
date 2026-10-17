#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the synthesized FM voice (instruments/fm.h) through the Engine's instrument API.
// Set FM_WAV_DIR=<dir> to dump cases as 16-bit WAVs for listening (see docs/fm-instrument.md).
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
constexpr int32_t kFm = static_cast<int32_t>(InstrumentKind::Fm);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
int32_t id(FmParam p) { return static_cast<int32_t>(p); }

// Ratio step indices (fm.h kFmRatios): 0.5, 1, 2, 3, 4, 5, 1.414, 3.5 (0 and 4 unused here)
constexpr float R_1 = 1, R_2 = 2, R_3 = 3, R_5 = 5, R_SQRT2 = 6, R_35 = 7;

struct Setup {
  // -1 = leave at default.
  float tune = -1, ratio = -1, timbre = -1, snap = -1, tone = -1, attack = -1, decay = -1,
        sustain = -1, release = -1, legato = -1, slide = -1;
  float velocity = 1.0f, pitch = 0.0f;
  double duration = -1;  // seconds; < 0 = held
  double rate = kRate;
};

size_t at(double s, double rate = kRate) { return static_cast<size_t>(s * rate); }

void apply(Engine& e, const Setup& s) {
  auto set = [&](FmParam p, float v) { if (v >= 0) e.setInstrumentParam(1, id(p), v); };
  set(FmParam::Tune, s.tune); set(FmParam::Ratio, s.ratio); set(FmParam::Timbre, s.timbre);
  set(FmParam::Snap, s.snap); set(FmParam::Tone, s.tone); set(FmParam::Attack, s.attack);
  set(FmParam::Decay, s.decay); set(FmParam::Sustain, s.sustain); set(FmParam::Release, s.release);
  set(FmParam::Legato, s.legato); set(FmParam::Slide, s.slide);
}

void run(Engine& e, int64_t& frame, double seconds, Buf* l, double rate = kRate) {
  const int32_t frames = static_cast<int32_t>(seconds * rate / kBlock) * kBlock;
  for (int32_t d = 0; d < frames; d += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    l->insert(l->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
}

void writeWav(const char* name, const Buf& b) {
  const char* dir = std::getenv("FM_WAV_DIR");
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
    const int16_t s = static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, b[i])) * 32767);
    std::fwrite(&s, 2, 1, f);
  }
  std::fclose(f);
}

// One note on instrument 1 (master bus), `seconds` rendered.
Buf play(const Setup& s, double seconds, const char* wav = nullptr) {
  Engine e;
  e.init(s.rate, 2, 8);
  e.createInstrument(1, kFm, 0);
  apply(e, s);
  const int32_t dur = s.duration < 0 ? -1 : static_cast<int32_t>(std::lround(s.duration * s.rate));
  e.instrumentNoteOn(1, s.velocity, 0.0f, s.pitch, dur);
  int64_t f = 0;
  Buf out;
  run(e, f, seconds, &out, s.rate);
  if (wav) writeWav(wav, out);
  return out;
}

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
// Magnitude of the DFT at `hz` over [a, b) with a Hann window.
double bin(const Buf& v, size_t a, size_t b, double hz, double rate = kRate) {
  double re = 0, im = 0;
  b = std::min(b, v.size());
  const double n = double(b - a);
  for (size_t i = a; i < b; i++) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * double(i - a) / n);
    const double ph = 2 * kPi * hz * double(i) / rate;
    re += w * v[i] * std::cos(ph);
    im -= w * v[i] * std::sin(ph);
  }
  return std::sqrt(re * re + im * im) * 2.0 / n;
}
// Strongest DFT bin within +-8% of `nominal`.
double peakFreq(const Buf& v, size_t a, size_t b, double nominal) {
  double best = 0, bestHz = 0;
  for (double hz = nominal * 0.92; hz <= nominal * 1.08; hz += nominal * 0.0005) {
    const double m = bin(v, a, b, hz);
    if (m > best) { best = m; bestHz = hz; }
  }
  return bestHz;
}
// Share of spectral energy that is NOT the fundamental (harmonics 2..32 over 1..32). 0 = pure sine.
double harmonicActivity(const Buf& v, size_t a, size_t b, double f0) {
  double fund = 0, all = 0;
  for (int h = 1; h <= 32; h++) {
    const double m = bin(v, a, b, f0 * h);
    all += m * m;
    if (h == 1) fund = m * m;
  }
  return all > 0 ? 1.0 - fund / all : 0.0;
}
// Share of spectral energy in harmonics 4..32 (brightness that, unlike harmonicActivity, keeps
// rising with the index even once the fundamental itself has grown back).
double highShare(const Buf& v, size_t a, size_t b, double f0) {
  double hi = 0, all = 0;
  for (int h = 1; h <= 32; h++) {
    const double m = bin(v, a, b, f0 * h);
    all += m * m;
    if (h >= 4) hi += m * m;
  }
  return all > 0 ? hi / all : 0.0;
}
// Spectral bandwidth: the harmonic number below which 90% of the energy (harmonics 1..40) lies.
// Grows with the modulation index.
int bandwidthH(const Buf& v, size_t a, size_t b, double f0) {
  double m[41], all = 0;
  for (int h = 1; h <= 40; h++) { m[h] = bin(v, a, b, f0 * h); m[h] *= m[h]; all += m[h]; }
  double acc = 0;
  for (int h = 1; h <= 40; h++) { acc += m[h]; if (acc >= 0.9 * all) return h; }
  return 40;
}
// Zero-crossing frequency estimate (positive-going) on a clean sine.
double zcFreq(const Buf& v, size_t a, size_t b) {
  int n = 0;
  size_t first = 0, last = 0;
  for (size_t i = a + 1; i < b && i < v.size(); i++) {
    if (v[i - 1] <= 0.0f && v[i] > 0.0f) {
      if (n == 0) first = i;
      last = i;
      n++;
    }
  }
  return n < 2 ? 0.0 : (n - 1) * kRate / double(last - first);
}
double maxStep(const Buf& v, size_t a, size_t b) {
  double m = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) m = std::fmax(m, std::fabs(double(v[i]) - double(v[i - 1])));
  return m;
}
bool finiteAll(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}

// A clean reference: no modulation at all, pure carrier sine, flat envelope.
Setup sine() {
  Setup s;
  s.ratio = R_1; s.timbre = 0; s.snap = 0; s.tone = 1; s.attack = 0.001f;
  s.decay = 0.03f; s.sustain = 1; s.release = 0.05f;
  return s;
}
// A sustained, flat-level FM voice with no pitch-dependent bass weight (>= 320 Hz).
Setup flat(float timbre, float ratio) {
  Setup s;
  s.tune = 400; s.ratio = ratio; s.timbre = timbre; s.snap = 0; s.tone = 1; s.attack = 0.001f;
  s.decay = 0.03f; s.sustain = 1; s.release = 0.05f;
  return s;
}

// --- tests -----------------------------------------------------------------------------------

void testProducesFiniteBoundedAudio() {
  Buf o = play({}, 1.0, "default");
  CHECK(finiteAll(o));
  CHECK(peak(o) > 0.2f);
  CHECK(peak(o) <= 1.0f);
  // Velocity above 1 cannot exceed full scale either.
  Setup s; s.velocity = 4.0f; s.timbre = 1; s.snap = 1;
  Buf loud = play(s, 0.5);
  CHECK(finiteAll(loud));
  CHECK(peak(loud) <= 1.0f);
}

void testDeterministic() {
  Setup s; s.timbre = 0.7f; s.snap = 0.8f; s.ratio = R_SQRT2;
  Buf a = play(s, 0.5), b = play(s, 0.5);
  CHECK(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// Carrier pitch follows tune and the note's semitone offset.
void testCarrierPitch() {
  const float tunes[] = {55, 110, 220, 440};
  const float semis[] = {0, 7, 12, -12};
  for (float t : tunes) {
    for (float p : semis) {
      Setup s = sine(); s.tune = t; s.pitch = p;
      Buf o = play(s, 0.5);
      const double want = t * std::exp2(p / 12.0);
      CHECK_NEAR(peakFreq(o, at(0.1), at(0.5), want) / want, 1.0, 0.003);
    }
  }
  // With real modulation the fundamental (the strongest of ratio 1 at low index) is still the note.
  Setup s; s.tune = 220; s.ratio = R_1; s.timbre = 0.2f; s.snap = 0;
  CHECK_NEAR(peakFreq(play(s, 0.5), at(0.1), at(0.5), 220) / 220.0, 1.0, 0.003);
}

// The modulator is derived from the carrier: spectrum shape is the same at every pitch, and the
// partials sit where ratio says.
void testModulatorFollowsCarrier() {
  auto profile = [](const Buf& o, double f0) {
    double sum = 0;
    std::vector<double> m;
    for (int h = 1; h <= 10; h++) { m.push_back(bin(o, at(0.1), at(0.6), f0 * h)); sum += m.back() * m.back(); }
    for (double& x : m) x /= std::sqrt(sum);
    return m;
  };
  for (float ratio : {R_1, R_2, R_3}) {
    Setup lo = flat(0.4f, ratio); lo.tune = 400;
    Setup hi = lo; hi.pitch = 12;   // 800 Hz via the note, not via tune
    Setup hi2 = lo; hi2.tune = 800; // 800 Hz via tune
    const auto a = profile(play(lo, 0.6), 400), b = profile(play(hi, 0.6), 800),
               c = profile(play(hi2, 0.6), 800);
    double dab = 0, dbc = 0;
    for (size_t i = 0; i < a.size(); i++) { dab += std::fabs(a[i] - b[i]); dbc += std::fabs(b[i] - c[i]); }
    CHECK(dab < 0.12);   // same shape an octave up
    CHECK(dbc < 0.02);   // note pitch and tune are the same thing
  }
  // Integer ratios put every partial on a multiple of the note: nothing between the harmonics.
  {
    Setup s = flat(0.5f, R_3);
    Buf o = play(s, 0.6);
    double between = 0, on = 0;
    for (int h = 1; h <= 8; h++) {
      on += bin(o, at(0.1), at(0.6), 400.0 * h);
      between += bin(o, at(0.1), at(0.6), 400.0 * (h + 0.5));
    }
    CHECK(between < on * 0.02);
  }
  // Sideband positions: ratio 3 puts sidebands at f(1 +- 3k): 2f, 4f, 5f, 7f ... and (for a
  // low index) nothing at 3f, 6f. Ratio sqrt(2) puts energy at f(1 + 1.414), not on a harmonic.
  {
    Setup s = flat(0.35f, R_3);
    Buf o = play(s, 0.6);
    CHECK(bin(o, at(0.1), at(0.6), 800) > 0.05);
    CHECK(bin(o, at(0.1), at(0.6), 1200) < 0.01);
    Setup w = flat(0.35f, R_1);
    Setup m = flat(0.35f, R_SQRT2);
    const double side = 400.0 * (1.0 + std::sqrt(2.0));
    CHECK(bin(play(m, 0.6), at(0.1), at(0.6), side) > 0.05);
    CHECK(bin(play(w, 0.6), at(0.1), at(0.6), side) < 0.01);
  }
}

// Index 0 is exactly the carrier sine (also in the bass register, where the weight blend is on).
void testZeroModulationIsCarrierOnly() {
  for (float tune : {55.f, 220.f, 440.f}) {
    Setup s = sine(); s.tune = tune; s.velocity = 0.5f;   // 0.25 peak: below the soft-clip knee
    Buf o = play(s, 0.5);
    const double fund = bin(o, at(0.1), at(0.5), tune);
    double rest = 0;
    for (int h = 2; h <= 10; h++) rest = std::fmax(rest, bin(o, at(0.1), at(0.5), tune * h));
    CHECK(fund > 0.09);
    CHECK(rest < fund * 1e-3);
    CHECK_NEAR(peak(o, at(0.1), at(0.5)), 0.25, 0.01);  // 0.5 * velocity * sustain level
  }
  // Same thing through every ratio: with no index the ratio is inaudible.
  Buf a = play([] { Setup s = sine(); s.ratio = R_1; return s; }(), 0.3);
  Buf b = play([] { Setup s = sine(); s.ratio = R_35; return s; }(), 0.3);
  double d = 0;
  for (size_t i = 0; i < a.size(); i++) d = std::fmax(d, std::fabs(a[i] - b[i]));
  CHECK(d < 1e-5);
}

// More timbre = more harmonic activity, and loudness does not run away with it.
void testTimbreRaisesHarmonicActivity() {
  for (float ratio : {R_1, R_3}) {
    double prev = -1;
    for (float t : {0.0f, 0.15f, 0.3f, 0.45f, 0.6f}) {
      Buf o = play(flat(t, ratio), 0.6);
      const double h = harmonicActivity(o, at(0.1), at(0.6), 400);
      CHECK(h >= prev - 1e-9);
      if (t == 0.0f) CHECK(h < 1e-4);
      prev = h;
    }
    CHECK(prev > 0.5);   // timbre 0.6 is already far from a sine
  }
  // Brightness is generated, not scaled: RMS stays within a few dB across the whole range.
  double lo = 1e9, hi = 0;
  for (float t : {0.0f, 0.3f, 0.6f, 1.0f}) {
    const double r = rms(play(flat(t, R_1), 0.6), at(0.1), at(0.6));
    lo = std::fmin(lo, r); hi = std::fmax(hi, r);
  }
  CHECK(hi / lo < 1.6);
}

// The index envelope: bright onset that settles, while the amplitude envelope stays flat.
void testSnapIsAnIndexEnvelope() {
  Setup s = flat(0.1f, R_1); s.snap = 1; s.decay = 0.3f; s.sustain = 1;   // flat level
  Buf o = play(s, 1.2, "snap");
  const double early = harmonicActivity(o, at(0.004), at(0.034), 400);
  const double mid = harmonicActivity(o, at(0.1), at(0.13), 400);
  const double late = harmonicActivity(o, at(0.8), at(0.83), 400);
  CHECK(early > mid);
  CHECK(mid > late);
  CHECK(early > 0.5);
  CHECK(late < 0.15);
  // Level does not follow it (the amplitude envelope is flat at sustain 1).
  CHECK(rms(o, at(0.8), at(0.9)) / rms(o, at(0.1), at(0.2)) > 0.6);
  CHECK(rms(o, at(0.8), at(0.9)) / rms(o, at(0.1), at(0.2)) < 1.6);
  // snap 0: no movement.
  Setup n = s; n.snap = 0;
  Buf q = play(n, 1.2);
  CHECK_NEAR(harmonicActivity(q, at(0.004), at(0.034), 400), harmonicActivity(q, at(0.8), at(0.83), 400), 0.02);
  // The envelope follows `decay`: a longer decay is still bright later.
  Setup slow = s; slow.decay = 1.2f;
  Buf sl = play(slow, 1.2);
  CHECK(harmonicActivity(sl, at(0.1), at(0.13), 400) > mid);
  // And more snap is brighter at the onset.
  Setup half = s; half.snap = 0.2f;
  CHECK(bandwidthH(play(half, 0.1), at(0.004), at(0.034), 400) < bandwidthH(o, at(0.004), at(0.034), 400));
}

void testAmplitudeEnvelope() {
  Setup s = sine(); s.tune = 220; s.attack = 0.05f; s.decay = 0.2f; s.sustain = 0.5f; s.release = 0.2f;
  s.duration = 1.0;
  Buf o = play(s, 1.6);
  const double pk = peak(o, at(0.04), at(0.12));
  const double sus = peak(o, at(0.6), at(0.9));
  CHECK_NEAR(sus / pk, 0.5, 0.05);                         // public sustain is the real level
  CHECK(peak(o, at(0.005), at(0.015)) < pk * 0.3);         // still climbing in the attack
  CHECK(rms(o, at(1.3), at(1.6)) < 1e-4);                  // released and silent
  const size_t off = at(1.0);
  CHECK(peak(o, off - 1500, off) > sus * 0.8);             // release starts at the duration frame
  CHECK(peak(o, off + at(0.18), off + at(0.2)) < sus * 0.2);
  // sustain 0: a pluck that ends itself after the decay.
  Setup p = s; p.sustain = 0; p.duration = -1;
  CHECK(rms(play(p, 1.2), at(0.9), at(1.2)) < 1e-4);
}

// Release is click-free: no step beyond what the signal itself does, and it ends at exactly zero.
void testReleaseIsContinuous() {
  for (float ratio : {R_1, R_2, R_SQRT2}) {
    Setup s; s.tune = 110; s.ratio = ratio; s.timbre = 0.6f; s.snap = 0.5f; s.attack = 0.003f;
    s.decay = 0.2f; s.sustain = 0.6f; s.release = 0.15f; s.duration = 0.5;
    Buf o = play(s, 1.0, "release");
    const double steady = maxStep(o, at(0.35), at(0.5));
    CHECK(maxStep(o, at(0.5) - 96, at(0.5) + 96) < steady * 1.3);        // across note-off
    CHECK(maxStep(o, at(0.5), at(0.66)) < steady * 1.3);                 // through the tail
    CHECK(rms(o, at(0.7), at(1.0)) == 0.0);                              // ended, exact silence
    CHECK(peak(o, at(0.66), at(0.7)) < 1e-3);
  }
  // Retrigger into a releasing voice: still no step.
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kFm, 0);
  Setup s; s.timbre = 0.5f; s.snap = 0; s.release = 0.4f; apply(e, s);   // (snap would re-brighten)
  e.instrumentNoteOn(1, 1.0f, 0, 0, 12000);
  int64_t f = 0; Buf o; run(e, f, 0.35, &o);
  e.instrumentNoteOn(1, 1.0f, 0, 7);
  run(e, f, 0.3, &o);
  CHECK(finiteAll(o));
  // (A higher note and a re-attack legitimately steepen the waveform; a click would be far larger.)
  CHECK(maxStep(o, at(0.35) - 64, at(0.35) + 64) < maxStep(o, at(0.1), at(0.2)) * 2.5);
}

void testParamBoundsAndMetadata() {
  CHECK(kFmParamCount == 11);
  const char* names[] = {"tune", "ratio", "timbre", "snap", "tone", "attack", "decay", "sustain",
                         "release", "legato", "slide"};
  for (int i = 0; i < kFmParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kFm, i);
    CHECK(p && p->id == i && std::strcmp(p->name, names[i]) == 0);
    CHECK(p && p->min < p->max && p->def >= p->min && p->def <= p->max);
  }
  CHECK(instrumentParamInfo(kFm, kFmParamCount) == nullptr);
  CHECK(std::strcmp(instrumentKindMeta(kFm)->id, "fm") == 0);
  CHECK(std::strcmp(instrumentKindMeta(kFm)->category, "synth") == 0);
  // Out of range clamps; non-finite and unknown ids are ignored.
  auto run1 = [](auto&& configure) {
    Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kFm, 0);
    configure(e);
    e.instrumentNoteOn(1, 1.0f);
    int64_t f = 0; Buf o; run(e, f, 0.3, &o);
    return o;
  };
  auto same = [](const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * 4) == 0; };
  const float nan = std::nanf("");
  CHECK(same(run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Timbre), 99.f); }),
             run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Timbre), 1.f); })));
  CHECK(same(run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Timbre), -5.f); }),
             run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Timbre), 0.f); })));
  CHECK(same(run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Ratio), 99.f); }),
             run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Ratio), 7.f); })));
  CHECK(same(run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Ratio), 2.4f); }),
             run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Ratio), 2.f); })));
  CHECK(same(run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Tune), 1e9f); }),
             run1([](Engine& e) { e.setInstrumentParam(1, id(FmParam::Tune), 880.f); })));
  CHECK(same(run1([&](Engine& e) { e.setInstrumentParam(1, id(FmParam::Snap), nan); e.setInstrumentParam(1, 99, 1.f); }),
             run1([](Engine&) {})));
  // Extreme note inputs stay finite and bounded.
  for (float semis : {-1000.f, -48.f, 48.f, 1000.f}) {
    Setup s; s.timbre = 1; s.snap = 1; s.pitch = semis; s.tune = 880;
    Buf o = play(s, 0.3);
    CHECK(finiteAll(o) && peak(o) <= 1.0f);
  }
}

// Several simultaneous instances stay stable and independent (the mix is the sum of the parts).
void testPolyphony() {
  auto setupVoice = [](Engine& e, int32_t idn, float tune, float ratio, float timbre, float pitch) {
    e.createInstrument(idn, kFm, 0);
    e.setInstrumentParam(idn, id(FmParam::Tune), tune);
    e.setInstrumentParam(idn, id(FmParam::Ratio), ratio);
    e.setInstrumentParam(idn, id(FmParam::Timbre), timbre);
    e.setInstrumentParam(idn, id(FmParam::Sustain), 1.0f);
    e.instrumentNoteOn(idn, 0.3f, 0, pitch);
  };
  auto render = [&](int count) {
    Engine e; e.init(kRate, 2, 8);
    for (int i = 0; i < count; i++) setupVoice(e, 10 + i, 55.f * (1 + i), float(i % 8), 0.2f + 0.05f * i, float(i % 5));
    int64_t f = 0; Buf o; run(e, f, 0.5, &o);
    return o;
  };
  Buf many = render(24);
  CHECK(finiteAll(many));
  CHECK(peak(many) < 24 * 0.5f + 1e-3f);
  CHECK(peak(many) > 0.3f);
  // Linearity: voice A + voice B rendered together equals the sum of each alone.
  auto one = [&](int i) {
    Engine e; e.init(kRate, 2, 8);
    setupVoice(e, 10 + i, 55.f * (1 + i), float(i % 8), 0.2f + 0.05f * i, float(i % 5));
    int64_t f = 0; Buf o; run(e, f, 0.3, &o);
    return o;
  };
  Buf a = one(0), b = one(3);
  Engine e; e.init(kRate, 2, 8);
  setupVoice(e, 10, 55.f, 0.f, 0.2f, 0.f);
  setupVoice(e, 13, 55.f * 4, 3.f, 0.35f, 3.f);
  int64_t f = 0; Buf both; run(e, f, 0.3, &both);
  double d = 0;
  for (size_t i = 0; i < both.size(); i++) d = std::fmax(d, std::fabs(both[i] - (a[i] + b[i])));
  CHECK(d < 1e-5);
}

// Slam every parameter and the note pitch at several sample rates: always finite and bounded.
void testStabilityFuzz() {
  for (double rate : {22050.0, 48000.0, 96000.0}) {
    Engine e; e.init(rate, 2, 8);
    e.createInstrument(1, kFm, 0);
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1 << 24); };
    int64_t f = 0;
    Buf o;
    for (int b = 0; b < int(rate * 3 / kBlock); b++) {
      if (b % 40 == 0) {
        for (int p = 0; p < kFmParamCount; p++) {
          const InstrumentParamInfo* pi = instrumentParamInfo(kFm, p);
          e.setInstrumentParam(1, pi->id, pi->min + rnd() * (pi->max - pi->min));
        }
        e.instrumentNoteOn(1, rnd() * 1.5f, rnd() * 2 - 1, (rnd() - 0.5f) * 80, rnd() < 0.5f ? int(rnd() * rate * 0.3) : -1);
      }
      e.process(f, kBlock);
      f += kBlock;
      o.insert(o.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
      o.insert(o.end(), e.outputChannel(1), e.outputChannel(1) + kBlock);
    }
    CHECK(finiteAll(o));
    CHECK(peak(o) <= 1.4143f);   // 1.0 plus the panner's +3 dB at hard pan
  }
}

// High notes with a high ratio and full index cannot alias: sideband energy near Nyquist stays
// negligible (the effective index is bandwidth-limited), at any sample rate.
void testBandwidthGuard() {
  for (double rate : {22050.0, 48000.0, 96000.0}) {
    Setup s; s.rate = rate; s.tune = 880; s.pitch = 12; s.ratio = R_5; s.timbre = 1; s.snap = 1;
    s.tone = 1; s.sustain = 1; s.attack = 0.001f; s.decay = 0.05f;
    Buf o = play(s, 0.5, rate == kRate ? "alias_guard" : nullptr);
    CHECK(finiteAll(o));
    double total = 0, top = 0;
    const size_t a = at(0.1, rate), b = at(0.5, rate);
    for (double hz = 100; hz < rate * 0.5; hz += 97) {
      const double m = bin(o, a, b, hz, rate);
      total += m * m;
      if (hz > rate * 0.44) top += m * m;
    }
    CHECK(top < total * 0.01);
  }
}

void testVelocity() {
  // Level is linear in velocity (pure sine, below the soft-clip knee).
  Setup a = sine(), b = sine(); a.velocity = 0.25f; b.velocity = 0.5f;
  CHECK_NEAR(peak(play(b, 0.3), at(0.1), at(0.3)) / peak(play(a, 0.3), at(0.1), at(0.3)), 2.0, 0.05);
  // Harder is brighter (always on), not just louder.
  Setup soft = flat(0.4f, R_1), hard = soft;
  soft.velocity = 0.2f; hard.velocity = 1.0f;
  CHECK(harmonicActivity(play(hard, 0.5), at(0.1), at(0.5), 400) >
        harmonicActivity(play(soft, 0.5), at(0.1), at(0.5), 400) + 0.05);
}

void testToneFilter() {
  Setup open = flat(0.6f, R_1), closed = open;
  closed.tone = 0.1f;
  CHECK(highShare(play(closed, 0.5), at(0.1), at(0.5), 400) <
        highShare(play(open, 0.5), at(0.1), at(0.5), 400) * 0.5);
  // The cutoff is relative to the note, so the same tone setting keeps the same brightness an
  // octave up (both above the bass-weight range).
  Setup lo = flat(0.5f, R_1), hi = lo;
  lo.tone = 0.5f; hi.tone = 0.5f; hi.pitch = 12;
  const double ha = harmonicActivity(play(lo, 0.5), at(0.1), at(0.5), 400);
  const double hb = harmonicActivity(play(hi, 0.5), at(0.1), at(0.5), 800);
  CHECK_NEAR(ha, hb, 0.1);
  // tone 0 leaves the fundamental dominant.
  Setup dark = open; dark.tone = 0;
  CHECK(harmonicActivity(play(dark, 0.5), at(0.1), at(0.5), 400) < 0.5);
}

// Bass register: the default (ratio 1) keeps its fundamental as the index sweeps, where plain PM
// would pass through Bessel zeros and lose it.
void testBassKeepsItsFundamental() {
  Setup ref = sine(); ref.tune = 55;
  const double carrier = bin(play(ref, 0.6), at(0.1), at(0.6), 55);
  double worst = 1e9;
  for (float t = 0; t <= 1.001f; t += 0.05f) {
    Setup s = ref; s.timbre = t; s.ratio = R_1;
    worst = std::fmin(worst, bin(play(s, 0.6), at(0.1), at(0.6), 55) / carrier);
  }
  CHECK(worst > 0.35);   // never more than ~9 dB under the plain carrier
  // And through a snap sweep, in short windows from the onset on.
  Setup s = ref; s.timbre = 0.35f; s.snap = 1; s.decay = 0.5f;
  Buf o = play(s, 1.0, "bass_fund");
  for (double t0 : {0.01, 0.05, 0.1, 0.2, 0.4, 0.8}) {
    CHECK(bin(o, at(t0), at(t0 + 0.06), 55) > carrier * 0.25);
  }
}

// Continuous retrigger: a note landing on a sounding voice does not step.
void testRetriggerIsContinuous() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kFm, 0);
  Setup s = sine(); s.tune = 110; s.sustain = 0.8f; apply(e, s);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0; Buf o; run(e, f, 0.3, &o);
  e.instrumentNoteOn(1, 1.0f, 0, 5);
  run(e, f, 0.3, &o);
  CHECK(finiteAll(o));
  CHECK(maxStep(o, at(0.3) - 64, at(0.3) + 64) < maxStep(o, at(0.2), at(0.29)) * 2.0);  // pitch is 1.33x
  CHECK_NEAR(peakFreq(o, at(0.4), at(0.6), 110 * std::exp2(5 / 12.0)) / (110 * std::exp2(5 / 12.0)), 1.0, 0.003);
}

// A scheduled note starts exactly on its frame and its duration is sample-accurate.
void testScheduledNoteAndDuration() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kFm, 0);
  Setup s = sine(); s.release = 0.01f; apply(e, s);
  e.scheduleInstrumentNote(4800, 1, 1.0f, 0.0f, 0.0f, 4800);  // start at 0.1 s, held 0.1 s
  int64_t f = 0; Buf o; run(e, f, 0.4, &o);
  size_t first = 0; while (first < o.size() && o[first] == 0.0f) first++;
  CHECK(first >= 4800 && first <= 4801 + 2);
  size_t last = o.size(); while (last > 0 && o[last - 1] == 0.0f) last--;
  CHECK(last > 9600 && last <= 9600 + at(0.012));
}

void testLegatoAndSlide() {
  // Legato on: a note on a held note keeps the envelope (no re-attack), off: it re-attacks.
  auto runPair = [](float legato, float slide) {
    Setup s = sine(); s.tune = 220; s.legato = legato; s.slide = slide; s.attack = 0.002f;
    s.decay = 0.05f; s.sustain = 0.4f;
    Engine e; e.init(kRate, 2, 8);
    e.createInstrument(1, kFm, 0);
    apply(e, s);
    e.instrumentNoteOn(1, 1.0f);
    int64_t f = 0; Buf o; run(e, f, 0.5, &o);
    e.instrumentNoteOn(1, 1.0f, 0, 12);
    run(e, f, 0.5, &o);
    return o;
  };
  Buf leg = runPair(1, 0), non = runPair(0, 0);
  const double held = peak(leg, at(0.4), at(0.5));
  CHECK(peak(leg, at(0.5), at(0.56)) < held * 1.1);           // no bump
  CHECK(peak(non, at(0.5), at(0.56)) > held * 1.5);            // re-attack to full level
  CHECK_NEAR(zcFreq(leg, at(0.6), at(0.9)) / 440.0, 1.0, 0.005);
  // Slide: linear in pitch, lands on the target.
  Buf sl = runPair(1, 0.2f);
  const double early = zcFreq(sl, at(0.5), at(0.54)), late = zcFreq(sl, at(0.66), at(0.7));
  CHECK(early > 225 && early < 300);                           // starting to rise
  CHECK(late > 300 && late < 430);                             // still in motion past halfway
  CHECK_NEAR(zcFreq(sl, at(0.8), at(0.95)) / 440.0, 1.0, 0.005);
  // The held note's velocity is kept.
  Setup s = sine(); s.legato = 1; s.sustain = 1;
  Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kFm, 0); apply(e, s);
  e.instrumentNoteOn(1, 0.8f);
  int64_t f = 0; Buf o; run(e, f, 0.3, &o);
  e.instrumentNoteOn(1, 0.2f, 0, 7);
  run(e, f, 0.3, &o);
  CHECK_NEAR(peak(o, at(0.4), at(0.6)) / peak(o, at(0.1), at(0.3)), 1.0, 0.03);
}

void testRoutesThroughBus() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kFm, /*busId=*/1);
  e.setBusParam(1, static_cast<int32_t>(NodeParam::BusGain), 0.5f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0; Buf o; run(e, f, 0.3, &o);
  Buf direct = play({}, 0.3);
  CHECK(peak(o) > 0.0f);
  CHECK_NEAR(peak(o), peak(direct) * 0.5, 1e-3);
}

// Guards: it must not collapse to "oscillator -> filter -> ADSR".
void testIsNotSubtractive() {
  // 1. Timbre moves while level is flat: spectrum changes a lot, RMS barely.
  Setup s = flat(0.1f, R_1); s.snap = 1; s.decay = 0.3f; s.sustain = 1;
  Buf o = play(s, 1.0);
  CHECK(harmonicActivity(o, at(0.004), at(0.034), 400) - harmonicActivity(o, at(0.8), at(0.83), 400) > 0.4);
  // 2. Hollow, odd-harmonics-only colour (ratio 2): even harmonics absent while odd ones are strong.
  //    A low-pass on a full-spectrum source cannot remove 2f, 4f, 6f and keep 3f, 5f, 7f.
  Buf h = play(flat(0.4f, R_2), 0.6);
  double odd = 0, even = 0;
  for (int k = 3; k <= 9; k += 2) odd += bin(h, at(0.1), at(0.6), 400.0 * k);
  for (int k = 2; k <= 10; k += 2) even += bin(h, at(0.1), at(0.6), 400.0 * k);
  CHECK(odd > 0.1);
  CHECK(even < odd * 0.02);
  // 3. Inharmonic partials exist (metal): energy off the harmonic grid, with the tone fully open.
  Buf m = play(flat(0.5f, R_35), 0.6);
  double off = 0, on = 0;
  for (int k = 1; k <= 12; k++) {
    on += bin(m, at(0.1), at(0.6), 400.0 * k);
    off += bin(m, at(0.1), at(0.6), 400.0 * (k + 0.5));
  }
  CHECK(off > on * 0.1);
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kFm, 1);
  e.setInstrumentParam(1, id(FmParam::Timbre), 0.9f);
  e.setInstrumentParam(1, id(FmParam::Legato), 1.0f);
  e.setInstrumentParam(1, id(FmParam::Slide), 0.1f);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 600; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
    if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, -7);
    if (i == 300) e.instrumentNoteOff(1);
    if (i % 50 == 0) e.setInstrumentParam(1, id(FmParam::Ratio), float(i / 50 % 8));
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

// Dump a few characteristic patches for listening (only when FM_WAV_DIR is set).
void dumpDemoPatches() {
  if (!std::getenv("FM_WAV_DIR")) return;
  Setup bass; bass.tune = 55; bass.ratio = R_1; bass.timbre = 0.3f; bass.snap = 0.8f; bass.decay = 0.25f;
  bass.sustain = 0.5f; bass.duration = 0.4;
  play(bass, 0.8, "demo_bass");
  Setup slap = bass; slap.ratio = R_2; slap.timbre = 0.15f; slap.snap = 1; slap.decay = 0.12f;
  play(slap, 0.8, "demo_hollow_slap");
  Setup lead; lead.tune = 440; lead.ratio = R_3; lead.timbre = 0.45f; lead.snap = 0.4f; lead.sustain = 0.8f;
  lead.attack = 0.01f; lead.release = 0.3f; lead.duration = 0.8;
  play(lead, 1.4, "demo_lead");
  Setup bell = lead; bell.ratio = R_35; bell.timbre = 0.35f; bell.snap = 0.7f; bell.sustain = 0.2f; bell.decay = 1.0f;
  play(bell, 1.6, "demo_bell");
}

}  // namespace

int main() {
  testProducesFiniteBoundedAudio();
  testDeterministic();
  testCarrierPitch();
  testModulatorFollowsCarrier();
  testZeroModulationIsCarrierOnly();
  testTimbreRaisesHarmonicActivity();
  testSnapIsAnIndexEnvelope();
  testAmplitudeEnvelope();
  testReleaseIsContinuous();
  testParamBoundsAndMetadata();
  testPolyphony();
  testStabilityFuzz();
  testBandwidthGuard();
  testVelocity();
  testToneFilter();
  testBassKeepsItsFundamental();
  testRetriggerIsContinuous();
  testScheduledNoteAndDuration();
  testLegatoAndSlide();
  testRoutesThroughBus();
  testIsNotSubtractive();
  testRenderDoesNotAllocate();
  dumpDemoPatches();
  std::printf("fm_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
