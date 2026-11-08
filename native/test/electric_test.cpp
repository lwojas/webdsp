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

// Covers the Electric instrument (instruments/electric.h) through the Engine's instrument API: registration,
// parameter table, lifecycle, determinism, what each macro does to the envelope and the spectrum (strike, decay,
// body, bell, warmth, age, crush, cutoff, resonance, velocity, width), chords and overlapping tails, loudness
// stability across every macro, click-free parameter jumps, extremes, other sample rates, and allocation.
// DSP checks are sanity measurements (band levels, envelope ratios), never exact samples.
// Set ELECTRIC_WAV_DIR=<dir> to dump the validation sounds as stereo 16-bit WAVs for listening
// (docs/electric-instrument.md).
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
constexpr int32_t kEl = static_cast<int32_t>(InstrumentKind::Electric);
constexpr double kPi = 3.14159265358979323846;
constexpr double kF0 = 261.63;  // the default Tune

using Buf = std::vector<float>;
using P = ElectricParam;
int32_t id(P p) { return static_cast<int32_t>(p); }
size_t at(double s) { return static_cast<size_t>(s * kRate); }

struct Out {
  Buf l, r;
};

struct Rig {
  Engine e;
  int64_t f = 0;
  Out o;
  explicit Rig(int32_t instruments = 1, double rate = kRate) : rate_(rate) {
    e.init(rate, 2, 8);
    o.l.reserve(1 << 22);
    o.r.reserve(1 << 22);
    for (int32_t i = 1; i <= instruments; i++) e.createInstrument(i, kEl, 0);
  }
  void set(P p, float v, int32_t inst = 1) { e.setInstrumentParam(inst, id(p), v); }
  void go(double seconds) {
    const int32_t frames = static_cast<int32_t>(std::ceil(seconds * rate_ / kBlock)) * kBlock;
    for (int32_t d = 0; d < frames; d += kBlock) {
      e.process(f, kBlock);
      f += kBlock;
      o.l.insert(o.l.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
      o.r.insert(o.r.end(), e.outputChannel(1), e.outputChannel(1) + kBlock);
    }
  }
  double rate_;
};

void writeWav(const char* name, const Out& b) {
  const char* dir = std::getenv("ELECTRIC_WAV_DIR");
  if (!dir) return;
  std::string path = std::string(dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(b.l.size()), bytes = n * 4, rate = 48000;
  const uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
  const uint32_t riff = 36 + bytes, fmt = 16, byteRate = rate * 4;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    const int16_t a = static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, b.l[i])) * 32767);
    const int16_t c = static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, b.r[i])) * 32767);
    std::fwrite(&a, 2, 1, f);
    std::fwrite(&c, 2, 1, f);
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
double ampDb(double r) { return 20.0 * std::log10(std::fmax(r, 1e-30)); }

// Power of the windowed FFT of v[a, a+n) summed into band [lo, hi) Hz.
double bandPower(const Buf& v, size_t a, double lo, double hi, size_t n = 16384, double rate = kRate) {
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
    const double f = double(i) * rate / double(n);
    if (f >= lo && f < hi) t += std::norm(x[i]);
  }
  return t;
}
// Power in a band around ratio * f0 (+-4 %).
double partial(const Buf& v, size_t a, double ratio, double f0 = kF0) {
  return bandPower(v, a, ratio * f0 * 0.96, ratio * f0 * 1.04);
}
// Spectral centroid (Hz) of [a, a+n) up to 16 kHz.
double centroid(const Buf& v, size_t a) {
  double num = 0, den = 0;
  for (double f = 200; f < 16000; f *= 1.12) {
    const double p = bandPower(v, a, f, f * 1.12);
    num += p * f * 1.06;
    den += p;
  }
  return num / std::fmax(den, 1e-30);
}

