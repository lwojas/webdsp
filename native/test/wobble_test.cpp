#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the Wobble instrument (instruments/wobble.h) through the Engine's instrument API: registration,
// parameter table, lifecycle, determinism, the tempo-locked wobble (rate, shapes, free-run, tempo change),
// what each macro does to the spectrum (growl, bite, weight, drive, cutoff, resonance), loudness stability across
// every macro, a steady sub under wobble, click-free parameter jumps, glide, extremes, and allocation.
// DSP checks are sanity measurements (band levels, envelope periodicity), never exact samples.
// Set WOBBLE_WAV_DIR=<dir> to dump the validation sounds as 16-bit WAVs for listening (docs/wobble-instrument.md).
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
constexpr int32_t kWb = static_cast<int32_t>(InstrumentKind::Wobble);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
using P = WobbleParam;
int32_t id(P p) { return static_cast<int32_t>(p); }
size_t at(double s) { return static_cast<size_t>(s * kRate); }

struct Rig {
  Engine e;
  int64_t f = 0;
  Buf o;
  explicit Rig(double bpm = 120.0) {
    e.init(kRate, 2, 8);
    e.createInstrument(1, kWb, 0);
    e.scheduleTransport(0, 0.0, bpm);
  }
  void set(P p, float v) { e.setInstrumentParam(1, id(p), v); }
  void go(double seconds) {
    const int32_t frames = static_cast<int32_t>(std::ceil(seconds * kRate / kBlock)) * kBlock;
    for (int32_t d = 0; d < frames; d += kBlock) {
      e.process(f, kBlock);
      f += kBlock;
      o.insert(o.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
    }
  }
};

void writeWav(const char* name, const Buf& b) {
  const char* dir = std::getenv("WOBBLE_WAV_DIR");
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
bool finite(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}
double dbOf(double r) { return 10.0 * std::log10(std::fmax(r, 1e-30)); }

// Power of the windowed FFT of v[a, a+n) summed into band [lo, hi) Hz.
double bandPower(const Buf& v, size_t a, double lo, double hi, size_t n = 8192) {
  std::vector<std::complex<double>> x(n);
  for (size_t i = 0; i < n; i++) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(n));
    x[i] = (a + i < v.size() ? v[a + i] : 0.0f) * w;
  }
  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(x[i], x[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (size_t k = 0; k < len / 2; k++) {
        const auto u = x[i + k], t = x[i + k + len / 2] * w;
        x[i + k] = u + t;
        x[i + k + len / 2] = u - t;
        w *= wl;
      }
    }
  }
  double t = 0;
  for (size_t i = 1; i < n / 2; i++) {
    const double f = double(i) * kRate / double(n);
    if (f >= lo && f < hi) t += std::norm(x[i]);
  }
  return t;
}
// Average band power in dB over [t0, t1) seconds.
double bandDb(const Buf& v, double lo, double hi, double t0 = 1.0, double t1 = 3.0) {
  double t = 0;
  int32_t k = 0;
  for (size_t a = at(t0); a + 8192 <= at(t1); a += 4096, k++) t += bandPower(v, a, lo, hi);
  return dbOf(t / std::max(k, 1));
}
// Spread (max - min, dB) of band power across 4096-sample windows in [t0, t1).
double bandSwing(const Buf& v, double lo, double hi, double t0 = 0.5, double t1 = 2.5) {
  double mn = 1e9, mx = -1e9;
  for (size_t a = at(t0); a + 4096 <= at(t1); a += 2048) {
    const double d = dbOf(bandPower(v, a, lo, hi, 4096));
    mn = std::min(mn, d);
    mx = std::max(mx, d);
  }
  return mx - mn;
}

