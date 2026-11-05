#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the Noise instrument (instruments/noise.h) through the Engine's instrument API: registration,
// parameter table and propagation, lifecycle, determinism, the six sources' spectral/temporal identity,
// colour, texture, the resonator bank (tuning, spread, sweep, ring), bounded drive/feedback, extremes, and
// allocation. DSP checks are sanity measurements (spectral slopes, peaks, crest factors), never exact samples.
// Set NOISE_WAV_DIR=<dir> to dump the seven validation sounds as 16-bit WAVs for listening (docs/noise-instrument.md).
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
constexpr int32_t kNs = static_cast<int32_t>(InstrumentKind::Noise);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
using P = NoiseParam;
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
  const char* dir = std::getenv("NOISE_WAV_DIR");
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
  explicit Rig(int32_t bus = 0) { e.init(kRate, 2, 8); e.createInstrument(1, kNs, bus); }
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
bool finite(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}

// ---- spectrum helpers: a Hann-windowed radix-2 power spectrum over [a, a + n).
using Spec = std::vector<double>;
Spec spectrum(const Buf& v, size_t a, size_t n = 16384) {
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
  Spec p(n / 2);
  for (size_t i = 0; i < n / 2; i++) p[i] = std::norm(x[i]);
  return p;
}
// Welch estimate: Hann segments of n, 50 % overlap, averaged from `a` for `segments` segments. Narrow high-Q
// bands wander in level over a second, so tonal checks average several seconds rather than trust one window.
Spec welch(const Buf& v, size_t a, size_t segments, size_t n = 16384) {
  Spec acc(n / 2, 0.0);
  for (size_t k = 0; k < segments; k++) {
    const Spec s = spectrum(v, a + k * n / 2, n);
    for (size_t i = 0; i < acc.size(); i++) acc[i] += s[i];
  }
  return acc;
}
double binHz(size_t n2) { return kRate / (2.0 * double(n2)); }
// Mean power per bin (= per Hz) in [lo, hi) Hz.
double psd(const Spec& s, double lo, double hi) {
  const double hz = binHz(s.size());
  size_t a = std::max<size_t>(1, size_t(lo / hz)), b = std::min(s.size(), size_t(hi / hz));
  double t = 0;
  for (size_t i = a; i < b; i++) t += s[i];
  return t / double(std::max<size_t>(b - a, 1));
}
double dbOf(double r) { return 10.0 * std::log10(std::fmax(r, 1e-30)); }
// Strongest bin (Hz) in [lo, hi) and its level above the median bin of the 100 Hz..16 kHz range (dB).
// Narrow bands are resolution-limited and a single bin is a noisy estimate, so the region is found on a
// 9-bin boxcar and the frequency refined to the strongest raw bin within it.
double peakHz(const Spec& s, double lo, double hi) {
  const double hz = binHz(s.size());
  const size_t a = std::max<size_t>(5, size_t(lo / hz)), b = std::min(s.size() - 5, size_t(hi / hz));
  size_t best = a;
  double bestSm = -1;
  for (size_t i = a; i < b; i++) {
    double sm = 0;
    for (size_t k = i - 4; k <= i + 4; k++) sm += s[k];
    if (sm > bestSm) { bestSm = sm; best = i; }
  }
  size_t raw = best;
  for (size_t k = best - 4; k <= best + 4; k++) if (s[k] > s[raw]) raw = k;
  return double(raw) * hz;
}
double peakOverMedianDb(const Spec& s) {
  const double hz = binHz(s.size());
  std::vector<double> r(s.begin() + size_t(100 / hz), s.begin() + size_t(16000 / hz));
  std::vector<double> sorted = r;
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  return dbOf(*std::max_element(r.begin(), r.end()) / sorted[sorted.size() / 2]);
}
// Geometric / arithmetic mean of the power spectrum, 100 Hz..16 kHz: 0 dB = flat (noise), very negative = tonal.
double flatnessDb(const Spec& s) {
  const double hz = binHz(s.size());
  double lg = 0, am = 0;
  size_t a = size_t(100 / hz), b = size_t(16000 / hz);
  for (size_t i = a; i < b; i++) { lg += std::log(s[i] + 1e-30); am += s[i]; }
  return dbOf(std::exp(lg / double(b - a)) / (am / double(b - a)));
}
double centroid(const Buf& v, size_t a, size_t n = 8192) {
  const Spec s = spectrum(v, a, n);
  const double hz = binHz(s.size());
  double num = 0, den = 0;
  for (size_t i = 1; i < s.size(); i++) { num += double(i) * hz * s[i]; den += s[i]; }
  return num / std::fmax(den, 1e-30);
}

using Params = std::vector<std::pair<P, float>>;
// Reference patch for measurements: raw white noise, everything neutral, held (sustain 1).
const Params kBase = {{P::Source, 0.0f}, {P::Color, 0.5f}, {P::Texture, 0.0f}, {P::Density, 40.0f},
                      {P::Tune, 900.0f}, {P::Resonance, 0.4f}, {P::Spread, 0.45f}, {P::Sweep, 0.0f},
                      {P::Attack, 0.001f}, {P::Decay, 1.0f}, {P::Sustain, 1.0f}, {P::Drive, 0.0f},
                      {P::Tone, 0.5f}, {P::Mix, 0.0f}};