using Params = std::vector<std::pair<P, float>>;
// One note on instrument 1. duration 0 = held until it dies away by itself.
Out render(const Params& params, double seconds = 2.0, float pitch = 0.0f, float velocity = 1.0f,
           double duration = 0.0) {
  Rig r;
  for (auto& [p, v] : params) r.set(p, v);
  if (duration > 0) r.e.instrumentNoteOn(1, velocity, 0.0f, pitch, static_cast<int32_t>(duration * kRate));
  else r.e.instrumentNoteOn(1, velocity, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}
// Level (dB) of the first 0.4 s after a 20 ms settle: what the loudness trims are measured on.
double earlyDb(const Params& params, float vel = 1.0f) {
  const Out o = render(params, 0.6, 0.0f, vel);
  return ampDb(rms(o.l, at(0.02), at(0.4)));
}

// Time (s) for the envelope of v to fall below `fraction` of its peak, measured on 10 ms RMS windows.
double timeToFall(const Buf& v, double fraction) {
  const size_t win = at(0.01);
  double pk = 0;
  std::vector<double> env;
  for (size_t a = 0; a + win <= v.size(); a += win) {
    env.push_back(rms(v, a, a + win));
    pk = std::fmax(pk, env.back());
  }
  size_t imax = 0;
  for (size_t i = 0; i < env.size(); i++) if (env[i] == pk) imax = i;
  for (size_t i = imax; i < env.size(); i++) if (env[i] < fraction * pk) return double(i) * 0.01;
  return double(env.size()) * 0.01;
}

// Fundamental frequency from upward zero crossings in a mono, low-passed (cutoff 400) render.
double freqOf(const Buf& v, size_t a, size_t n) {
  int32_t crossings = 0;
  double first = -1, last = -1;
  for (size_t i = a + 1; i < a + n; i++) {
    if (v[i - 1] < 0 && v[i] >= 0) {
      const double t = double(i - 1) + double(-v[i - 1]) / double(v[i] - v[i - 1]);
      if (first < 0) first = t;
      last = t;
      crossings++;
    }
  }
  return crossings < 3 ? 0.0 : (crossings - 1) * kRate / (last - first);
}

void testRegistrationAndDefaults() {
  const InstrumentKindMeta* m = instrumentKindMeta(kEl);
  CHECK(m && std::strcmp(m->id, "electric") == 0 && std::strcmp(m->name, "Electric") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(kEl == 21 && instrumentParamCount(kEl) == kElectricParamCount + 1 && kElectricParamCount == 12);
  auto in = makeInstrument(kEl);
  CHECK(in && in->kind() == InstrumentKind::Electric && !in->isActive());
  for (int32_t i = 0; i < kElectricParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kEl, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max && p->min < p->max);
  }
  CHECK(instrumentParamInfo(kEl, kElectricParamCount)->id == kInstrumentGainParam);
  // Stable names: the contract a client reads.
  const char* names[] = {"body", "strike", "bell", "warmth", "age", "crush", "cutoff",
                         "resonance", "decay", "tune", "release", "width"};
  for (int32_t i = 0; i < kElectricParamCount; i++) CHECK(std::strcmp(instrumentParamInfo(kEl, i)->name, names[i]) == 0);
  // The macros are 0..1 and the defaults are a finished sound: a bit of everything, nothing at an end stop.
  for (P q : {P::Body, P::Strike, P::Bell, P::Warmth, P::Age, P::Resonance, P::Decay, P::Width}) {
    const InstrumentParamInfo* p = instrumentParamInfo(kEl, id(q));
    CHECK(p->min == 0.0f && p->max == 1.0f && p->def > 0.1f && p->def < 0.9f);
  }
  CHECK(instrumentParamInfo(kEl, id(P::Crush))->def < 0.2f);  // almost clean out of the box
  CHECK(instrumentParamInfo(kEl, id(P::Cutoff))->def > 3000.0f);
  CHECK(resolveModDomain(kElectricParams[id(P::Cutoff)]) == ModDomain::Log);
  CHECK(resolveModDomain(kElectricParams[id(P::Tune)]) == ModDomain::Log);
  CHECK(resolveModDomain(kElectricParams[id(P::Bell)]) == ModDomain::Linear);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kEl, 0));
}

// Every parameter reaches the DSP: moving it off its default changes the rendered note.
void testEveryParameterPropagates() {
  Rig ref;
  ref.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
  ref.go(0.8);
  for (int32_t i = 0; i < kElectricParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kEl, i);
    const float target = std::fabs(p->def - p->min) > std::fabs(p->def - p->max) ? p->min : p->max;
    Rig r;
    r.e.setInstrumentParam(1, i, target);
    r.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
    r.go(0.8);
    CHECK(finite(r.o.l) && finite(r.o.r));
    if (r.o.l == ref.o.l) std::fprintf(stderr, "param %s has no effect\n", p->name);
    CHECK(r.o.l != ref.o.l);
  }
}

void testParamValidation() {
  Rig r;
  r.set(P::Bell, 7.0f);
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Bell)), 1.0, 1e-6);
  r.set(P::Cutoff, -5.0f);
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Cutoff)), 80.0, 1e-6);
  r.set(P::Tune, std::numeric_limits<float>::quiet_NaN());
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Tune)), 261.63, 1e-4);
  r.set(P::Warmth, std::numeric_limits<float>::infinity());
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Warmth)), 0.4, 1e-6);
  r.e.setInstrumentParam(1, 999, 1.0f);  // unknown id: no effect
}

void testLifecycle() {
  // A note sounds from the first milliseconds, rings on its own and ends in true silence; nothing sticks.
  const Out a = render({{P::Decay, 0.2f}}, 6.0);
  CHECK(finite(a.l) && finite(a.r));
  CHECK(peak(a.l, 0, at(0.05)) > 0.05f);
  CHECK(peak(a.l, at(5.5), at(6.0)) == 0.0f);
  // Held: still sounding after a second at the default decay; a note-off releases it within the release time.
  Rig r;
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(1.0);
  CHECK(rms(r.o.l, at(0.8), at(1.0)) > 0.005);
  r.e.instrumentNoteOff(1);
  r.go(0.8);
  CHECK(peak(r.o.l, at(1.7), at(1.8)) == 0.0f);  // release 0.35 s x (0.5 + 0.5)
  CHECK(rms(r.o.l, at(1.0), at(1.05)) > rms(r.o.l, at(1.25), at(1.3)));
  // A timed note ends by itself, and Release is honoured.
  const Out t = render({}, 1.2, 0.0f, 1.0f, 0.25);
  CHECK(peak(t.l, at(1.0), at(1.2)) == 0.0f);
  const Out lr = render({{P::Release, 3.0f}, {P::Decay, 1.0f}}, 3.0, 0.0f, 1.0f, 0.25);
  CHECK(rms(lr.l, at(1.0), at(1.2)) > 0.005);
  // Velocity: monotonic in level; velocity 0 is silent, not an error.
  const double v2 = earlyDb({}, 0.2f), v5 = earlyDb({}, 0.5f), v8 = earlyDb({}, 0.8f), v10 = earlyDb({}, 1.0f);
  CHECK(v2 < v5 && v5 < v8 && v8 < v10);
  CHECK(peak(render({}, 0.5, 0.0f, 0.0f).l) == 0.0f);
  // Pre-note delay: silence until the event, then sound.
  Rig d;
  d.e.scheduleInstrumentNote(at(0.1), 1, 1.0f);
  d.go(0.4);
  CHECK(peak(d.o.l, 0, at(0.099)) == 0.0f && peak(d.o.l, at(0.11), at(0.4)) > 0.05f);
}