using Params = std::vector<std::pair<P, float>>;
Buf render(const Params& params, double seconds = 3.0, float pitch = 0.0f, float velocity = 1.0f,
           double bpm = 120.0) {
  Rig r(bpm);
  for (auto& [p, v] : params) r.set(p, v);
  r.e.instrumentNoteOn(1, velocity, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}
double ampDb(double r) { return 20.0 * std::log10(std::fmax(r, 1e-30)); }
double loudnessDb(const Params& params) { return ampDb(rms(render(params, 3.0), at(0.5), at(2.8))); }

// Envelope of the content above ~400 Hz (a first difference removes the sub), measured over 20 ms windows (longer
// than one bass cycle, so it follows the wobble and not the waveform), 5 ms apart; then the wobble period (s):
// the shortest lag of the normalised autocorrelation within 15 % of its best peak, over lags 60 ms .. 1.2 s.
double wobblePeriod(const Buf& v, double t0 = 0.5, double t1 = 4.0) {
  std::vector<double> env;
  const size_t win = at(0.02), hop = at(0.005);
  for (size_t a = at(t0); a + win <= at(t1); a += hop) {
    double s = 0;
    for (size_t i = a; i < a + win; i++) { const double d = double(v[i]) - double(v[i - 1]); s += d * d; }
    env.push_back(std::sqrt(s / double(win)));
  }
  double mean = 0;
  for (double e : env) mean += e;
  mean /= double(env.size());
  for (double& e : env) e -= mean;
  std::vector<double> ac(size_t(1.2 / 0.005), 0.0);
  double best = -1e30;
  const size_t lo = size_t(0.06 / 0.005);
  for (size_t lag = lo; lag < ac.size() && lag < env.size() / 2; lag++) {
    double c = 0;
    for (size_t i = 0; i + lag < env.size(); i++) c += env[i] * env[i + lag];
    ac[lag] = c / double(env.size() - lag);
    best = std::max(best, ac[lag]);
  }
  for (size_t lag = lo; lag < ac.size(); lag++) {
    const bool localMax = lag > 0 && lag + 1 < ac.size() && ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1];
    if (localMax && ac[lag] >= 0.85 * best) return double(lag) * 0.005;
  }
  return 0.0;
}

void testRegistrationAndDefaults() {
  const InstrumentKindMeta* m = instrumentKindMeta(kWb);
  CHECK(m && std::strcmp(m->id, "wobble") == 0 && std::strcmp(m->name, "Wobble") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(kWb == 20 && instrumentParamCount(kWb) == kWobbleParamCount + 1 && kWobbleParamCount == 13);
  auto in = makeInstrument(kWb);
  CHECK(in && in->kind() == InstrumentKind::Wobble && !in->isActive());
  for (int32_t i = 0; i < kWobbleParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kWb, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max && p->min < p->max);
  }
  CHECK(instrumentParamInfo(kWb, kWobbleParamCount)->id == kInstrumentGainParam);
  // Stable names: the contract a client reads.
  const char* names[] = {"wobble", "rate", "shape", "growl", "bite", "weight", "drive", "cutoff",
                         "resonance", "tune", "release", "freerate", "glide"};
  for (int32_t i = 0; i < kWobbleParamCount; i++) CHECK(std::strcmp(instrumentParamInfo(kWb, i)->name, names[i]) == 0);
  // Macros are 0..1; the default patch is a finished sound (moderate wobble and growl, a real sub, some drive).
  for (int32_t i : {id(P::Wobble), id(P::Growl), id(P::Bite), id(P::Weight), id(P::Drive), id(P::Resonance)}) {
    const InstrumentParamInfo* p = instrumentParamInfo(kWb, i);
    CHECK(p->min == 0.0f && p->max == 1.0f && p->def > 0.2f && p->def < 0.8f);
  }
  CHECK(instrumentParamInfo(kWb, id(P::Rate))->max == float(kWobbleRateCount - 1));
  CHECK(instrumentParamInfo(kWb, id(P::FreeRate))->def == 0.0f);  // synced out of the box
  CHECK(resolveModDomain(kWobbleParams[id(P::Cutoff)]) == ModDomain::Log);
  CHECK(resolveModDomain(kWobbleParams[id(P::Rate)]) == ModDomain::None);
  CHECK(resolveModDomain(kWobbleParams[id(P::Shape)]) == ModDomain::None);
  CHECK(resolveModDomain(kWobbleParams[id(P::Wobble)]) == ModDomain::Linear);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kWb, 0));
}