Buf render(Params params, double seconds = 1.0, float pitch = 0.0f, float velocity = 1.0f,
           const Params& base = kBase) {
  Rig r;
  for (auto& [p, v] : base) r.set(p, v);
  for (auto& [p, v] : params) r.set(p, v);
  r.e.instrumentNoteOn(1, velocity, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}

void testRegistrationAndDefaults() {
  const InstrumentKindMeta* m = instrumentKindMeta(kNs);
  CHECK(m && std::strcmp(m->id, "noise") == 0 && std::strcmp(m->name, "Noise") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(kNs == 19 && instrumentParamCount(kNs) == kNoiseParamCount + 1 && kNoiseParamCount == 15);
  auto in = makeInstrument(kNs);
  CHECK(in && in->kind() == InstrumentKind::Noise && !in->isActive());
  for (int32_t i = 0; i < kNoiseParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kNs, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max && p->min < p->max);
  }
  CHECK(instrumentParamInfo(kNs, kNoiseParamCount)->id == kInstrumentGainParam);
  auto find = [](const char* n) { for (int32_t i = 0; i < kNoiseParamCount; i++) if (!std::strcmp(kNoiseParams[i].name, n)) return &kNoiseParams[i]; return (const InstrumentParamInfo*)nullptr; };
  CHECK(find("sustain")->def == 0.0f);                       // a one-shot out of the box
  CHECK(find("attack")->max >= 8.0f && find("decay")->max >= 8.0f);   // risers and long swells
  CHECK(resolveModDomain(*find("tune")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("density")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("color")) == ModDomain::Linear);
  CHECK(resolveModDomain(*find("sweep")) == ModDomain::Linear && find("sweep")->min < 0.0f);
  CHECK(resolveModDomain(*find("source")) == ModDomain::None);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kNs, 0));
}

// Every parameter reaches the DSP: moving it off its default changes the rendered note. The patch
// exercises every stage (a gated, resonated, driven crackle-free source) so no parameter is inert.
void testEveryParameterPropagates() {
  const Params patch = {{P::Texture, 0.5f}, {P::Mix, 0.6f}, {P::Drive, 0.6f}, {P::Sustain, 0.5f},
                        {P::Sweep, 1.0f}, {P::Color, 0.7f}, {P::Tone, 0.6f}};
  Rig ref;
  for (auto& [p, v] : patch) ref.set(p, v);
  ref.e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.3 * kRate));
  ref.go(0.6);
  for (int32_t i = 0; i < kNoiseParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kNs, i);
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
  Rig r;
  r.set(P::Color, 7.0f);
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Color)), 1.0, 1e-6);
  r.set(P::Tune, std::numeric_limits<float>::quiet_NaN());
  CHECK_NEAR(r.e.instrumentBaseParam(1, id(P::Tune)), 900.0, 1e-6);
  r.e.setInstrumentParam(1, 999, 1.0f);   // unknown id: no effect
}

void testLifecycleAndEnvelope() {
  // A one-shot ends by itself and goes inactive.
  Rig r;
  r.set(P::Decay, 0.1f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(0.05);
  CHECK(peak(r.o) > 0.01f);
  r.go(0.5);
  CHECK(rms(r.o, at(0.4), r.o.size()) < 1e-6);
  // Held with sustain: plays until the note-off, then releases to silence.
  Rig h;
  for (auto& [p, v] : kBase) h.set(p, v);
  h.set(P::Mix, 0.0f); h.set(P::Release, 0.1f);
  h.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.5 * kRate));
  h.go(0.45);
  CHECK(rms(h.o, at(0.3), at(0.45)) > 0.02);
  h.go(0.5);
  CHECK(rms(h.o, at(0.7), h.o.size()) < 1e-6);
  // Attack: a 1 s attack is quiet at the start and loud at the end.
  const Buf a = render({{P::Attack, 1.0f}}, 1.2);
  CHECK(rms(a, at(0.05), at(0.15)) < 0.25 * rms(a, at(0.9), at(1.0)));
}

void testDeterministicAndRetrigger() {
  const Params p = {{P::Source, 3.0f}, {P::Texture, 0.5f}, {P::Mix, 0.7f}, {P::Drive, 0.7f}, {P::Sustain, 0.0f}, {P::Decay, 0.3f}};
  CHECK(render(p, 0.5) == render(p, 0.5));
  // A fresh note after the previous one finished matches the first (state fully reset).
  Rig r;
  for (auto& [pp, v] : p) r.set(pp, v);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(1.5);
  const Buf first(r.o.begin(), r.o.begin() + at(0.3));
  Buf second;
  r.o.clear();
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(0.3);
  second.assign(r.o.begin(), r.o.begin() + at(0.3));
  CHECK(first == second);
}