void testDeterminismAndPitch() {
  const Out a = render({}, 1.0), b = render({}, 1.0);
  CHECK(a.l == b.l && a.r == b.r);
  // Pitch is semitones on top of Tune: +12 doubles the fundamental.
  const Params clean = {{P::Bell, 0.0f}, {P::Strike, 0.2f}, {P::Age, 0.0f}, {P::Cutoff, 18000.0f}};
  const Out lo = render(clean, 1.0, 0.0f), hi = render(clean, 1.0, 12.0f);
  CHECK(bandPower(lo.l, at(0.3), 250, 275) > 20.0 * bandPower(lo.l, at(0.3), 510, 540) * 0 +
                                              bandPower(lo.l, at(0.3), 120, 135));
  CHECK(bandPower(hi.l, at(0.3), 510, 545) > 5.0 * bandPower(hi.l, at(0.3), 250, 275));
  CHECK_NEAR(freqOf(render({{P::Cutoff, 400.0f}, {P::Bell, 0.0f}, {P::Age, 0.0f}, {P::Body, 0.3f}}, 0.8).l, at(0.3), at(0.3)) / kF0, 1.0, 0.02);
  // Tune moves the root.
  const Out tu = render({{P::Tune, 523.25f}, {P::Cutoff, 18000.0f}, {P::Bell, 0.0f}}, 1.0);
  CHECK(bandPower(tu.l, at(0.3), 510, 545) > 5.0 * bandPower(tu.l, at(0.3), 250, 275));
}

void testStrikeAndDecayEnvelope() {
  // Strike: a harder strike is a stronger, brighter, shorter-lived transient relative to the sustain; a soft one
  // takes longer to arrive.
  auto attackRatio = [&](float strike) {
    const Out o = render({{P::Strike, strike}}, 1.0);
    return ampDb(rms(o.l, 0, at(0.03))) - ampDb(rms(o.l, at(0.3), at(0.5)));
  };
  const double sLow = attackRatio(0.0f), sMid = attackRatio(0.5f), sHigh = attackRatio(1.0f);
  CHECK(sLow < sMid && sMid < sHigh);
  CHECK(sHigh - sLow > 6.0);
  auto riseTime = [&](float strike) {
    const Out o = render({{P::Strike, strike}}, 0.5);
    float pk = peak(o.l, 0, at(0.15));
    for (size_t i = 0; i < at(0.15); i++) if (std::fabs(o.l[i]) >= 0.5f * pk) return double(i) / kRate;
    return 0.15;
  };
  CHECK(riseTime(0.0f) > riseTime(1.0f) + 0.003);
  auto hf = [&](float strike) {
    const Out o = render({{P::Strike, strike}, {P::Cutoff, 18000.0f}}, 0.5);
    return dbOf(bandPower(o.l, at(0.0), 3000, 12000, 2048) / bandPower(o.l, at(0.0), 100, 800, 2048));
  };
  CHECK(hf(1.0f) > hf(0.0f) + 6.0);  // the tick and the tine burst
  // Decay: the transition from a short key to a sustained one.
  auto fall = [&](float decay) { return timeToFall(render({{P::Decay, decay}}, 12.0).l, 0.1); };
  const double d0 = fall(0.0f), d25 = fall(0.25f), d5 = fall(0.5f), d1 = fall(1.0f);
  CHECK(d0 < d25 && d25 < d5 && d5 < d1);
  CHECK(d0 < 0.6 && d5 > 0.8 && d5 < 4.0 && d1 > 6.0);
  // A percussive key at Decay 0 still has the same strike: peak level is not decay-dependent.
  CHECK_NEAR(ampDb(peak(render({{P::Decay, 0.0f}}, 0.5).l)), ampDb(peak(render({{P::Decay, 1.0f}}, 0.5).l)), 2.5);
}