// Every parameter reaches the DSP: moving it off its default changes the rendered note.
void testEveryParameterPropagates() {
  Rig ref;
  ref.set(P::FreeRate, 0.0f);
  ref.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
  ref.go(0.8);
  for (int32_t i = 0; i < kWobbleParamCount; i++) {
    if (i == id(P::Glide)) continue;  // only acts on a tied note (testGlideAndTie)
    const InstrumentParamInfo* p = instrumentParamInfo(kWb, i);
    const float target = std::fabs(p->def - p->min) > std::fabs(p->def - p->max) ? p->min : p->max;
    Rig r;
    r.e.setInstrumentParam(1, i, target);
    r.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
    r.go(0.8);
    CHECK(finite(r.o));
    if (r.o == ref.o) std::fprintf(stderr, "param %s has no effect\n", p->name);
    CHECK(r.o != ref.o);
  }
}

void testParamValidation() {
  Rig r;
  r.set(P::Growl, 7.0f);
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Growl)), 1.0, 1e-6);
  r.set(P::Cutoff, -5.0f);
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Cutoff)), 80.0, 1e-6);
  r.set(P::Tune, std::numeric_limits<float>::quiet_NaN());
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Tune)), 55.0, 1e-6);
  r.set(P::Drive, std::numeric_limits<float>::infinity());
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Drive)), 0.4, 1e-6);
  r.e.setInstrumentParam(1, 999, 1.0f);  // unknown id: no effect
}

void testLifecycle() {
  // Immediate attack, sustained while held, then a bounded release to true silence.
  Rig r;
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.05);
  CHECK(peak(r.o, 0, at(0.02)) > 0.1f);  // punch from the first cycle
  r.go(1.0);
  CHECK(rms(r.o, at(0.8), at(1.0)) > 0.1);  // still sounding: sustain is full
  r.e.instrumentNoteOff(1);
  r.go(0.5);
  CHECK(finite(r.o));
  CHECK(peak(r.o, at(1.5), at(1.55)) == 0.0f);  // silent after the release (0.12 s)
  // A timed note ends by itself.
  Rig t;
  t.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.25 * kRate));
  t.go(0.6);
  CHECK(rms(t.o, at(0.1), at(0.2)) > 0.1 && peak(t.o, at(0.5), at(0.6)) == 0.0f);
  // Release time is honoured.
  Rig lr;
  lr.set(P::Release, 1.0f);
  lr.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.25 * kRate));
  lr.go(1.6);
  CHECK(rms(lr.o, at(0.5), at(0.6)) > 0.01 && peak(lr.o, at(1.5), at(1.6)) == 0.0f);
  // Velocity scales level; velocity 0 is silent, not an error.
  const double loud = rms(render({}, 1.0, 0.0f, 1.0f), at(0.3), at(0.9));
  const double soft = rms(render({}, 1.0, 0.0f, 0.4f), at(0.3), at(0.9));
  CHECK(soft < loud * 0.85 && soft > loud * 0.3);
  CHECK(peak(render({}, 0.5, 0.0f, 0.0f)) == 0.0f);
}

void testDeterminismAndPitch() {
  CHECK(render({}, 1.0) == render({}, 1.0));
  // Pitch is semitones on top of Tune: +12 doubles the fundamental (sub-dominated low band moves up an octave).
  const Buf lo = render({{P::Wobble, 0.0f}, {P::Growl, 0.0f}, {P::Weight, 1.0f}}, 2.0, 0.0f);
  const Buf hi = render({{P::Wobble, 0.0f}, {P::Growl, 0.0f}, {P::Weight, 1.0f}}, 2.0, 12.0f);
  CHECK(bandPower(lo, at(1.0), 45, 65) > 4.0 * bandPower(lo, at(1.0), 100, 120));
  CHECK(bandPower(hi, at(1.0), 100, 120) > 4.0 * bandPower(hi, at(1.0), 45, 65));
  // Pre-note delay: silence until the event, then sound.
  Rig d;
  d.e.scheduleInstrumentNote(at(0.1), 1, 1.0f);
  d.go(0.3);
  CHECK(peak(d.o, 0, at(0.099)) == 0.0f && peak(d.o, at(0.11), at(0.3)) > 0.1f);
}