void testRetriggerFadesWithoutStep() {
  Rig r;
  for (auto& [p, v] : kBase) r.set(p, v);
  r.set(P::Mix, 0.6f); r.set(P::Sustain, 1.0f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(0.2);
  r.e.instrumentNoteOn(1, 0.6f, 0.0f, 3.0f);   // retrigger a sounding note, with a new velocity and pitch
  r.go(0.2);
  CHECK(finite(r.o));
  // No sample-to-sample jump beyond what the noise itself produces (the fade is ~2 ms, not a cut).
  float worst = 0, typical = 0;
  for (size_t i = at(0.19); i < at(0.21); i++) worst = std::fmax(worst, std::fabs(r.o[i] - r.o[i - 1]));
  for (size_t i = at(0.05); i < at(0.15); i++) typical = std::fmax(typical, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(worst < 1.5f * typical);
  CHECK(rms(r.o, at(0.3), at(0.4)) > 0.01);
}

void testVelocity() {
  const Params p = {{P::Mix, 0.5f}};
  CHECK(rms(render(p, 0.3, 0.0f, 1.0f), 0, at(0.3)) > 1.5 * rms(render(p, 0.3, 0.0f, 0.4f), 0, at(0.3)));
}

// --- sources -----------------------------------------------------------------------------------

void testSourceSpectralSlopes() {
  // mix 0, colour 0.5, drive 0: the raw source. PSD per Hz: white flat, pink -3 dB/oct, brown -6 dB/oct.
  const Spec w = spectrum(render({{P::Source, 0.0f}}, 1.0), at(0.2));
  const Spec p = spectrum(render({{P::Source, 1.0f}}, 1.0), at(0.2));
  const Spec b = spectrum(render({{P::Source, 2.0f}}, 1.0), at(0.2));
  auto slope = [](const Spec& s) { return dbOf(psd(s, 250, 350) / psd(s, 4000, 5600)); };   // ~4 octaves apart
  const double sw = slope(w), sp = slope(p), sb = slope(b);
  CHECK(std::fabs(sw) < 2.0);
  CHECK(sp > 9.0 && sp < 15.0);
  CHECK(sb > 19.0 && sb < 29.0);
  CHECK(sb > sp + 6.0 && sp > sw + 6.0);
}

void testSourceLevelsAreComparable() {
  // Switching source must not jump the level by more than a few dB (except the intentionally sparse ones).
  const double ref = rms(render({{P::Source, 0.0f}}, 1.0), at(0.2), at(1.0));
  CHECK(ref > 0.05);
  for (int32_t s : {1, 2, 3}) {
    const double r = rms(render({{P::Source, float(s)}}, 1.0), at(0.2), at(1.0));
    const double db = std::fabs(20.0 * std::log10(r / ref));
    if (db >= 4.0) std::fprintf(stderr, "source %d is %.1f dB from white\n", s, db);
    CHECK(db < 4.0);
  }
}

void testSourcesHaveDifferentBehaviour() {
  const Buf white = render({{P::Source, 0.0f}}, 1.0);
  const Buf metal = render({{P::Source, 3.0f}}, 1.0);
  const Buf crackle = render({{P::Source, 4.0f}, {P::Density, 30.0f}}, 1.0);
  const Buf burst = render({{P::Source, 5.0f}, {P::Density, 10.0f}}, 1.0);
  // Metal is line-structured (tonal peaks way above the median); white is not.
  CHECK(peakOverMedianDb(spectrum(metal, at(0.2))) > peakOverMedianDb(spectrum(white, at(0.2))) + 8.0);
  CHECK(flatnessDb(spectrum(metal, at(0.2))) < flatnessDb(spectrum(white, at(0.2))) - 6.0);
  // Crackle is sparse: huge crest factor, many near-silent samples.
  const double crestCrackle = peak(crackle, at(0.2)) / rms(crackle, at(0.2), crackle.size());
  const double crestWhite = peak(white, at(0.2)) / rms(white, at(0.2), white.size());
  CHECK(crestCrackle > 3.0 * crestWhite);
  size_t quiet = 0;
  for (size_t i = at(0.2); i < crackle.size(); i++) if (std::fabs(crackle[i]) < 0.01f) quiet++;
  CHECK(double(quiet) / double(crackle.size() - at(0.2)) > 0.6);
  // Burst: the 20 ms rms swings from loud to silent (white never does).
  auto swing = [](const Buf& v) {
    double lo = 1e9, hi = 0;
    for (size_t a = at(0.2); a + at(0.02) < v.size(); a += at(0.02)) { const double r = rms(v, a, a + at(0.02)); lo = std::fmin(lo, r); hi = std::fmax(hi, r); }
    return hi / std::fmax(lo, 1e-9);
  };
  CHECK(swing(burst) > 20.0);
  CHECK(swing(white) < 1.5);
}

void testDensityChangesEventRate() {
  auto pops = [](double density) {
    const Buf b = render({{P::Source, 4.0f}, {P::Density, float(density)}}, 2.0);
    int n = 0;
    for (size_t i = at(0.2) + 1; i < b.size(); i++) if (std::fabs(b[i]) > 0.1f && std::fabs(b[i - 1]) <= 0.1f) n++;
    return n / (double(b.size() - at(0.2)) / kRate);
  };
  const double lo = pops(20), hi = pops(200);
  CHECK(hi > 5.0 * lo && lo > 8.0 && lo < 40.0);
}

// --- colour, texture, tone ----------------------------------------------------------------------

void testColorMovesTheSpectrum() {
  auto cen = [](float c) { return centroid(render({{P::Color, c}}, 1.0), at(0.2)); };
  const double dark = cen(0.0f), mid = cen(0.5f), bright = cen(1.0f);
  CHECK(dark < 600.0);
  CHECK(mid > 4000.0 && mid < 12000.0);
  CHECK(bright > mid);
  CHECK(dark < 0.1 * mid);
  // Level stays usable and comparable across the range (within ~4 dB of neutral at the extremes for white (6 dB for brown)).
  const double neutral = rms(render({{P::Color, 0.5f}}, 1.0), at(0.2), at(1.0));
  for (float c : {0.0f, 0.2f, 0.8f, 1.0f}) {
    const double db = 20.0 * std::log10(rms(render({{P::Color, c}}, 1.0), at(0.2), at(1.0)) / neutral);
    if (std::fabs(db) >= 4.0) std::fprintf(stderr, "color %.1f is %.1f dB from neutral\n", c, db);
    CHECK(std::fabs(db) < 4.0);
    const double bn = rms(render({{P::Source, 2.0f}, {P::Color, c}}, 1.0), at(0.2), at(1.0));
    const double b0 = rms(render({{P::Source, 2.0f}, {P::Color, 0.5f}}, 1.0), at(0.2), at(1.0));
    const double dbb = 20.0 * std::log10(bn / b0);
    if (std::fabs(dbb) >= 6.0) std::fprintf(stderr, "brown color %.1f is %.1f dB from neutral\n", c, dbb);
    CHECK(std::fabs(dbb) < 6.0);   // brown has almost nothing up there to level, so the bright end is looser
  }
  // Neutral colour is exactly the raw source (transparent).
  CHECK(render({{P::Color, 0.5f}}, 0.3) == render({{P::Color, 0.5f}, {P::Tone, 0.5f}}, 0.3));
}

void testToneIsAnOutputTilt() {
  auto cen = [](float t) { return centroid(render({{P::Tone, t}}, 1.0), at(0.2)); };
  CHECK(cen(0.1f) < 0.5 * cen(0.5f));
  CHECK(cen(0.9f) > cen(0.5f));
}

void testTextureMovesFromSmoothToSparse() {
  auto crest = [](float t, float dens) {
    const Buf b = render({{P::Texture, t}, {P::Density, dens}}, 1.5);
    return peak(b, at(0.2)) / rms(b, at(0.2), b.size());
  };
  auto swing = [](float t, float dens) {
    const Buf b = render({{P::Texture, t}, {P::Density, dens}}, 1.5);
    double lo = 1e9, hi = 0;
    for (size_t a = at(0.2); a + at(0.01) < b.size(); a += at(0.01)) { const double r = rms(b, a, a + at(0.01)); lo = std::fmin(lo, r); hi = std::fmax(hi, r); }
    return hi / std::fmax(lo, 1e-9);
  };
  CHECK(swing(0.0f, 20.0f) < 1.5);        // smooth
  CHECK(swing(0.6f, 20.0f) > 3.0);        // events are audible as level swings
  CHECK(crest(1.0f, 20.0f) > 2.0 * crest(0.0f, 20.0f));   // sparse and clicky
  // More density at the same texture fills the gaps in.
  CHECK(swing(0.6f, 2000.0f) < swing(0.6f, 20.0f));
  // Level is kept roughly constant across the texture range (gate makeup).
  const double r0 = rms(render({{P::Texture, 0.0f}}, 1.5), at(0.2), at(1.5));
  const double r1 = rms(render({{P::Texture, 0.5f}, {P::Density, 200.0f}}, 1.5), at(0.2), at(1.5));
  CHECK(std::fabs(20 * std::log10(r1 / r0)) < 6.0);
}

// --- resonator bank ----------------------------------------------------------------------------

void testResonatorTunesTheNoise() {
  // Harmonic spread, high resonance, wet only: peaks on the harmonic series of Tune, and the pitch follows both
  // Tune and note pitch. (Each band is levelled equally for noise, so which harmonic is highest is a matter of
  // the weights and chance; what must hold is that the energy sits on the series and the fundamental is present.)
  const Params tuned = {{P::Mix, 1.0f}, {P::Resonance, 0.85f}, {P::Spread, 1.0f / 3.0f}};
  auto onSeries = [&](float tune, float pitch) {
    Params p = tuned;
    p.push_back({P::Tune, tune});
    const double f0 = tune * std::exp2(pitch / 12.0);
    const double pk = peakHz(welch(render(p, 7.0, pitch), at(0.5), 16), 80, 3000);
    const double n = std::round(pk / f0);
    return n >= 1.0 && n <= 6.0 && std::fabs(pk - n * f0) < 0.02 * f0 + 3.0;
  };
  CHECK(onSeries(220.0f, 0.0f));
  CHECK(onSeries(330.0f, 0.0f));
  CHECK(onSeries(220.0f, 12.0f));
  const Buf b = render(tuned, 7.0);
  const Spec s = welch(b, at(0.5), 16);
  CHECK(peakOverMedianDb(s) > 25.0);              // no longer reads as broadband noise
  CHECK(flatnessDb(s) < flatnessDb(welch(render({}, 7.0), at(0.5), 16)) - 10.0);
  const Spec s220 = welch(render({{P::Mix, 1.0f}, {P::Resonance, 0.85f}, {P::Spread, 1.0f / 3.0f}, {P::Tune, 220.0f}}, 7.0), at(0.5), 16);
  const double fund = psd(s220, 212, 228), gap = psd(s220, 270, 300);
  CHECK(dbOf(fund / gap) > 20.0);                 // fundamental stands well above the space between harmonics
  CHECK(dbOf(psd(s220, 432, 448) / psd(s220, 500, 560)) > 20.0);   // and so does the second harmonic
}

void testResonanceControlsRing() {
  // More resonance = narrower and longer: the spectrum gets more peaked step by step...
  auto tonal = [](float res) {
    const Buf b = render({{P::Mix, 1.0f}, {P::Resonance, res}, {P::Spread, 0.0f}, {P::Tune, 600.0f}}, 1.0);
    return peakOverMedianDb(spectrum(b, at(0.3)));
  };
  CHECK(tonal(0.9f) > tonal(0.5f) + 5.0);
  CHECK(tonal(0.5f) > tonal(0.1f) + 5.0);
  // ...and an impulsive source keeps ringing after its tick at high Q, but not at low Q.
  auto ring = [](float res) {
    const Buf b = render({{P::Source, 4.0f}, {P::Density, 2.0f}, {P::Mix, 1.0f}, {P::Tune, 600.0f}, {P::Resonance, res}, {P::Spread, 0.0f}}, 2.0);
    size_t p = 0;   // the first tick (the next one is >150 ms later)
    while (p < b.size() && std::fabs(b[p]) < 0.05f) p++;
    return rms(b, p + at(0.05), p + at(0.07)) / std::fmax(rms(b, p + at(0.005), p + at(0.025)), 1e-9);
  };
  CHECK(ring(0.9f) > 0.25);   // (the detuned unison bands beat, so it is not a pure exponential)
  CHECK(ring(0.1f) < 0.1);
  // Level does not collapse as Q rises (noise-normalised bands).
  const double lo = rms(render({{P::Mix, 1.0f}, {P::Resonance, 0.1f}}, 1.0), at(0.3), at(1.0));
  const double hi = rms(render({{P::Mix, 1.0f}, {P::Resonance, 0.9f}}, 1.0), at(0.3), at(1.0));
  CHECK(std::fabs(20 * std::log10(hi / lo)) < 9.0);
}

void testSpreadChangesTheLayout() {
  auto lines = [](float spread) {
    // Distinct peaks of the averaged spectrum (9-bin boxcar, local maxima over +-20 bins) within 20 dB of the strongest.
    const Spec s = welch(render({{P::Mix, 1.0f}, {P::Resonance, 0.9f}, {P::Spread, spread}, {P::Tune, 300.0f}}, 5.0), at(0.5), 8);
    Spec sm(s.size(), 0.0);
    double mx = 0;
    for (size_t i = 4; i + 4 < s.size(); i++) { for (size_t k = i - 4; k <= i + 4; k++) sm[i] += s[k]; mx = std::fmax(mx, sm[i]); }
    int n = 0;
    for (size_t i = 24; i + 24 < s.size(); i++) {
      bool top = sm[i] > mx * 0.01;
      for (size_t k = i - 20; top && k <= i + 20; k++) if (sm[k] > sm[i]) top = false;
      if (top) n++;
    }
    return n;
  };
  // Unison is one cluster of lines; harmonic and cluster layouts open up into more.
  CHECK(lines(1.0f / 3.0f) > lines(0.0f));
  CHECK(lines(1.0f) > lines(0.0f));
  // Inharmonic bell: a partial near 2.756x, and none at exactly 2x.
  const Spec bell = welch(render({{P::Mix, 1.0f}, {P::Resonance, 0.9f}, {P::Spread, 2.0f / 3.0f}, {P::Tune, 300.0f}}, 5.0), at(0.5), 8);
  CHECK(dbOf(psd(bell, 820, 835) / psd(bell, 590, 610)) > 15.0);
}

void testSweepMovesTheBank() {
  // A rising sweep with a long attack: the spectral peak climbs as the envelope rises.
  const Buf b = render({{P::Mix, 1.0f}, {P::Resonance, 0.85f}, {P::Spread, 0.0f}, {P::Tune, 200.0f},
                        {P::Sweep, 3.0f}, {P::Attack, 2.0f}, {P::Decay, 0.5f}, {P::Sustain, 1.0f}}, 2.5);
  const double early = peakHz(spectrum(b, at(0.4), 8192), 80, 4000);
  const double late = peakHz(spectrum(b, at(2.2), 8192), 80, 4000);
  CHECK(late > 3.0 * early);
  CHECK_NEAR(late, 1600.0, 250.0);
  // Zero sweep stays put.
  const Buf s0 = render({{P::Mix, 1.0f}, {P::Resonance, 0.85f}, {P::Spread, 0.0f}, {P::Tune, 200.0f},
                         {P::Attack, 1.0f}}, 1.5);
  CHECK_NEAR(peakHz(spectrum(s0, at(0.4), 8192), 80, 4000), peakHz(spectrum(s0, at(1.2), 8192), 80, 4000), 30.0);
}

void testMixIsEqualPower() {
  const double dry = rms(render({{P::Mix, 0.0f}}, 1.0), at(0.3), at(1.0));
  const double mid = rms(render({{P::Mix, 0.5f}}, 1.0), at(0.3), at(1.0));
  const double wet = rms(render({{P::Mix, 1.0f}}, 1.0), at(0.3), at(1.0));
  CHECK(std::fabs(20 * std::log10(mid / dry)) < 4.5);
  CHECK(std::fabs(20 * std::log10(wet / dry)) < 6.0);
}

// --- drive and feedback ------------------------------------------------------------------------

void testDriveAddsDensityAndStaysBounded() {
  const Params base = {{P::Source, 0.0f}, {P::Mix, 0.8f}, {P::Resonance, 0.7f}, {P::Spread, 0.7f}, {P::Tune, 400.0f}};
  Params clean = base; clean.push_back({P::Drive, 0.0f});
  Params hot = base; hot.push_back({P::Drive, 1.0f});
  const Buf c = render(clean, 1.0), h = render(hot, 1.0);
  CHECK(finite(c) && finite(h));
  CHECK(peak(h) <= 1.0f && peak(c) <= 1.0f);
  CHECK(c != h);
  // Drive at 0 is exactly transparent: switching drive off equals a patch that never had the stage.
  CHECK(render(clean, 0.3) == render({{P::Mix, 0.8f}, {P::Resonance, 0.7f}, {P::Spread, 0.7f}, {P::Tune, 400.0f}, {P::Drive, 0.0f}}, 0.3));
  // A hot resonator patch does not run away: late level is not larger than early level by more than a few dB.
  CHECK(rms(h, at(0.7), at(1.0)) < 2.0 * rms(h, at(0.1), at(0.4)) + 0.05);
  // Feedback with a sustained, near-unison resonance: bounded and finite for several seconds.
  const Buf fb = render({{P::Source, 0.0f}, {P::Mix, 1.0f}, {P::Resonance, 1.0f}, {P::Spread, 0.0f}, {P::Drive, 1.0f}, {P::Tune, 500.0f}}, 4.0);
  CHECK(finite(fb) && peak(fb) <= 1.0f);
  CHECK(rms(fb, at(3.0), at(4.0)) < 2.0 * rms(fb, at(0.5), at(1.5)) + 0.05);
}

// --- bounds, live changes, allocation ----------------------------------------------------------

void testExtremesStayFiniteAndBounded() {
  uint32_t s = 12345;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return float(s >> 8) / float(1u << 24); };
  for (int trial = 0; trial < 120; trial++) {
    Rig r;
    for (int32_t i = 0; i < kNoiseParamCount; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kNs, i);
      // Half the draws sit on the range ends.
      const float u = rnd();
      const float v = u < 0.25f ? p->min : u > 0.75f ? p->max : p->min + rnd() * (p->max - p->min);
      r.e.setInstrumentParam(1, i, v);
    }
    r.set(P::Attack, 0.01f + 0.1f * rnd()); r.set(P::Decay, 0.1f + 0.5f * rnd()); r.set(P::Sustain, 1.0f);
    r.e.instrumentNoteOn(1, 1.0f + 0.5f * rnd(), 0.0f, 24.0f * rnd() - 12.0f, static_cast<int32_t>(0.6 * kRate));
    r.go(1.0);
    CHECK(finite(r.o));
    CHECK(peak(r.o) <= 1.0001f);
  }
  // Extreme note pitch pushes bands past Nyquist: they fade, nothing blows up.
  const Buf hi = render({{P::Mix, 1.0f}, {P::Resonance, 1.0f}, {P::Spread, 1.0f}, {P::Tune, 12000.0f}}, 0.5, 48.0f);
  CHECK(finite(hi) && peak(hi) <= 1.0f);
}