void testBodyBellWarmth() {
  const size_t a = at(0.25);
  // Body: the fundamental and the sub gain on the upper partials.
  const Params base = {{P::Bell, 0.0f}, {P::Strike, 0.3f}, {P::Cutoff, 18000.0f}};
  auto withBody = [&](float b) { Params p = base; p.push_back({P::Body, b}); return render(p, 0.8); };
  const Out thin = withBody(0.0f), full = withBody(1.0f);
  const double fundThin = dbOf(partial(thin.l, a, 1.0)), upThin = dbOf(partial(thin.l, a, 2.0));
  const double fundFull = dbOf(partial(full.l, a, 1.0)), upFull = dbOf(partial(full.l, a, 2.0));
  CHECK(fundFull - upFull > fundThin - upThin + 6.0);
  CHECK(dbOf(partial(full.l, a, 0.5)) > dbOf(partial(thin.l, a, 0.5)) + 10.0);  // the sub octave

  // Bell: inharmonic partials appear (1.6 x and 9.6 x f0 are not harmonics) while the harmonic ones barely move.
  auto withBell = [&](float b) { Params p = {{P::Cutoff, 18000.0f}, {P::Bell, b}}; return render(p, 1.5); };
  const Out b0 = withBell(0.0f), b5 = withBell(0.5f), b10 = withBell(1.0f);
  auto inh = [&](const Out& o) { return dbOf(partial(o.l, at(0.12), 1.6) + partial(o.l, at(0.12), 9.6)); };
  CHECK(inh(b5) > inh(b0) + 12.0);
  CHECK(inh(b10) > inh(b5) + 6.0);
  CHECK(dbOf(partial(b10.l, at(0.12), 1.6)) > dbOf(partial(b10.l, at(0.12), 1.0)) - 15.0);  // genuinely metallic
  // ...and not an EQ: with the filter shut above 2 kHz, the bell is still audible below it (1.6 x is pure FM).
  const Out closed = render({{P::Cutoff, 2200.0f}, {P::Bell, 1.0f}}, 0.8);
  CHECK(dbOf(partial(closed.l, at(0.12), 1.6)) > dbOf(partial(b0.l, at(0.12), 1.6)) + 15.0);
  // The bell rings shorter than the body: late in the note the inharmonic part has faded more than the fundamental.
  const double earlyInh = dbOf(partial(b10.l, at(0.1), 1.6)), lateInh = dbOf(partial(b10.l, at(1.0), 1.6));
  const double earlyF = dbOf(partial(b10.l, at(0.1), 1.0)), lateF = dbOf(partial(b10.l, at(1.0), 1.0));
  CHECK(earlyInh - lateInh > earlyF - lateF + 3.0);

  // Warmth: more harmonics (3 x .. 8 x), at a steady level.
  auto harmonics = [&](float w) {
    Params p = {{P::Bell, 0.0f}, {P::Warmth, w}, {P::Cutoff, 18000.0f}, {P::Crush, 0.0f}};
    const Out o = render(p, 0.9);
    double up = 0;
    for (int32_t h = 3; h <= 8; h++) up += partial(o.l, at(0.4), double(h));
    return dbOf(up / partial(o.l, at(0.4), 1.0));
  };
  CHECK(harmonics(1.0f) > harmonics(0.0f) + 6.0);
  CHECK(harmonics(0.5f) > harmonics(0.0f) + 2.0);
  CHECK(std::fabs(earlyDb({{P::Warmth, 1.0f}}) - earlyDb({{P::Warmth, 0.0f}})) < 1.5);
}

void testAge() {
  // Age 0 is precise: the pitch holds. Age 1 wanders a few cents; never more than ~12.
  auto pitchTrack = [&](float age, float& minC, float& maxC) {
    const Out o = render({{P::Age, age}, {P::Warmth, 0.0f}, {P::Bell, 0.0f}, {P::Cutoff, 400.0f}, {P::Body, 0.3f}, {P::Decay, 1.0f}}, 6.0);
    minC = 1e9f; maxC = -1e9f;
    for (double t = 0.3; t < 5.5; t += 0.5) {
      const double f = freqOf(o.l, at(t), at(0.4));
      const float c = static_cast<float>(1200.0 * std::log2(f / kF0));
      minC = std::min(minC, c); maxC = std::max(maxC, c);
    }
  };
  float lo0, hi0, lo1, hi1;
  pitchTrack(0.0f, lo0, hi0);
  pitchTrack(1.0f, lo1, hi1);
  CHECK(hi0 - lo0 < 1.5f && std::fabs(lo0) < 2.0f && std::fabs(hi0) < 2.0f);
  CHECK(hi1 - lo1 > 1.5f);                     // audibly alive over a long note
  CHECK(std::fabs(lo1) < 14.0f && std::fabs(hi1) < 14.0f);  // restrained, not a lo-fi wobble
  // Successive notes in one instance differ a little (never the same twice); a fresh instance repeats exactly.
  Rig r;
  r.set(P::Age, 0.8f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.1 * kRate));
  r.go(0.6);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.1 * kRate));
  r.go(0.6);
  Buf n1(r.o.l.begin() + at(0.0), r.o.l.begin() + at(0.3)), n2(r.o.l.begin() + at(0.6), r.o.l.begin() + at(0.9));
  CHECK(n1 != n2);
  CHECK_NEAR(ampDb(rms(n1, 0, n1.size())), ampDb(rms(n2, 0, n2.size())), 1.0);
  // With Age 0 two notes are identical.
  Rig z;
  z.set(P::Age, 0.0f);
  z.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.1 * kRate));
  z.go(0.6);
  z.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.1 * kRate));
  z.go(0.6);
  double diff = 0;
  for (size_t i = 0; i < at(0.3); i++) diff = std::fmax(diff, std::fabs(z.o.l[i] - z.o.l[i + at(0.6)]));
  CHECK(diff < 1e-4);
}

void testCrush() {
  // Crush adds aliased, inharmonic digital content (above and between the harmonics) and keeps the level.
  const Params base = {{P::Cutoff, 18000.0f}, {P::Bell, 0.3f}};
  auto digital = [&](float c) {
    Params p = base; p.push_back({P::Crush, c});
    const Out o = render(p, 0.8);
    return dbOf(bandPower(o.l, at(0.1), 9000, 22000));
  };
  CHECK(digital(1.0f) > digital(0.0f) + 6.0);
  CHECK(digital(0.5f) > digital(0.0f) + 1.0);
  CHECK(std::fabs(earlyDb({{P::Crush, 1.0f}}) - earlyDb({{P::Crush, 0.0f}})) < 1.5);
  // Cutoff still tames it: the integrated crush sits before the filter.
  const Out shut = render({{P::Crush, 1.0f}, {P::Cutoff, 1500.0f}}, 0.8);
  CHECK(dbOf(bandPower(shut.l, at(0.1), 9000, 22000)) < digital(1.0f) - 25.0);
}