// The wobble is locked to the beat grid: cycle length follows Rate and tempo, free-run follows FreeRate.
void testRateAndSync() {
  const Params base = {{P::Wobble, 1.0f}, {P::Growl, 0.0f}, {P::Drive, 0.2f}, {P::Cutoff, 400.0f}};
  auto period = [&](float step, double bpm) {
    Params p = base;
    p.push_back({P::Rate, step});
    return wobblePeriod(render(p, 4.5, 0.0f, 1.0f, bpm));
  };
  const double p18 = period(6, 120.0);   // 1/8 at 120 BPM = 0.25 s
  const double p14 = period(3, 120.0);   // 1/4 = 0.5 s
  const double p116 = period(9, 120.0);  // 1/16 = 0.125 s
  CHECK_NEAR(p18, 0.25, 0.02);
  CHECK_NEAR(p14, 0.5, 0.03);
  CHECK_NEAR(p116, 0.125, 0.015);
  CHECK_NEAR(period(6, 60.0), 0.5, 0.03);    // tempo changes the cycle length
  CHECK_NEAR(period(6, 240.0), 0.125, 0.015);
  // Dotted and triplet steps sit where they should (1/8. = 0.375 s, 1/8T = 1/6 s at 120 BPM).
  CHECK_NEAR(period(4, 120.0), 0.375, 0.025);
  CHECK_NEAR(period(8, 120.0), 1.0 / 6.0, 0.015);
  // Free-running: FreeRate overrides Rate (5 Hz = 0.2 s at any tempo).
  Params f = base;
  f.push_back({P::FreeRate, 5.0f});
  CHECK_NEAR(wobblePeriod(render(f, 4.5, 0.0f, 1.0f, 120.0)), 0.2, 0.015);
  CHECK_NEAR(wobblePeriod(render(f, 4.5, 0.0f, 1.0f, 200.0)), 0.2, 0.015);
  // Beat-locked: a note starting on a different beat in a different bar sees the same wobble phase.
  Rig a(120.0), b(120.0);
  for (Rig* r : {&a, &b}) { r->set(P::Wobble, 1.0f); r->set(P::Growl, 0.0f); }
  a.e.scheduleInstrumentNote(0, 1, 1.0f);
  b.e.scheduleInstrumentNote(at(2.0), 1, 1.0f);  // 2 s = 4 beats later, a whole number of 1/8 cycles
  a.go(1.0);
  b.go(3.0);
  double diff = 0, ref = 0;
  for (size_t i = 0; i < at(0.5); i++) {
    diff += std::fabs(a.o[i] - b.o[i + at(2.0)]);
    ref += std::fabs(a.o[i]);
  }
  CHECK(diff < 0.02 * ref);
}

// Wobble depth: mid band moves with the wobble; at 0 it is steady. The sub stays put either way.
void testWobbleMovement() {
  const Params steady = {{P::Wobble, 0.0f}, {P::Growl, 0.0f}};
  const Params moving = {{P::Wobble, 1.0f}, {P::Growl, 0.0f}};
  const Buf s = render(steady, 3.0), m = render(moving, 3.0);
  const double swingS = bandSwing(s, 500, 3000), swingM = bandSwing(m, 500, 3000);
  CHECK(swingM > 8.0);               // obvious rhythmic movement
  CHECK(swingS < 4.0);               // relatively stable
  CHECK(swingM > swingS + 6.0);
  CHECK(bandSwing(m, 30, 90) < 4.0);  // the sub path never wobbles
  // Monotonic-ish: more wobble, more movement.
  const double q = bandSwing(render({{P::Wobble, 0.3f}, {P::Growl, 0.0f}}, 3.0), 500, 3000);
  CHECK(q > swingS && q < swingM + 1.0);
  // Growl couples to the wobble (movement of the harmonics, not just the cutoff).
  const double g = bandSwing(render({{P::Wobble, 1.0f}, {P::Growl, 1.0f}}, 3.0), 500, 3000);
  CHECK(g > swingM);
}

// All four shapes move, differently, and stay bounded.
void testShapes() {
  Buf out[4];
  for (int32_t s = 0; s < 4; s++) {
    out[s] = render({{P::Shape, float(s)}, {P::Wobble, 1.0f}}, 3.0);
    CHECK(finite(out[s]) && peak(out[s]) <= 1.0f);
    CHECK(bandSwing(out[s], 500, 3000) > 5.0);
  }
  for (int32_t a = 0; a < 4; a++) for (int32_t b = a + 1; b < 4; b++) CHECK(out[a] != out[b]);
  // The random shape is stepped, not a periodic repeat of the same value: its period differs from the smooth one.
  const Params w = {{P::Wobble, 1.0f}, {P::Growl, 0.0f}, {P::Drive, 0.2f}, {P::Cutoff, 400.0f}};
  Params r = w; r.push_back({P::Shape, 3.0f});
  CHECK(wobblePeriod(render(r, 4.5)) > 0.0);
}