void testLiveParameterChanges() {
  Rig r;
  for (auto& [p, v] : kBase) r.set(p, v);
  r.set(P::Sustain, 1.0f); r.set(P::Mix, 0.6f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  int64_t f = 0;
  Buf o;
  uint32_t s = 99;
  for (int i = 0; i < 600; i++) {
    s = s * 1664525u + 1013904223u;
    const int32_t param = int32_t((s >> 8) % uint32_t(kNoiseParamCount));
    const InstrumentParamInfo* pi = instrumentParamInfo(kNs, param);
    s = s * 1664525u + 1013904223u;
    if (param != id(P::Attack) && param != id(P::Sustain)) r.e.setInstrumentParam(1, param, pi->min + (pi->max - pi->min) * float(s >> 8) / float(1u << 24));
    if (i == 300) r.e.instrumentNoteOn(1, 0.7f, 0, 2);
    r.e.process(f, kBlock);
    o.insert(o.end(), r.e.outputChannel(0), r.e.outputChannel(0) + kBlock);
    f += kBlock;
  }
  CHECK(finite(o) && peak(o) <= 1.0001f);
}

void testScheduledNoteStartsOnItsFrame() {
  Rig r;
  for (auto& [p, v] : kBase) r.set(p, v);
  r.set(P::Mix, 0.5f);
  const int64_t at0 = 10000 + 37;
  r.e.scheduleInstrumentNote(at0, 1, 1.0f);
  r.go(0.4);
  CHECK(peak(r.o, 0, static_cast<size_t>(at0)) == 0.0f);
  CHECK(peak(r.o, static_cast<size_t>(at0), at(0.4)) > 0.01f);
}

void testRoutesThroughBusAndPan() {
  Rig r;
  for (auto& [p, v] : kBase) r.set(p, v);
  r.set(P::Mix, 0.5f);
  r.e.instrumentNoteOn(1, 1.0f, -1.0f, 0.0f);
  int64_t f = 0;
  double l = 0, rr = 0;
  for (int i = 0; i < 100; i++) {
    r.e.process(f, kBlock);
    f += kBlock;
    for (int k = 0; k < kBlock; k++) { l += std::fabs(r.e.outputChannel(0)[k]); rr += std::fabs(r.e.outputChannel(1)[k]); }
  }
  CHECK(l > 10.0 * rr);
}

void testDefaultPatchLevel() {
  Rig r;
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(0.5);
  CHECK(peak(r.o) > 0.1f && peak(r.o) <= 1.0f);
}

void testRenderDoesNotAllocate() {
  Rig r;
  r.set(P::Sustain, 1.0f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  int64_t f = 0;
  r.e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 600; i++) {
    if (i == 50) r.e.setInstrumentParam(1, id(P::Source), 3.0f);
    if (i == 100) r.e.setInstrumentParam(1, id(P::Texture), 0.8f);
    if (i == 150) r.e.setInstrumentParam(1, id(P::Resonance), 0.95f);
    if (i == 200) r.e.setInstrumentParam(1, id(P::Drive), 0.9f);
    if (i == 250) r.e.setInstrumentParam(1, id(P::Source), 5.0f);
    if (i == 300) r.e.instrumentNoteOn(1, 0.7f, 0, 2);
    if (i == 350) r.e.setInstrumentParam(1, id(P::Source), 4.0f);
    if (i == 400) r.e.setInstrumentParam(1, id(P::Spread), 1.0f);
    if (i == 450) r.e.setInstrumentParam(1, id(P::Sweep), 3.0f);
    if (i == 500) r.e.instrumentNoteOff(1);
    r.e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

// --- the seven validation sounds ---------------------------------------------------------------
// Each is checked against what it claims to be, and (the point of the engine) against "white noise
// through a filter": same spectral measure on the plain-filtered reference vs the sound.

Buf renderSound(const Params& p, double seconds, double hold = 0.0) {
  Rig r;
  for (auto& [pp, v] : p) r.set(pp, v);
  if (hold > 0.0) r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(hold * kRate));
  else r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);
  r.go(seconds);
  return r.o;
}

void testSoundDesignTargets() {
  const Buf plain = render({{P::Color, 0.9f}}, 1.0);                       // white -> high-pass: the control case
  const Spec plainSpec = spectrum(plain, at(0.3));

  // 1. tight closed hat
  const Buf closedHat = renderSound({{P::Source, 3.0f}, {P::Color, 0.85f}, {P::Resonance, 0.35f}, {P::Spread, 1.0f}, {P::Tune, 5200.0f},
                                     {P::Mix, 0.55f}, {P::Attack, 0.0005f}, {P::Decay, 0.06f}, {P::Drive, 0.3f}, {P::Tone, 0.6f}}, 0.4);
  CHECK(peak(closedHat) > 0.1f);
  CHECK(rms(closedHat, at(0.2), at(0.4)) < 0.02 * rms(closedHat, 0, at(0.03)));       // tight: gone in 200 ms
  CHECK(centroid(closedHat, 0, 2048) > 5000.0);                                        // hat territory

  // 2. long metallic hat / cymbal
  const Buf cymbal = renderSound({{P::Source, 3.0f}, {P::Color, 0.8f}, {P::Resonance, 0.55f}, {P::Spread, 0.9f}, {P::Tune, 3500.0f},
                                  {P::Mix, 0.6f}, {P::Attack, 0.0005f}, {P::Decay, 1.6f}, {P::Drive, 0.25f}, {P::Tone, 0.6f}}, 2.2);
  CHECK(rms(cymbal, at(0.8), at(1.0)) > 0.02);                                         // still ringing at 0.8 s
  CHECK(centroid(cymbal, at(0.05)) > 4000.0);
  CHECK(peakOverMedianDb(spectrum(cymbal, at(0.1))) > peakOverMedianDb(plainSpec) + 6.0);   // metallic lines, not hiss

  // 3. aggressive industrial percussion
  const Buf industrial = renderSound({{P::Source, 5.0f}, {P::Density, 14.0f}, {P::Texture, 0.5f}, {P::Color, 0.45f}, {P::Resonance, 0.7f},
                                      {P::Spread, 0.75f}, {P::Tune, 180.0f}, {P::Mix, 0.7f}, {P::Decay, 0.5f}, {P::Drive, 0.85f}}, 0.8);
  CHECK(peak(industrial) > 0.4f && peak(industrial) <= 1.0f);
  CHECK(rms(industrial, 0, at(0.3)) > 0.1);

  // 4. tuned resonant noise
  const Buf tuned = renderSound({{P::Source, 1.0f}, {P::Color, 0.55f}, {P::Resonance, 0.85f}, {P::Spread, 1.0f / 3.0f}, {P::Tune, 220.0f},
                                 {P::Mix, 1.0f}, {P::Decay, 0.5f}, {P::Sustain, 0.8f}, {P::Drive, 0.1f}}, 2.0, 1.5);
  {
    const double pk = peakHz(spectrum(tuned, at(0.5), 32768), 80, 1500);
    const double n = std::round(pk / 220.0);
    CHECK(n >= 1.0 && std::fabs(pk - n * 220.0) < 8.0);   // on the harmonic series of the tune
  }
  CHECK(peakOverMedianDb(spectrum(tuned, at(0.5))) > 20.0);

  // 5. icy high-frequency texture
  const Buf icy = renderSound({{P::Source, 0.0f}, {P::Color, 1.0f}, {P::Texture, 0.55f}, {P::Density, 500.0f}, {P::Resonance, 0.8f},
                               {P::Spread, 2.0f / 3.0f}, {P::Tune, 5200.0f}, {P::Mix, 0.8f}, {P::Attack, 0.3f}, {P::Decay, 2.5f},
                               {P::Sustain, 0.5f}, {P::Tone, 0.8f}}, 3.0, 2.0);
  CHECK(centroid(icy, at(1.0)) > 5000.0);
  CHECK(rms(icy, at(1.0), at(2.0)) > 0.01);
  CHECK(peakOverMedianDb(spectrum(icy, at(1.0))) > peakOverMedianDb(plainSpec) + 6.0);

  // 6. dark low-frequency drone
  const Buf drone = renderSound({{P::Source, 2.0f}, {P::Color, 0.15f}, {P::Resonance, 0.55f}, {P::Spread, 0.2f}, {P::Tune, 55.0f},
                                 {P::Mix, 0.55f}, {P::Attack, 1.2f}, {P::Decay, 1.0f}, {P::Sustain, 0.9f}, {P::Drive, 0.4f}, {P::Tone, 0.3f}}, 4.0, 3.0);
  CHECK(centroid(drone, at(2.0)) < 600.0);
  CHECK(rms(drone, at(2.0), at(3.0)) > 0.02);
  CHECK(finite(drone) && peak(drone) <= 1.0f);

  // 7. evolving noise riser: the spectrum climbs and the level swells
  const Buf riser = renderSound({{P::Source, 0.0f}, {P::Color, 0.5f}, {P::Texture, 0.25f}, {P::Density, 300.0f}, {P::Resonance, 0.6f},
                                 {P::Spread, 0.5f}, {P::Tune, 400.0f}, {P::Sweep, 3.0f}, {P::Mix, 0.8f}, {P::Attack, 3.5f},
                                 {P::Decay, 0.3f}, {P::Sustain, 1.0f}, {P::Drive, 0.3f}}, 4.5, 4.0);
  CHECK(centroid(riser, at(3.4)) > 2.0 * centroid(riser, at(0.6)));
  CHECK(rms(riser, at(3.3), at(3.8)) > 4.0 * rms(riser, at(0.3), at(0.8)));

  // The engine's identity, quantified: tuned, icy, cymbal are far from the plain filtered noise; so is the riser/drone
  // in their own way (different slope + lines). Here: the tonal ones against the control.
  CHECK(flatnessDb(spectrum(tuned, at(0.5))) < flatnessDb(plainSpec) - 10.0);

  writeWav("1_closed_hat", closedHat);
  writeWav("2_metal_cymbal", cymbal);
  writeWav("3_industrial", industrial);
  writeWav("4_tuned_noise", tuned);
  writeWav("5_icy_texture", icy);
  writeWav("6_dark_drone", drone);
  writeWav("7_riser", riser);
  writeWav("0_control_plain_hp_white", plain);
}

}  // namespace

int main() {
  testRegistrationAndDefaults();
  testEveryParameterPropagates();
  testParamValidation();
  testLifecycleAndEnvelope();
  testDeterministicAndRetrigger();
  testRetriggerFadesWithoutStep();
  testVelocity();
  testSourceSpectralSlopes();
  testSourceLevelsAreComparable();
  testSourcesHaveDifferentBehaviour();
  testDensityChangesEventRate();
  testColorMovesTheSpectrum();
  testToneIsAnOutputTilt();
  testTextureMovesFromSmoothToSparse();
  testResonatorTunesTheNoise();
  testResonanceControlsRing();
  testSpreadChangesTheLayout();
  testSweepMovesTheBank();
  testMixIsEqualPower();
  testDriveAddsDensityAndStaysBounded();
  testExtremesStayFiniteAndBounded();
  testLiveParameterChanges();
  testScheduledNoteStartsOnItsFrame();
  testRoutesThroughBusAndPan();
  testDefaultPatchLevel();
  testRenderDoesNotAllocate();
  testSoundDesignTargets();
  std::printf("noise_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