void testCutoffResonanceVelocity() {
  auto high = [&](float cutoff) {
    const Out o = render({{P::Cutoff, cutoff}}, 0.8);
    return dbOf(bandPower(o.l, at(0.1), 3000, 20000));
  };
  CHECK(high(800.0f) < high(7000.0f) - 12.0);
  CHECK(high(3000.0f) < high(18000.0f));
  // Resonance: a peak at the cutoff.
  auto atCut = [&](float res) {
    const Out o = render({{P::Cutoff, 1500.0f}, {P::Resonance, res}, {P::Bell, 0.8f}, {P::Strike, 0.8f}}, 0.8);
    return dbOf(bandPower(o.l, at(0.0), 1200, 1900, 4096) / bandPower(o.l, at(0.0), 100, 700, 4096));
  };
  CHECK(atCut(1.0f) > atCut(0.0f) + 4.0);
  // The ladder never self-oscillates: with the key up and everything else shut, max resonance rings out.
  const Out ring = render({{P::Resonance, 1.0f}, {P::Cutoff, 900.0f}}, 3.0, 0.0f, 1.0f, 0.1);
  CHECK(peak(ring.l, at(2.0), at(3.0)) < 0.01f);
  // Velocity: a harder touch is brighter, not only louder (compare at matched loudness).
  const Out soft = render({}, 0.6, 0.0f, 0.4f), hard = render({}, 0.6, 0.0f, 1.0f);
  CHECK(centroid(hard.l, at(0.02)) > centroid(soft.l, at(0.02)));
  const double brightSoft = dbOf(bandPower(soft.l, at(0.0), 2500, 16000, 2048) / bandPower(soft.l, at(0.0), 150, 700, 2048));
  const double brightHard = dbOf(bandPower(hard.l, at(0.0), 2500, 16000, 2048) / bandPower(hard.l, at(0.0), 150, 700, 2048));
  CHECK(brightHard > brightSoft + 3.0);
}

void testStereo() {
  // Width 0: exactly mono. Width up: the channels decorrelate while the mono sum stays put and the centre holds.
  const Out mono = render({{P::Width, 0.0f}}, 1.0);
  CHECK(mono.l == mono.r);
  const Out wide = render({{P::Width, 1.0f}}, 1.0), mid = render({{P::Width, 0.6f}}, 1.0);
  auto corr = [&](const Out& o) {
    double lr = 0, ll = 0, rr = 0;
    for (size_t i = at(0.05); i < at(0.9); i++) { lr += o.l[i] * o.r[i]; ll += o.l[i] * o.l[i]; rr += o.r[i] * o.r[i]; }
    return lr / std::sqrt(ll * rr);
  };
  CHECK(corr(wide) < corr(mid) && corr(mid) < 0.995 && corr(wide) > 0.3);  // moderate stereo, never out of phase
  auto monoSum = [&](const Out& o) { Buf s(o.l.size()); for (size_t i = 0; i < s.size(); i++) s[i] = 0.5f * (o.l[i] + o.r[i]); return s; };
  CHECK_NEAR(ampDb(rms(monoSum(wide), at(0.05), at(0.5))), ampDb(rms(monoSum(mono), at(0.05), at(0.5))), 1.5);
  // Pan places the instrument (balance).
  Rig p;
  p.e.instrumentNoteOn(1, 1.0f, 1.0f, 0.0f);
  p.go(0.5);
  CHECK(peak(p.o.l, at(0.1), at(0.5)) < 0.02f * peak(p.o.r, at(0.1), at(0.5)));
}

void testChordsAndTails() {
  // A minor-7 chord on four instances: finite, bounded and fuller than one note.
  Rig c(4);
  const float pitches[4] = {0.0f, 3.0f, 7.0f, 10.0f};
  for (int32_t i = 0; i < 4; i++) c.e.instrumentNoteOn(i + 1, 0.9f, 0.0f, pitches[i], static_cast<int32_t>(0.8 * kRate));
  c.go(3.0);
  CHECK(finite(c.o.l) && finite(c.o.r));
  CHECK(peak(c.o.l) < 1.0f && peak(c.o.l) > 0.3f);
  Rig one;
  one.e.instrumentNoteOn(1, 0.9f, 0.0f, 0.0f, static_cast<int32_t>(0.8 * kRate));
  one.go(3.0);
  CHECK(rms(c.o.l, at(0.1), at(0.6)) > 1.5 * rms(one.o.l, at(0.1), at(0.6)));
  writeWav("chord_cm7", c.o);
  // A new note on the same instance lets the old one ring: the old pitch is still there after the retrigger.
  Rig t;
  t.set(P::Decay, 1.0f);
  t.set(P::Bell, 0.0f);
  t.set(P::Cutoff, 18000.0f);
  t.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  t.go(0.5);
  t.e.instrumentNoteOn(1, 1.0f, 0.0f, 7.0f);
  t.go(0.5);
  auto at8k = [&](double ratio) { return dbOf(bandPower(t.o.l, at(0.52), ratio * kF0 * 0.96, ratio * kF0 * 1.04, 8192)); };
  CHECK(at8k(1.0) > at8k(std::exp2(7.0 / 12.0)) - 20.0);  // the old key still rings under the new one
  CHECK(dbOf(partial(t.o.l, at(1.5), 1.0)) < dbOf(partial(t.o.l, at(0.52), 1.0)) - 20.0);  // ...and dies away
  // The retrigger is click-free: the largest step across the boundary stays in the range of the note itself.
  float maxStep = 0.0f, maxStepNote = 0.0f;
  for (size_t i = at(0.49); i < at(0.55); i++) maxStep = std::fmax(maxStep, std::fabs(t.o.l[i] - t.o.l[i - 1]));
  for (size_t i = at(0.2); i < at(0.26); i++) maxStepNote = std::fmax(maxStepNote, std::fabs(t.o.l[i] - t.o.l[i - 1]));
  CHECK(maxStep < 2.0f * maxStepNote + 0.02f);
  // Stealing: more overlapping notes than slots stays finite and bounded.
  Rig s;
  s.set(P::Decay, 1.0f);
  for (int32_t i = 0; i < 8; i++) {
    s.e.instrumentNoteOn(1, 1.0f, 0.0f, float(i * 2));
    s.go(0.1);
  }
  s.go(1.0);
  CHECK(finite(s.o.l) && finite(s.o.r) && peak(s.o.l) <= 1.0f && peak(s.o.l) > 0.05f);
  // The slots free up again: a long way after the last note (short decay) the instrument is silent.
  Rig q;
  q.set(P::Decay, 0.0f);
  for (int32_t i = 0; i < 4; i++) { q.e.instrumentNoteOn(1, 1.0f, 0.0f, float(i)); q.go(0.05); }
  q.go(3.0);
  CHECK(peak(q.o.l, at(3.0), at(3.2)) == 0.0f);
}