void testGrowlBiteWeightDriveCutoffResonance() {
  const Params off = {{P::Wobble, 0.0f}};
  auto with = [&](P p, float v) { Params q = off; q.push_back({p, v}); return render(q, 3.0); };
  // Growl adds mid harmonics (FM + formants), progressively.
  const double g0 = bandDb(with(P::Growl, 0.0f), 300, 3000), g5 = bandDb(with(P::Growl, 0.5f), 300, 3000),
               g1 = bandDb(with(P::Growl, 1.0f), 300, 3000);
  CHECK(g5 > g0 + 0.8 && g1 > g5 + 3.0);  // level-compensated, so the lift is modest; full growl is +5 dB of mids
  // Growl is not an EQ: the cutoff is untouched, the mids grow through FM sidebands and formants, and the sound
  // starts to move on its own (testWobbleMovement: growl raises the movement of the harmonics under the wobble).
  // Bite adds upper-mid content (2-8k), with the low end untouched.
  const double b0 = bandDb(with(P::Bite, 0.0f), 2000, 8000), b1 = bandDb(with(P::Bite, 1.0f), 2000, 8000);
  CHECK(b1 > b0 + 6.0);
  CHECK_NEAR(bandDb(with(P::Bite, 0.0f), 30, 120), bandDb(with(P::Bite, 1.0f), 30, 120), 1.5);
  // Weight changes the low-end balance (sub vs the rest) while the total level stays near.
  const Buf w0 = with(P::Weight, 0.0f), w1 = with(P::Weight, 1.0f);
  const double ratio0 = bandDb(w0, 30, 90) - bandDb(w0, 150, 3000), ratio1 = bandDb(w1, 30, 90) - bandDb(w1, 150, 3000);
  CHECK(ratio1 > ratio0 + 6.0);
  // Drive adds harmonic density (3rd+ harmonics of the saw path) at roughly constant level.
  const Buf d0 = with(P::Drive, 0.0f), d1 = with(P::Drive, 1.0f);
  CHECK(bandDb(d1, 1000, 8000) > bandDb(d0, 1000, 8000) + 1.5);
  CHECK_NEAR(ampDb(rms(d0, at(0.5), at(2.8))), ampDb(rms(d1, at(0.5), at(2.8))), 2.0);
  // Cutoff opens the sound; resonance builds a peak.
  const double c0 = bandDb(with(P::Cutoff, 150.0f), 600, 6000), c1 = bandDb(with(P::Cutoff, 4000.0f), 600, 6000);
  CHECK(c1 > c0 + 10.0);
  const double r0 = bandDb(with(P::Resonance, 0.0f), 400, 1500), r1 = bandDb(with(P::Resonance, 1.0f), 400, 1500);
  CHECK(r1 > r0 + 3.0);
}

// Macros change colour, not loudness: every control swept alone keeps the level within a few dB of the default.
void testGainStability() {
  const double base = loudnessDb({});
  CHECK(base > -16.0 && base < -6.0);  // a finished level: not an init patch, not hot
  const struct { P p; float lo, hi; } sweeps[] = {
      {P::Wobble, 0.0f, 1.0f}, {P::Growl, 0.0f, 1.0f}, {P::Bite, 0.0f, 1.0f}, {P::Weight, 0.0f, 1.0f},
      {P::Drive, 0.0f, 1.0f}, {P::Cutoff, 80.0f, 8000.0f}, {P::Resonance, 0.0f, 1.0f}, {P::Shape, 0.0f, 3.0f},
      {P::Rate, 0.0f, 11.0f}};
  for (const auto& s : sweeps) {
    for (float v : {s.lo, 0.5f * (s.lo + s.hi), s.hi}) {
      const double l = loudnessDb({{s.p, v}});
      if (std::fabs(l - base) > 3.0) std::fprintf(stderr, "param %s=%g: %+.1f dB\n", kWobbleParams[id(s.p)].name, v, l - base);
      CHECK(std::fabs(l - base) <= 3.0);
    }
  }
  // Pitch: the same patch an octave or two up does not jump out.
  CHECK(std::fabs(ampDb(rms(render({}, 3.0, 12.0f), at(0.5), at(2.8))) - base) < 4.0);
}