void testGainStability() {
  // The macros change colour, not level: the early level stays within +-1.5 dB of the default across each
  // macro's whole range (Decay and Cutoff excepted: they change the energy by design).
  const double ref = earlyDb({});
  for (P q : {P::Body, P::Strike, P::Bell, P::Warmth, P::Age, P::Crush, P::Resonance, P::Width}) {
    for (float v : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      const double d = earlyDb({{q, v}}) - ref;
      if (std::fabs(d) > 1.5) std::fprintf(stderr, "param %s=%.2f: %+.2f dB\n", instrumentParamInfo(kEl, id(q))->name, v, d);
      CHECK(std::fabs(d) < 1.5);
    }
  }
  // The peak at the default is sensible headroom for a sequenced chord and nothing clips at any end stop.
  const Out d = render({}, 1.0);
  CHECK(peak(d.l) > 0.25f && peak(d.l) < 0.7f);
  uint32_t seed = 12345u;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
  for (int32_t trial = 0; trial < 40; trial++) {
    Params p;
    for (int32_t i = 0; i < kElectricParamCount; i++) {
      const InstrumentParamInfo* pi = instrumentParamInfo(kEl, i);
      const float r = rnd();
      const float v = r < 0.35f ? pi->min : (r < 0.7f ? pi->max : pi->min + rnd() * (pi->max - pi->min));
      p.push_back({static_cast<P>(i), v});
    }
    const Out o = render(p, 1.2, (rnd() - 0.5f) * 60.0f, rnd() * 1.5f);
    CHECK(finite(o.l) && finite(o.r));
    CHECK(peak(o.l) <= 1.0f && peak(o.r) <= 1.0f);
  }
}

void testExtremes() {
  Params lo, hi;
  for (int32_t i = 0; i < kElectricParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kEl, i);
    lo.push_back({static_cast<P>(i), p->min});
    hi.push_back({static_cast<P>(i), p->max});
  }
  for (const Params& p : {lo, hi}) {
    for (float pitch : {-48.0f, 0.0f, 48.0f}) {
      const Out o = render(p, 0.8, pitch, 4.0f);  // a hot velocity
      CHECK(finite(o.l) && finite(o.r) && peak(o.l) <= 1.0f && peak(o.r) <= 1.0f);
    }
  }
  // Non-finite and huge note arguments are tolerated.
  Rig r;
  r.e.instrumentNoteOn(1, std::numeric_limits<float>::quiet_NaN(), 0.0f, std::numeric_limits<float>::infinity());
  r.go(0.3);
  CHECK(finite(r.o.l) && finite(r.o.r));
  // Other sample rates: the same instrument, still finite, still the right pitch.
  for (double rate : {44100.0, 96000.0}) {
    Rig q(1, rate);
    q.set(P::Cutoff, 400.0f); q.set(P::Bell, 0.0f); q.set(P::Age, 0.0f); q.set(P::Body, 0.3f);
    q.e.instrumentNoteOn(1, 1.0f);
    q.go(0.8);
    CHECK(finite(q.o.l) && peak(q.o.l) > 0.05f);
    int32_t crossings = 0;
    for (size_t i = size_t(0.3 * rate) + 1; i < size_t(0.7 * rate); i++) if (q.o.l[i - 1] < 0 && q.o.l[i] >= 0) crossings++;
    CHECK_NEAR(double(crossings) / 0.4, kF0, 8.0);
  }
}

// Parameters jump mid-note without a click: the largest sample step in the 20 ms after the change stays within
// 2x of the largest step the unchanged note makes there (plus a floor), and the output stays finite.
void testSmoothing() {
  for (P q : {P::Body, P::Strike, P::Bell, P::Warmth, P::Age, P::Crush, P::Cutoff, P::Resonance, P::Decay, P::Width}) {
    const InstrumentParamInfo* pi = instrumentParamInfo(kEl, id(q));
    const float target = std::fabs(pi->def - pi->min) > std::fabs(pi->def - pi->max) ? pi->min : pi->max;
    Rig a, b;
    for (Rig* r : {&a, &b}) { r->set(P::Decay, 0.8f); r->e.instrumentNoteOn(1, 1.0f); r->go(0.5); }
    b.set(q, target);
    a.go(0.2); b.go(0.2);
    float stepA = 0, stepB = 0;
    for (size_t i = at(0.5); i < at(0.52); i++) {
      stepA = std::fmax(stepA, std::fabs(a.o.l[i] - a.o.l[i - 1]));
      stepB = std::fmax(stepB, std::fabs(b.o.l[i] - b.o.l[i - 1]));
    }
    if (stepB > 2.0f * stepA + 0.06f) std::fprintf(stderr, "param %s jump: step %.3f vs %.3f\n", pi->name, stepB, stepA);
    CHECK(finite(b.o.l) && stepB < 2.0f * stepA + 0.06f);
  }
}