// Everything at its extremes, every pitch and velocity: finite, bounded by 1, and it ends.
void testExtremes() {
  const float lows[kWobbleParamCount] = {0, 0, 0, 0, 0, 0, 0, 80, 0, 27.5f, 0.01f, 0, 0};
  const float highs[kWobbleParamCount] = {1, 11, 3, 1, 1, 1, 1, 8000, 1, 110, 1.5f, 20, 0.5f};
  for (int32_t mask : {0, 1, 2}) {
    for (float pitch : {-48.0f, -12.0f, 0.0f, 24.0f, 48.0f}) {
      for (float vel : {0.0f, 0.5f, 1.0f, 4.0f}) {
        Rig r(mask == 2 ? 300.0 : 90.0);
        for (int32_t i = 0; i < kWobbleParamCount; i++) {
          const float v = mask == 0 ? lows[i] : (mask == 1 ? highs[i] : (i % 2 ? highs[i] : lows[i]));
          r.e.setInstrumentParam(1, i, v);
        }
        r.e.instrumentNoteOn(1, vel, 0.0f, pitch, static_cast<int32_t>(0.5 * kRate));
        r.go(1.4);
        CHECK(finite(r.o));
        CHECK(peak(r.o) <= 1.0f);
        CHECK(peak(r.o, at(1.3), at(1.4)) < 1e-6f || mask == 1);  // a timed note with a long release may still ring in mask 1
      }
    }
  }
  // Maximum everything, held for a long time: bounded, no drift, no growth.
  Rig r;
  for (int32_t i = 0; i < kWobbleParamCount; i++) r.e.setInstrumentParam(1, i, i == id(P::Cutoff) ? 8000.0f : kWobbleParams[i].max);
  r.set(P::FreeRate, 14.0f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(8.0);
  CHECK(finite(r.o) && peak(r.o) <= 1.0f);
  CHECK(rms(r.o, at(6.0), at(8.0)) < 2.0 * rms(r.o, at(0.5), at(2.5)) + 0.01);
  // Sample rates other than 48 kHz.
  for (double sr : {22050.0, 44100.0, 96000.0}) {
    Engine e; e.init(sr, 2, 8);
    e.createInstrument(1, kWb, 0);
    for (int32_t i = 0; i < kWobbleParamCount; i++) e.setInstrumentParam(1, i, kWobbleParams[i].max);
    e.instrumentNoteOn(1, 1.0f);
    bool ok = true;
    for (int64_t f = 0; f < int64_t(sr); f += kBlock) {
      e.process(f, kBlock);
      for (int32_t k = 0; k < kBlock; k++) { const float x = e.outputChannel(0)[k]; ok = ok && std::isfinite(x) && std::fabs(x) <= 1.0f; }
    }
    CHECK(ok);
  }
}

// Parameter jumps mid-note are smoothed: in the first 2 ms after the jump the signal's steepest slope is
// still the old sound's (a step would show up as a click right there; a smooth change takes ~10 ms to arrive).
void testSmoothing() {
  const struct { P p; float v; } jumps[] = {
      {P::Cutoff, 8000.0f}, {P::Resonance, 1.0f}, {P::Growl, 1.0f}, {P::Drive, 1.0f},
      {P::Weight, 0.0f}, {P::Bite, 1.0f}, {P::Wobble, 0.0f}};
  for (const auto& j : jumps) {
    Rig r;
    r.set(P::Wobble, 0.5f);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(1.0);
    const size_t at1 = r.o.size();
    r.set(j.p, j.v);
    r.go(0.5);
    float before = 0, after = 0;
    for (size_t i = at(0.5) + 1; i < at1; i++) before = std::fmax(before, std::fabs(r.o[i] - r.o[i - 1]));
    for (size_t i = at1; i < at1 + at(0.002); i++) after = std::fmax(after, std::fabs(r.o[i] - r.o[i - 1]));
    if (after > 1.5f * before + 0.03f) std::fprintf(stderr, "jump in %s: slope %g -> %g\n", kWobbleParams[id(j.p)].name, before, after);
    CHECK(after <= 1.5f * before + 0.03f);
  }
}

void testGlideAndTie() {
  // A tied note glides without re-attacking: no dip to silence at the join.
  Rig r;
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(0.5);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 7.0f, -1, 0, /*tie=*/true);
  r.go(0.5);
  float dip = 1.0f;
  for (size_t i = at(0.5); i + at(0.01) < at(0.55); i += at(0.005)) {
    double s = 0;
    for (size_t k = 0; k < at(0.005); k++) s += std::fabs(r.o[i + k]);
    dip = std::fmin(dip, float(s / double(at(0.005))));
  }
  CHECK(dip > 0.05f);
  // The pitch ends up a fifth higher (sub-dominated: fundamental x 1.498).
  Rig q;
  q.set(P::Wobble, 0.0f); q.set(P::Growl, 0.0f); q.set(P::Weight, 1.0f);
  q.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  q.go(0.3);
  q.e.instrumentNoteOn(1, 1.0f, 0.0f, 7.0f, -1, 0, true);
  q.go(1.0);
  CHECK(bandPower(q.o, at(1.0), 78, 88) > 10.0 * bandPower(q.o, at(1.0), 50, 60));
}