void testNoAllocationInAudioPath() {
  Rig r(2);
  r.go(0.05);
  const long before = g_allocs;
  for (int32_t i = 0; i < 12; i++) {
    r.e.instrumentNoteOn(1 + i % 2, 0.9f, 0.0f, float(i));
    r.e.setInstrumentParam(1, id(P::Cutoff), 400.0f + 500.0f * i);
    r.e.setInstrumentParam(2, id(P::Crush), i / 12.0f);
    r.go(0.1);
  }
  r.e.instrumentNoteOff(1);
  r.go(0.3);
  CHECK(g_allocs == before);
}

// The starting sounds (mirrored by ELECTRIC_PRESETS in src/runtime/types.ts): finished sounds, distinct, bounded.
struct Preset {
  const char* name;
  Params p;
};
const std::vector<Preset>& presets() {
  static const std::vector<Preset> list = {
      {"electric", {}},
      {"bright", {{P::Body, 0.45f}, {P::Strike, 0.75f}, {P::Bell, 0.65f}, {P::Warmth, 0.3f}, {P::Age, 0.2f},
                  {P::Crush, 0.1f}, {P::Cutoff, 11000.0f}, {P::Resonance, 0.2f}, {P::Decay, 0.45f}}},
      {"warm", {{P::Body, 0.7f}, {P::Strike, 0.35f}, {P::Bell, 0.15f}, {P::Warmth, 0.75f}, {P::Age, 0.4f},
                {P::Crush, 0.05f}, {P::Cutoff, 3800.0f}, {P::Resonance, 0.12f}, {P::Decay, 0.55f}}},
      {"digital", {{P::Body, 0.5f}, {P::Strike, 0.65f}, {P::Bell, 0.7f}, {P::Warmth, 0.2f}, {P::Age, 0.1f},
                   {P::Crush, 0.55f}, {P::Cutoff, 9000.0f}, {P::Resonance, 0.3f}, {P::Decay, 0.4f}}},
      {"keys", {{P::Body, 0.65f}, {P::Strike, 0.2f}, {P::Bell, 0.25f}, {P::Warmth, 0.5f}, {P::Age, 0.3f},
                {P::Crush, 0.05f}, {P::Cutoff, 5200.0f}, {P::Resonance, 0.1f}, {P::Decay, 0.75f}}},
      {"glass", {{P::Body, 0.35f}, {P::Strike, 0.6f}, {P::Bell, 1.0f}, {P::Warmth, 0.15f}, {P::Age, 0.15f},
                 {P::Crush, 0.2f}, {P::Cutoff, 14000.0f}, {P::Resonance, 0.25f}, {P::Decay, 0.6f}}},
  };
  return list;
}

void testPresets() {
  std::vector<double> bright;
  for (const Preset& pr : presets()) {
    const Out o = render(pr.p, 2.0);
    CHECK(finite(o.l) && peak(o.l) > 0.15f && peak(o.l) < 0.8f);
    CHECK(std::fabs(ampDb(rms(o.l, at(0.02), at(0.4))) - ampDb(rms(render({}, 2.0).l, at(0.02), at(0.4)))) < 5.0);
    bright.push_back(centroid(o.l, at(0.02)));
    writeWav((std::string("preset_") + pr.name).c_str(), o);
  }
  // Bright and glass are brighter than warm and keys.
  CHECK(bright[1] > bright[2] * 1.3 && bright[5] > bright[4]);
}

void dumpListeningSet() {
  if (!std::getenv("ELECTRIC_WAV_DIR")) return;
  auto stab = [&](const char* name, const Params& p, float vel) {
    Rig c(4);
    for (int32_t i = 1; i <= 4; i++) for (auto& [q, v] : p) c.set(q, v, i);
    const float pitches[4] = {-12.0f, -5.0f, -2.0f, 3.0f};  // Cm9-ish voicing
    for (int32_t i = 0; i < 4; i++) c.e.instrumentNoteOn(i + 1, vel, 0.0f, pitches[i], static_cast<int32_t>(0.18 * kRate));
    c.go(2.5);
    writeWav(name, c.o);
  };
  stab("stab_default", {}, 0.9f);
  stab("stab_dubtechno", {{P::Strike, 0.7f}, {P::Decay, 0.35f}, {P::Cutoff, 2500.0f}, {P::Resonance, 0.35f}, {P::Warmth, 0.6f}}, 0.9f);
  Rig pad(4);
  for (int32_t i = 1; i <= 4; i++) { pad.set(P::Strike, 0.1f, i); pad.set(P::Decay, 0.95f, i); pad.set(P::Warmth, 0.5f, i); pad.set(P::Cutoff, 3500.0f, i); }
  const float chord[4] = {-12.0f, -5.0f, -2.0f, 3.0f};
  for (int32_t i = 0; i < 4; i++) pad.e.instrumentNoteOn(i + 1, 0.8f, 0.0f, chord[i], static_cast<int32_t>(3.0 * kRate));
  pad.go(6.0);
  writeWav("sustained_chord", pad.o);
  struct Sweep { P q; const char* name; };
  for (Sweep s : {Sweep{P::Body, "body"}, Sweep{P::Strike, "strike"}, Sweep{P::Bell, "bell"}, Sweep{P::Warmth, "warmth"},
                  Sweep{P::Age, "age"}, Sweep{P::Crush, "crush"}, Sweep{P::Resonance, "resonance"}}) {
    Rig r;
    r.e.setInstrumentParam(1, id(P::Decay), 0.5f);
    for (int32_t k = 0; k < 5; k++) {
      r.set(s.q, k / 4.0f);
      r.e.instrumentNoteOn(1, 0.9f, 0.0f, 0.0f, static_cast<int32_t>(0.6 * kRate));
      r.go(1.0);
    }
    writeWav((std::string("sweep_") + s.name).c_str(), r.o);
  }
  Rig line;
  const float mel[8] = {0, 3, 7, 10, 12, 10, 7, 3};
  for (int32_t k = 0; k < 8; k++) {
    line.e.instrumentNoteOn(1, 0.6f + 0.05f * float(k % 4), 0.0f, mel[k], static_cast<int32_t>(0.2 * kRate));
    line.go(0.25);
  }
  line.go(1.5);
  writeWav("melody", line.o);
}

}  // namespace

#ifdef ELECTRIC_DEBUG
void debug() {
  auto hf = [&](float strike) {
    const Out o = render({{P::Strike, strike}, {P::Cutoff, 18000.0f}}, 0.5);
    return dbOf(bandPower(o.l, at(0.0), 3000, 12000, 2048) / bandPower(o.l, at(0.0), 100, 800, 2048));
  };
  for (float w : {0.0f, 0.5f, 1.0f}) {
    Params p = {{P::Bell, 0.0f}, {P::Warmth, w}, {P::Cutoff, 18000.0f}, {P::Crush, 0.0f}};
    const Out o = render(p, 0.9);
    double up = 0;
    for (int32_t h = 3; h <= 8; h++) up += partial(o.l, at(0.4), double(h));
    std::printf("harm w=%.1f %.1f\n", w, dbOf(up / partial(o.l, at(0.4), 1.0)));
  }
  for (float age : {0.0f, 1.0f}) {
    const Out o = render({{P::Age, age}, {P::Warmth, 0.0f}, {P::Bell, 0.0f}, {P::Cutoff, 400.0f}, {P::Body, 0.3f}, {P::Decay, 1.0f}}, 6.0);
    std::printf("age %.0f cents:", age);
    for (double t = 0.3; t < 5.5; t += 0.5) std::printf(" %.1f", 1200.0 * std::log2(freqOf(o.l, at(t), at(0.4)) / kF0));
    std::printf("\n");
  }
  std::printf("hf strike 0/.5/1: %.1f %.1f %.1f\n", hf(0.0f), hf(0.5f), hf(1.0f));
  const Out b10 = render({{P::Cutoff, 18000.0f}, {P::Bell, 1.0f}}, 1.5);
  for (double t : {0.1, 0.3, 0.6, 1.0}) std::printf("bell t=%.1f inh %.1f fund %.1f\n", t, dbOf(partial(b10.l, at(t), 1.6)), dbOf(partial(b10.l, at(t), 1.0)));
  const Out o = render({{P::Crush, 0.9f}, {P::Cutoff, 18000.0f}, {P::Decay, 0.7f}}, 3.0);
  for (double t : {0.1, 0.5, 1.0, 1.5, 2.0}) std::printf("crush t=%.1f hi %.1f fund %.1f\n", t, dbOf(bandPower(o.l, at(t), 6000, 20000)), dbOf(partial(o.l, at(t), 1.0)));
  const Out soft = render({}, 0.6, 0.0f, 0.4f), hard = render({}, 0.6, 0.0f, 1.0f);
  std::printf("centroid soft %.0f hard %.0f\n", centroid(soft.l, at(0.02)), centroid(hard.l, at(0.02)));
  for (float w : {0.0f, 0.6f, 1.0f}) {
    const Out q = render({{P::Width, w}}, 1.0);
    double lr = 0, ll = 0, rr = 0;
    for (size_t i = at(0.05); i < at(0.9); i++) { lr += q.l[i] * q.r[i]; ll += q.l[i] * q.l[i]; rr += q.r[i] * q.r[i]; }
    Buf s(q.l.size()); for (size_t i = 0; i < s.size(); i++) s[i] = 0.5f * (q.l[i] + q.r[i]);
    std::printf("width %.1f corr %.3f L %.1f dB mono %.1f dB\n", w, lr / std::sqrt(ll * rr), ampDb(rms(q.l, at(0.05), at(0.5))), ampDb(rms(s, at(0.05), at(0.5))));
  }
  Rig t;
  t.set(P::Decay, 1.0f); t.set(P::Bell, 0.0f); t.set(P::Cutoff, 18000.0f);
  t.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f); t.go(0.5);
  t.e.instrumentNoteOn(1, 1.0f, 0.0f, 7.0f); t.go(0.5);
  std::printf("tail old %.1f new %.1f\n", dbOf(partial(t.o.l, at(0.8), 1.0)), dbOf(partial(t.o.l, at(0.8), std::exp2(7.0 / 12.0))));
}
#endif

int main() {
#ifdef ELECTRIC_DEBUG
  debug();
  return 0;
#endif
  testRegistrationAndDefaults();
  testEveryParameterPropagates();
  testParamValidation();
  testLifecycle();
  testDeterminismAndPitch();
  testStrikeAndDecayEnvelope();
  testBodyBellWarmth();
  testAge();
  testCrush();
  testCutoffResonanceVelocity();
  testStereo();
  testChordsAndTails();
  testGainStability();
  testExtremes();
  testSmoothing();
  testNoAllocationInAudioPath();
  testPresets();
  dumpListeningSet();
  std::printf("electric_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