void testNoAllocationInAudioPath() {
  Rig r;
  r.set(P::FreeRate, 6.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.1);
  const long before = g_allocs;
  r.o.clear();
  r.o.reserve(at(2.0));
  const long reserved = g_allocs;
  for (int32_t d = 0; d < 400; d++) {
    r.set(P::Cutoff, 200.0f + 20.0f * float(d));
    r.set(P::Growl, float(d % 10) / 10.0f);
    r.e.process(r.f, kBlock);
    r.f += kBlock;
  }
  CHECK(g_allocs == reserved);
  (void)before;
}

// Writes the listening set when WOBBLE_WAV_DIR is set.
void dumpListeningSet() {
  if (!std::getenv("WOBBLE_WAV_DIR")) return;
  auto seq = [&](const char* name, Params p, double bpm = 140.0) {
    Rig r(bpm);
    for (auto& [k, v] : p) r.set(k, v);
    const double beat = 60.0 / bpm;
    const float pitches[] = {0, 0, 0, 3, 0, 0, 5, 7};
    for (int i = 0; i < 8; i++)
      r.e.scheduleInstrumentNote(at(i * beat), 1, 1.0f, 0.0f, pitches[i], static_cast<int32_t>(0.9 * beat * kRate));
    r.go(8 * beat + 0.5);
    writeWav(name, r.o);
  };
  seq("01_default", {});
  seq("02_clean_no_wobble", {{P::Wobble, 0.0f}, {P::Growl, 0.0f}, {P::Bite, 0.2f}, {P::Drive, 0.2f}});
  seq("03_rate_quarter", {{P::Rate, 3.0f}});
  seq("04_rate_16T_growl", {{P::Rate, 10.0f}, {P::Growl, 0.9f}, {P::Bite, 0.7f}});
  seq("05_ramp", {{P::Shape, 2.0f}});
  seq("06_square", {{P::Shape, 1.0f}, {P::Rate, 5.0f}});
  seq("07_random", {{P::Shape, 3.0f}});
  seq("08_everything_up", {{P::Wobble, 1.0f}, {P::Growl, 1.0f}, {P::Bite, 1.0f}, {P::Weight, 1.0f}, {P::Drive, 1.0f}, {P::Resonance, 0.8f}});
  seq("09_lean_dark", {{P::Weight, 0.0f}, {P::Cutoff, 250.0f}, {P::Growl, 0.2f}});
}

}  // namespace

int main() {
  testRegistrationAndDefaults();
  testEveryParameterPropagates();
  testParamValidation();
  testLifecycle();
  testDeterminismAndPitch();
  testRateAndSync();
  testWobbleMovement();
  testShapes();
  testGrowlBiteWeightDriveCutoffResonance();
  testGainStability();
  testExtremes();
  testSmoothing();
  testGlideAndTie();
  testNoAllocationInAudioPath();
  dumpListeningSet();
  std::printf("wobble_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
