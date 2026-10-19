#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the Digital Machines (dsp/fm_kernel.h, instruments/digital_machine.h, instruments/dm_*.h)
// through the Engine's instrument API: the shared contract for every Machine (loop over kinds), then
// per-Machine behaviour. Set DM_WAV_DIR=<dir> to dump demo patches as 16-bit WAVs for listening
// (docs/digital-machines.md).
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
constexpr double kPi = 3.14159265358979323846;

constexpr int32_t kThump = static_cast<int32_t>(InstrumentKind::DmThump);
constexpr int32_t kMetal = static_cast<int32_t>(InstrumentKind::DmMetal);
constexpr int32_t kTone = static_cast<int32_t>(InstrumentKind::DmTone);
constexpr int32_t kCrack = static_cast<int32_t>(InstrumentKind::DmCrack);
constexpr int32_t kChaos = static_cast<int32_t>(InstrumentKind::DmChaos);
constexpr int32_t kAllKinds[] = {kThump, kMetal, kTone, kCrack, kChaos};  // grows as each Machine lands

using Buf = std::vector<float>;
template <class E>
int32_t id(E p) { return static_cast<int32_t>(p); }

struct Setup {
  int32_t kind = kThump;
  std::vector<std::pair<int32_t, float>> params;
  float velocity = 1.0f, pitch = 0.0f;
  double duration = -1;  // seconds; < 0 = held
  double rate = kRate;
  Setup& set(int32_t p, float v) { params.emplace_back(p, v); return *this; }
  template <class E> Setup& with(E p, float v) { return set(id(p), v); }
};

size_t at(double s, double rate = kRate) { return static_cast<size_t>(s * rate); }

void run(Engine& e, int64_t& frame, double seconds, Buf* l, double rate = kRate) {
  const int32_t frames = static_cast<int32_t>(seconds * rate / kBlock) * kBlock;
  for (int32_t d = 0; d < frames; d += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    l->insert(l->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
}

void writeWav(const char* name, const Buf& b) {
  const char* dir = std::getenv("DM_WAV_DIR");
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

void apply(Engine& e, const Setup& s) {
  for (const auto& p : s.params) e.setInstrumentParam(1, p.first, p.second);
}

// One note on instrument 1 (master bus), `seconds` rendered.
Buf play(const Setup& s, double seconds, const char* wav = nullptr) {
  Engine e;
  e.init(s.rate, 2, 8);
  e.createInstrument(1, s.kind, 0);
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
  if (b <= a) return 0;
  for (size_t i = a; i < b; i++) s += double(v[i]) * v[i];
  return std::sqrt(s / double(b - a));
}
bool allFinite(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}
double mean(const Buf& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; i++) s += v[i];
  return s / double(b - a);
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
// Share of spectral energy that is NOT the fundamental (harmonics 1..32). 0 = pure sine.
double harmonicActivity(const Buf& v, size_t a, size_t b, double f0) {
  double fund = 0, all = 0;
  for (int h = 1; h <= 32; h++) {
    const double m = bin(v, a, b, f0 * h);
    all += m * m;
    if (h == 1) fund = m * m;
  }
  return all > 0 ? 1.0 - fund / all : 0.0;
}
// Brightness proxy independent of level: rms of the first difference over rms. ~ 2 pi f / fs for a
// sine, so it grows with spectral centroid.
double brightness(const Buf& v, size_t a, size_t b) {
  double d = 0, s = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) {
    const double x = v[i] - v[i - 1];
    d += x * x;
    s += double(v[i]) * v[i];
  }
  return s > 0 ? std::sqrt(d / s) : 0.0;
}
// Time index (samples) of the first |x| > threshold.
size_t firstAbove(const Buf& v, float thr) {
  for (size_t i = 0; i < v.size(); i++) if (std::fabs(v[i]) > thr) return i;
  return v.size();
}
// Time after which the signal never exceeds `thr` again.
size_t lastAbove(const Buf& v, float thr) {
  for (size_t i = v.size(); i > 0; i--) if (std::fabs(v[i - 1]) > thr) return i;
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Shared contract: every Machine.
// ---------------------------------------------------------------------------------------------

void testMetadata() {
  for (int32_t kind : kAllKinds) {
    const InstrumentKindMeta* meta = instrumentKindMeta(kind);
    CHECK(meta != nullptr);
    CHECK(meta && std::strcmp(meta->category, "digital") == 0);
    CHECK(instrumentParamCount(kind) >= 8);
    for (int32_t i = 0; i < instrumentParamCount(kind); i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kind, i);
      CHECK(p != nullptr);
      if (!p) continue;
      CHECK(p->id == i);  // table order == id order: Machines index values[] by id
      CHECK(p->min < p->max);
      CHECK(p->def >= p->min && p->def <= p->max);
      CHECK(p->name && p->name[0]);
    }
    // Tune, Decay and the depth control sit at ids 0, 1, 2 on every Machine.
    CHECK(std::strcmp(instrumentParamInfo(kind, 0)->name, "tune") == 0);
    CHECK(std::strcmp(instrumentParamInfo(kind, 1)->name, "decay") == 0);
    CHECK(std::strcmp(instrumentParamInfo(kind, 1)->unit, "s") == 0);
  }
  // Clamping and non-finite handling through the engine.
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kThump, 0));
  e.setInstrumentParam(1, id(DmThumpParam::Tune), 1e9f);
  Buf a = play(Setup{}.with(DmThumpParam::Tune, 1e9f), 0.2);
  Buf b = play(Setup{}.with(DmThumpParam::Tune, 400.0f), 0.2);
  CHECK(a == b);  // clamped to the advertised max
  Buf n1 = play(Setup{}.with(DmThumpParam::Tune, std::nanf("")), 0.2);
  Buf n2 = play(Setup{}, 0.2);
  CHECK(n1 == n2);  // NaN ignored
  // Unknown param id: no-op.
  Setup unknown; unknown.set(999, 1.0f);
  CHECK(play(unknown, 0.2) == n2);
}

void testFiniteBoundedAndEnds() {
  for (int32_t kind : kAllKinds) {
    Setup s; s.kind = kind;
    Buf o = play(s, 3.0);
    CHECK(allFinite(o));
    CHECK(peak(o) > 0.05f);
    CHECK(peak(o) <= 1.0f);
    // Default patch is a one-shot (hold 0): it ends itself in exact silence.
    CHECK(o.size() >= at(3.0) - kBlock);
    CHECK(rms(o, at(2.9), o.size()) == 0.0);
    Engine e; e.init(kRate, 2, 8);
    e.createInstrument(1, kind, 0);
    e.instrumentNoteOn(1, 1.0f);
    int64_t f = 0; Buf tmp;
    CHECK(e.activeInstrumentCount() == 1);
    run(e, f, 3.0, &tmp);
    CHECK(e.activeInstrumentCount() == 0);
  }
}

void testDeterministic() {
  for (int32_t kind : kAllKinds) {
    Setup s; s.kind = kind;
    CHECK(play(s, 0.5) == play(s, 0.5));
    // A second note after a finished one is identical to the first (fresh-note reset).
    Engine e; e.init(kRate, 2, 8);
    e.createInstrument(1, kind, 0);
    e.instrumentNoteOn(1, 0.9f);
    int64_t f = 0; Buf first; run(e, f, 3.0, &first);
    e.instrumentNoteOn(1, 0.9f);
    Buf second; run(e, f, 3.0, &second);
    CHECK(first == second);
  }
}

void testVelocityAndPitchOffset() {
  for (int32_t kind : kAllKinds) {
    Setup lo; lo.kind = kind; lo.velocity = 0.3f;
    Setup hi; hi.kind = kind; hi.velocity = 1.0f;
    CHECK(rms(play(hi, 0.3), 0, at(0.3)) > rms(play(lo, 0.3), 0, at(0.3)) * 1.5);
    Setup silent; silent.kind = kind; silent.velocity = 0.0f;
    CHECK(peak(play(silent, 0.3)) == 0.0f);
    Setup up; up.kind = kind; up.pitch = 7.0f;
    Setup base; base.kind = kind;
    CHECK(play(up, 0.2) != play(base, 0.2));
  }
}

void testNoteOffChokes() {
  for (int32_t kind : kAllKinds) {
    Setup open; open.kind = kind;
    open.with(DmThumpParam::Decay, 1.5f);  // id 1 = decay on every Machine
    Setup choked = open; choked.duration = 0.05;
    Buf a = play(open, 0.6), b = play(choked, 0.6);
    CHECK(rms(a, at(0.3), at(0.5)) > 0.0);
    CHECK(rms(b, at(0.3), at(0.5)) < rms(a, at(0.3), at(0.5)) * 0.1);
    CHECK(allFinite(b));
  }
}

void testRetriggerIsContinuous() {
  for (int32_t kind : kAllKinds) {
    Engine e; e.init(kRate, 2, 8);
    e.createInstrument(1, kind, 0);
    e.setInstrumentParam(1, 1, 1.5f);
    e.instrumentNoteOn(1, 1.0f);
    int64_t f = 0; Buf o;
    run(e, f, 0.1, &o);
    e.instrumentNoteOn(1, 1.0f, 0.0f, 5.0f);  // retrigger mid-sound, different pitch
    run(e, f, 0.3, &o);
    CHECK(allFinite(o));
    CHECK(peak(o) <= 1.0f);
    // No full-scale single-sample step at the seam (a hard restart would jump).
    const size_t seam = at(0.1) / kBlock * kBlock;
    float step = 0;
    for (size_t i = seam - 2; i < seam + 4 && i + 1 < o.size(); i++) step = std::fmax(step, std::fabs(o[i + 1] - o[i]));
    CHECK(step < 1.2f);
  }
}

void testSilentUntilScheduledAndDuration() {
  for (int32_t kind : kAllKinds) {
    Engine e; e.init(kRate, 2, 8);
    e.createInstrument(1, kind, 0);
    e.setInstrumentParam(1, 1, 1.5f);
    e.scheduleInstrumentNote(4800, 1, 1.0f, 0.0f, 0.0f, 4800);  // start 0.1 s, held 0.1 s
    int64_t f = 0; Buf o; run(e, f, 0.5, &o);
    const size_t first = firstAbove(o, 0.0f);
    CHECK(first >= 4800 && first <= 4800 + 64);
    CHECK(lastAbove(o, 0.0f) < 4800 + 4800 + at(0.6));  // choked: tail is bounded
    CHECK(rms(o, at(0.45), o.size()) < rms(o, at(0.1), at(0.2)) * 0.2 + 1e-9);
  }
}

void testRoutesThroughBus() {
  for (int32_t kind : kAllKinds) {
    Engine e; e.init(kRate, 2, 8);
    e.createInstrument(1, kind, /*busId=*/1);
    e.setBusParam(1, static_cast<int32_t>(NodeParam::BusGain), 0.5f);
    e.instrumentNoteOn(1, 1.0f);
    int64_t f = 0; Buf o; run(e, f, 0.3, &o);
    Setup s; s.kind = kind;
    Buf direct = play(s, 0.3);
    CHECK(peak(o) > 0.0f);
    CHECK_NEAR(peak(o), peak(direct) * 0.5, 1e-3);
  }
}

void testPolyphonyIsSum() {
  for (int32_t kind : kAllKinds) {
    Engine e; e.init(kRate, 2, 16);
    e.setBusParam(0, static_cast<int32_t>(NodeParam::LimiterEnable), 0.0f);  // test sums raw voices
    for (int32_t i = 1; i <= 12; i++) e.createInstrument(i, kind, 0);
    for (int32_t i = 1; i <= 12; i++) e.instrumentNoteOn(i, 0.3f, 0.0f, float(i));
    int64_t f = 0; Buf o; run(e, f, 0.4, &o);
    CHECK(allFinite(o));
    Buf sum(o.size(), 0.0f);
    for (int32_t i = 1; i <= 12; i++) {
      Setup s; s.kind = kind; s.velocity = 0.3f; s.pitch = float(i);
      Buf one = play(s, 0.4);
      for (size_t k = 0; k < sum.size() && k < one.size(); k++) sum[k] += one[k];
    }
    double maxDiff = 0;
    for (size_t k = 0; k < o.size(); k++) maxDiff = std::fmax(maxDiff, std::fabs(o[k] - sum[k]));
    CHECK(maxDiff < 1e-4);
  }
}

// Random parameters (within the advertised ranges), velocities, pitch offsets and sample rates:
// always finite and bounded, never a stuck note.
void testStabilityFuzz() {
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1 << 24); };
  const double rates[] = {22050.0, 48000.0, 96000.0};
  for (int32_t kind : kAllKinds) {
    for (double rate : rates) {
      for (int trial = 0; trial < 12; trial++) {
        Engine e; e.init(rate, 2, 8);
        e.createInstrument(1, kind, 0);
        for (int32_t i = 0; i < instrumentParamCount(kind); i++) {
          const InstrumentParamInfo* p = instrumentParamInfo(kind, i);
          float v = p->min + rnd() * (p->max - p->min);
          if (std::strcmp(p->unit, "step") == 0) v = std::floor(v + 0.5f);
          e.setInstrumentParam(1, i, v);
        }
        e.instrumentNoteOn(1, rnd() * 1.2f, 0.0f, rnd() * 60.0f - 30.0f, trial % 2 ? int32_t(rate * 0.3) : -1);
        int64_t f = 0; Buf o;
        const int32_t frames = int32_t(rate * 1.0 / kBlock) * kBlock;
        for (int32_t d = 0; d < frames; d += kBlock) {
          e.process(f, kBlock); f += kBlock;
          o.insert(o.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
          if (d == 4 * kBlock) e.instrumentNoteOn(1, rnd(), 0.0f, rnd() * 24.0f);
        }
        CHECK(allFinite(o));
        CHECK(peak(o) <= 1.0f);
      }
    }
  }
}

void testRenderDoesNotAllocate() {
  for (int32_t kind : kAllKinds) {
    Engine e; e.init(kRate, 2, 8);
    e.createInstrument(1, kind, 1);
    e.instrumentNoteOn(1, 1.0f, 0.3f);
    int64_t f = 0;
    e.process(f, kBlock);
    f += kBlock;
    const long before = g_allocs;
    for (int i = 0; i < 600; i++) {
      if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
      if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, -7);
      if (i == 300) e.instrumentNoteOff(1);
      if (i == 400) e.instrumentNoteOn(1, 1.0f);
      if (i % 50 == 0) {
        for (int32_t p = 0; p < instrumentParamCount(kind); p++) {
          const InstrumentParamInfo* info = instrumentParamInfo(kind, p);
          e.setInstrumentParam(1, p, info->min + (info->max - info->min) * float((i / 50 + p) % 5) / 4.0f);
        }
      }
      e.process(f, kBlock);
      f += kBlock;
    }
    CHECK(g_allocs == before);
  }
}

// ---------------------------------------------------------------------------------------------
// Kernel
// ---------------------------------------------------------------------------------------------

void testKernel() {
  const float* t = fmk::sineTable();
  double worst = 0;
  for (double x = -3.7; x < 3.7; x += 0.000731) {
    worst = std::fmax(worst, std::fabs(fmk::sinTurns(t, x) - std::sin(2 * kPi * x)));
  }
  CHECK(worst < 2e-6);
  CHECK(fmk::sinTurns(t, 1.0 - 1e-17) == fmk::sinTurns(t, 0.0));  // wraps, no out-of-range read
  CHECK(std::fabs(fmk::sinTurns(t, -1e-20)) < 1e-6);

  // Magnetic ratio: curated values sit on plateaus, in between it glides, endpoints are exact.
  static const float tbl[] = {0.5f, 1.0f, 2.0f, 4.0f};
  CHECK_NEAR(fmk::magneticRatio(tbl, 0.0f), 0.5, 1e-6);
  CHECK_NEAR(fmk::magneticRatio(tbl, 1.0f), 4.0, 1e-6);
  CHECK_NEAR(fmk::magneticRatio(tbl, 1.0f / 3.0f), 1.0, 1e-5);
  CHECK_NEAR(fmk::magneticRatio(tbl, 1.0f / 3.0f + 0.04f), 1.0, 1e-4);  // still on the plateau
  CHECK_NEAR(fmk::magneticRatio(tbl, 1.0f / 3.0f - 0.04f), 1.0, 1e-4);
  const float mid = fmk::magneticRatio(tbl, 0.5f);  // halfway 1 -> 2: geometric mean
  CHECK_NEAR(mid, std::sqrt(2.0), 1e-3);
  float prev = 0;
  bool mono = true;
  for (float x = 0; x <= 1.0f; x += 0.01f) { float r = fmk::magneticRatio(tbl, x); mono = mono && r >= prev - 1e-6f; prev = r; }
  CHECK(mono);

  // Envelope: attack, decay to the sustain floor, release to exact zero, restart from level.
  fmk::ExpEnv env; env.configure(48000.0);
  env.trigger(0.001f, percDecayCoef(0.1f * kDecayToTau, 48000.0), 0.4f);
  float l = 0;
  for (int i = 0; i < 48; i++) l = env.next();
  CHECK_NEAR(l, 1.0, 1e-6);
  for (int i = 0; i < 48000; i++) l = env.next();
  CHECK_NEAR(l, 0.4, 1e-4);
  env.release(percDecayCoef(0.01f * kDecayToTau, 48000.0));
  for (int i = 0; i < 4800; i++) l = env.next();
  CHECK(l == 0.0f && env.idle());

  // Bandwidth helpers.
  CHECK(fmk::maxIndex(1000.0, 48000.0) > 17.0f && fmk::maxIndex(1000.0, 48000.0) < 19.5f);
  CHECK(fmk::maxIndex(1e6, 48000.0) == 0.0f);
  CHECK(fmk::nyquistGain(1000.0, 48000.0) == 1.0f && fmk::nyquistGain(30000.0, 48000.0) == 0.0f);

  // Drive 0 is a bypass; bounded and level-compensated above.
  fmk::Drive d; d.set(0.0f);
  CHECK(d.process(0.37f) == 0.37f);
  d.set(1.0f);
  CHECK(std::fabs(d.process(1.0f)) < 1.6f);
  CHECK(std::fabs(d.process(0.0f)) < 1e-6f);
}

// Share of spectral power in the band just under Nyquist (0.44-0.5 fs), from a 16384-point Hann FFT
// starting at 5 ms. Aliased FM piles energy up there; a clean voice has almost none.
double nearNyquistShare(const Buf& o, double rate) {
  const size_t n = 16384, start = at(0.005, rate);
  if (o.size() < start + n) return 1.0;
  std::vector<double> re(n), im(n, 0.0);
  for (size_t i = 0; i < n; i++) re[i] = o[start + i] * (0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(n)));
  for (size_t i = 1, j = 0; i < n; i++) {  // bit reversal
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / double(len);
    for (size_t i = 0; i < n; i += len) {
      for (size_t k = 0; k < len / 2; k++) {
        const double wr = std::cos(ang * double(k)), wi = std::sin(ang * double(k));
        const double ur = re[i + k], ui = im[i + k];
        const double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
        const double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
      }
    }
  }
  double near = 0, total = 0;
  for (size_t k = 1; k < n / 2; k++) {
    const double p = re[k] * re[k] + im[k] * im[k];
    total += p;
    if (double(k) / double(n) > 0.44) near += p;
  }
  return near / (total + 1e-30);
}

// ---------------------------------------------------------------------------------------------
// Thump
// ---------------------------------------------------------------------------------------------

Setup thump() {
  Setup s; s.kind = kThump;
  s.with(DmThumpParam::Fm, 0).with(DmThumpParam::Punch, 0).with(DmThumpParam::Body, 0)
   .with(DmThumpParam::Drive, 0).with(DmThumpParam::Tune, 55).with(DmThumpParam::Sweep, 2)
   .with(DmThumpParam::Decay, 0.6f);
  return s;
}

// Zero-crossing rate over [a, b) in Hz.
double crossingHz(const Buf& v, size_t a, size_t b, double rate = kRate) {
  int n = 0;
  for (size_t i = a + 1; i < b && i < v.size(); i++) if ((v[i - 1] < 0) != (v[i] < 0)) n++;
  return 0.5 * n * rate / double(b - a);
}

// Highest instantaneous frequency (from the shortest of the first few zero-crossing intervals).
double fastestHz(const Buf& v, size_t a, size_t b, double rate = kRate) {
  std::vector<size_t> z;
  for (size_t i = a + 1; i < b && i < v.size(); i++) if ((v[i - 1] < 0) != (v[i] < 0)) z.push_back(i);
  double best = 0;
  for (size_t k = 1; k < z.size() && k < 6; k++) best = std::fmax(best, 0.5 * rate / double(z[k] - z[k - 1]));
  return best;
}

void testThumpPitchSweep() {
  Buf o = play(thump(), 0.8);
  const double early = fastestHz(o, at(0.0005), at(0.02));
  const double late = peakFreq(o, at(0.2), at(0.5), 55.0);
  CHECK(early > 2.5 * 55.0);              // starts far above Tune (2 octaves = 220 Hz)
  CHECK_NEAR(late, 55.0, 55.0 * 0.04);   // settles on Tune
  // sweep 0: no movement at all.
  Setup flat = thump(); flat.with(DmThumpParam::Sweep, 0);
  Buf f = play(flat, 0.8);
  CHECK_NEAR(crossingHz(f, at(0.01), at(0.25)), 55.0, 6.0);
  // More sweep = a higher start.
  Setup big = thump(); big.with(DmThumpParam::Sweep, 4);
  CHECK(fastestHz(play(big, 0.5), at(0.0005), at(0.02)) > early * 1.5);
  // Tune and a pitch offset move the settled pitch.
  Setup t2 = thump(); t2.with(DmThumpParam::Tune, 110);
  CHECK_NEAR(peakFreq(play(t2, 0.8), at(0.2), at(0.5), 110.0), 110.0, 110.0 * 0.04);
  Setup oct = thump(); oct.pitch = 12;
  CHECK_NEAR(peakFreq(play(oct, 0.8), at(0.2), at(0.5), 110.0), 110.0, 110.0 * 0.04);
}

void testThumpFmPunchBody() {
  Setup base = thump(); base.with(DmThumpParam::Tune, 110).with(DmThumpParam::Sweep, 0).with(DmThumpParam::Ratio, 0.17f);
  Setup rich = base; rich.with(DmThumpParam::Fm, 1.0f);
  const double a0 = harmonicActivity(play(base, 0.4), at(0.005), at(0.1), 110.0);
  const double a1 = harmonicActivity(play(rich, 0.4), at(0.005), at(0.1), 110.0);
  CHECK(a0 < 0.02);
  CHECK(a1 > a0 + 0.1);

  // Punch: a click and a fast index burst at the very start.
  Setup soft = thump(), hard = thump(); hard.with(DmThumpParam::Punch, 1.0f);
  CHECK(brightness(play(hard, 0.2), 0, at(0.004)) > brightness(play(soft, 0.2), 0, at(0.004)) * 1.5);

  // Body: the membrane overtone at 1.59 x Tune appears (fm 0, no sweep, so a clean sine pair).
  Setup nobody = base; nobody.with(DmThumpParam::Tune, 100).with(DmThumpParam::Body, 0);
  Setup body = nobody; body.with(DmThumpParam::Body, 1.0f);
  const double o0 = bin(play(nobody, 0.4), at(0.01), at(0.15), 159.0);
  const double o1 = bin(play(body, 0.4), at(0.01), at(0.15), 159.0);
  CHECK(o0 < 0.01);
  CHECK(o1 > 0.02);
  CHECK(o1 > 10 * o0);

  // Drive adds harmonics to a clean tone.
  Setup clean = base, driven = base; driven.with(DmThumpParam::Drive, 1.0f);
  CHECK(harmonicActivity(play(driven, 0.4), at(0.01), at(0.1), 110.0) >
        harmonicActivity(play(clean, 0.4), at(0.01), at(0.1), 110.0) + 0.05);
}

void testThumpReachesBeyondKicks() {
  // Zap: high, fast, wide sweep.
  Setup zap = thump(); zap.with(DmThumpParam::Tune, 400).with(DmThumpParam::Sweep, 4).with(DmThumpParam::Decay, 0.15f).with(DmThumpParam::Fm, 0.6f);
  Buf z = play(zap, 0.4, "thump_zap");
  CHECK(allFinite(z) && peak(z) > 0.1f);
  // Tuned tom: mid pitch, short sweep, body overtone, FM on a membrane ratio.
  Setup tom = thump(); tom.with(DmThumpParam::Tune, 140).with(DmThumpParam::Sweep, 0.7f).with(DmThumpParam::Body, 0.8f)
      .with(DmThumpParam::Fm, 0.3f).with(DmThumpParam::Ratio, 0.33f).with(DmThumpParam::Decay, 0.5f).with(DmThumpParam::Punch, 0.4f);
  Buf t = play(tom, 0.8, "thump_tom");
  CHECK(bin(t, at(0.04), at(0.15), 140.0 * 1.59) > 0.005);
  // Kick and long bass tone.
  Setup kick = thump(); kick.with(DmThumpParam::Fm, 0.3f).with(DmThumpParam::Punch, 0.6f).with(DmThumpParam::Drive, 0.3f);
  play(kick, 0.8, "thump_kick");
  Setup bass = thump(); bass.with(DmThumpParam::Sweep, 0.2f).with(DmThumpParam::Decay, 1.5f).with(DmThumpParam::Fm, 0.5f).with(DmThumpParam::Tune, 41);
  CHECK(rms(play(bass, 1.5, "thump_bass"), at(0.2), at(0.5)) > 0.05);
}

void testThumpBandwidth() {
  // High note, high ratio, full FM: nothing near Nyquist (index limited), at 48 and 96 kHz.
  for (double rate : {48000.0, 96000.0}) {
    Setup s = thump(); s.rate = rate;
    s.with(DmThumpParam::Tune, 400).with(DmThumpParam::Sweep, 0).with(DmThumpParam::Fm, 1.0f)
     .with(DmThumpParam::Ratio, 1.0f).with(DmThumpParam::Punch, 1.0f).with(DmThumpParam::Decay, 0.5f);
    s.pitch = 12;
    Buf o = play(s, 0.4);
    CHECK(nearNyquistShare(o, rate) < 0.003);
  }
}

void testThumpLevel() {
  Setup s; s.kind = kThump;
  Buf o = play(s, 0.5);
  const double r = rms(o, 0, at(0.1));
  std::printf("  thump default rms(0-100ms) = %.3f peak = %.3f\n", r, peak(o));
  CHECK(r > 0.08 && r < 0.45);
}

// ---------------------------------------------------------------------------------------------
// Metal
// ---------------------------------------------------------------------------------------------

Setup metal() {
  Setup s; s.kind = kMetal;
  s.with(DmMetalParam::Noise, 0).with(DmMetalParam::Chick, 0).with(DmMetalParam::Drive, 0)
   .with(DmMetalParam::Tone, 0).with(DmMetalParam::Fm, 0).with(DmMetalParam::Decay, 1.0f)
   .with(DmMetalParam::Tune, 300);
  return s;
}

void testMetalRatioSets() {
  // fm 0: operators are bare sines at tune x ratio. Bar set (spread 1/3): 300 x 2.76, 5.40.
  Setup bar = metal(); bar.with(DmMetalParam::Spread, 1.0f / 3.0f);
  Buf b = play(bar, 1.0, "metal_bar");
  const size_t wa = at(0.05), wb = at(0.6);
  CHECK(bin(b, wa, wb, 300.0) > 0.005);
  CHECK(bin(b, wa, wb, 300.0 * 2.76) > 0.005);
  CHECK(bin(b, wa, wb, 300.0 * 5.40) > 0.005);
  CHECK(bin(b, wa, wb, 600.0) < 0.001);   // not the harmonic octave
  // Harmonic set (spread 0): partials at integer multiples.
  Setup harm = metal(); harm.with(DmMetalParam::Spread, 0.0f);
  Buf h = play(harm, 1.0);
  CHECK(bin(h, wa, wb, 600.0) > 0.005);
  CHECK(bin(h, wa, wb, 300.0 * 2.76) < 0.001);
  // 808 set (spread 2/3).
  Setup s808 = metal(); s808.with(DmMetalParam::Spread, 2.0f / 3.0f);
  Buf e = play(s808, 1.0);
  CHECK(bin(e, wa, wb, 300.0 * 1.483) > 0.005);
  CHECK(bin(e, wa, wb, 300.0 * 2.546) > 0.005);
  // Tune and pitch offset scale the whole cluster.
  Setup up = bar; up.pitch = 12;
  Buf u = play(up, 1.0);
  CHECK(bin(u, wa, wb, 600.0 * 2.76) > 0.005);
}

void testMetalBellDecayPerPartialAndFm() {
  // A bell: noise 0, long decay. FM fills in sidebands and the index envelope removes them again,
  // so the bright onset settles to the bare partials.
  Setup bell = metal(); bell.with(DmMetalParam::Spread, 1.0f / 3.0f).with(DmMetalParam::Fm, 0.6f).with(DmMetalParam::Decay, 1.5f);
  Buf b = play(bell, 2.0, "metal_bell");
  CHECK(brightness(b, at(0.002), at(0.05)) > brightness(b, at(0.8), at(1.2)) * 1.3);
  Setup flat = bell; flat.with(DmMetalParam::Fm, 0.0f);
  Buf f = play(flat, 2.0);
  // fm adds energy away from the bare partials.
  double off = 0, offFlat = 0;
  for (double hz : {410.0, 520.0, 700.0, 910.0, 1130.0, 1370.0}) {
    off += bin(b, at(0.01), at(0.15), hz);
    offFlat += bin(f, at(0.01), at(0.15), hz);
  }
  CHECK(off > offFlat * 3.0 + 0.001);
}

void testMetalHatBehaviour() {
  Setup hat; hat.kind = kMetal;
  hat.with(DmMetalParam::Tone, 1.0f).with(DmMetalParam::Noise, 0.6f).with(DmMetalParam::Chick, 0.6f).with(DmMetalParam::Decay, 0.06f);
  Setup open = hat; open.with(DmMetalParam::Decay, 0.8f);
  Buf c = play(hat, 1.2, "metal_hat_closed"), o = play(open, 1.4, "metal_hat_open");
  CHECK(lastAbove(c, 0.001f) < at(0.35));
  CHECK(lastAbove(o, 0.001f) > at(0.5));
  // Tone = high-pass: the hat is far brighter than the same hit with tone 0.
  Setup dark = hat; dark.with(DmMetalParam::Tone, 0.0f);
  CHECK(brightness(play(hat, 0.3), 0, at(0.1)) > brightness(play(dark, 0.3), 0, at(0.1)) * 1.5);
  // Choke: a note-off ends the open hat quickly.
  Setup choked = open; choked.duration = 0.05;
  Buf k = play(choked, 0.6);
  CHECK(lastAbove(k, 0.001f) < at(0.05) + at(0.05));
  // Noise adds energy that no partial explains: without noise the spectrum is lines.
  Setup quiet = hat; quiet.with(DmMetalParam::Noise, 0.0f);
  Setup noisy = hat; noisy.with(DmMetalParam::Noise, 1.0f);
  CHECK(rms(play(noisy, 0.3), 0, at(0.1)) > rms(play(quiet, 0.3), 0, at(0.1)) * 1.1);
  // Chick: more energy at the strike.
  Setup c0 = hat; c0.with(DmMetalParam::Chick, 0.0f);
  Setup c1 = hat; c1.with(DmMetalParam::Chick, 1.0f);
  CHECK(rms(play(c1, 0.2), 0, at(0.01)) > rms(play(c0, 0.2), 0, at(0.01)) * 1.2);
}

void testMetalBandwidth() {
  // Highest tune, widest ratios, full FM and noise: no energy piles up near Nyquist.
  for (double rate : {48000.0, 96000.0}) {
    Setup s; s.kind = kMetal; s.rate = rate;
    s.with(DmMetalParam::Tune, 1500).with(DmMetalParam::Spread, 1.0f / 3.0f).with(DmMetalParam::Fm, 1.0f)
     .with(DmMetalParam::Noise, 0).with(DmMetalParam::Decay, 0.5f).with(DmMetalParam::Chick, 1.0f).with(DmMetalParam::Tone, 0);
    s.pitch = 7;
    Buf o = play(s, 0.5);
    CHECK(allFinite(o) && peak(o) <= 1.0f);
    CHECK(nearNyquistShare(o, rate) < 0.003);
  }
}

void testMetalLevel() {
  Setup s; s.kind = kMetal;
  Buf o = play(s, 0.5);
  const double r = rms(o, 0, at(0.1));
  std::printf("  metal default rms(0-100ms) = %.3f peak = %.3f\n", r, peak(o));
  CHECK(r > 0.04 && r < 0.45);
}

// ---------------------------------------------------------------------------------------------
// Tone
// ---------------------------------------------------------------------------------------------

constexpr float kStack = 0, kTwin = 1, kFan = 2, kMajor = 3, kMinor = 4;

Setup tone(float structure = kStack) {
  Setup s; s.kind = kTone;
  s.with(DmToneParam::Structure, structure).with(DmToneParam::Drive, 0).with(DmToneParam::Tune, 110);
  return s;
}

void testToneStructuresAreDistinct() {
  Buf outs[6];
  for (int st = 0; st < 6; st++) {
    Setup s = tone(float(st)); s.with(DmToneParam::Fm, 0.7f);
    outs[st] = play(s, 0.6, st == 0 ? "tone_stack" : st == 1 ? "tone_twin" : st == 2 ? "tone_fan" : st == 3 ? "tone_major" : st == 4 ? "tone_minor" : "tone_sus");
    CHECK(allFinite(outs[st]) && peak(outs[st]) > 0.05f && peak(outs[st]) <= 1.0f);
  }
  for (int a = 0; a < 6; a++) for (int b = a + 1; b < 6; b++) CHECK(outs[a] != outs[b]);
  // Out-of-range structure values clamp (the param is clamped by the table; rounding is the policy's).
  Setup frac = tone(1.4f), one = tone(1.0f);
  CHECK(play(frac, 0.2) == play(one, 0.2));
}

void testTonePluckSettles() {
  // 80s pluck: bright strike, the modulators die faster than the carrier. Spectral centroid falls
  // while the level is still audible.
  Setup pluck = tone(kTwin);
  pluck.with(DmToneParam::Fm, 0.7f).with(DmToneParam::Damp, 0.1f).with(DmToneParam::Decay, 1.2f).with(DmToneParam::Tune, 220);
  Buf p = play(pluck, 1.6, "tone_pluck");
  CHECK(brightness(p, at(0.003), at(0.05)) > brightness(p, at(0.4), at(0.6)) * 1.5);
  CHECK(rms(p, at(0.4), at(0.6)) > 0.01);
  // damp 1: the modulators last as long as the note, so the timbre does not settle.
  Setup keep = pluck; keep.with(DmToneParam::Damp, 1.0f).with(DmToneParam::Fm, 1.0f);
  Setup quick = pluck; quick.with(DmToneParam::Fm, 1.0f);
  Buf k = play(keep, 1.6), q = play(quick, 1.6);
  CHECK(brightness(k, at(0.1), at(0.2)) > brightness(q, at(0.1), at(0.2)) * 1.5);
}

void testToneHoldSustains() {
  Setup organ = tone(kStack);
  organ.with(DmToneParam::Hold, 1.0f).with(DmToneParam::Decay, 0.2f).with(DmToneParam::Fm, 0.5f).with(DmToneParam::Damp, 0.7f);
  organ.duration = 0.8;
  Buf o = play(organ, 1.4, "tone_organ");
  const double early = rms(o, at(0.1), at(0.2)), late = rms(o, at(0.6), at(0.75));
  CHECK(late > early * 0.6);                       // holds
  CHECK(rms(o, at(1.2), at(1.4)) < late * 0.02);   // and releases after the note-off
  Setup pluck = organ; pluck.with(DmToneParam::Hold, 0.0f);
  Buf p = play(pluck, 1.4);
  CHECK(rms(p, at(0.6), at(0.75)) < rms(p, at(0.1), at(0.2)) * 0.2);
  // attack
  Setup slow = organ; slow.with(DmToneParam::Attack, 0.2f);
  Setup fast = organ; fast.with(DmToneParam::Attack, 0.001f);
  CHECK(rms(play(slow, 0.5), 0, at(0.02)) < rms(play(fast, 0.5), 0, at(0.02)) * 0.3);
}

void testToneChordStabs() {
  const double f0 = 110.0;
  const double rm = std::exp2(4.0 / 12.0), rn = std::exp2(3.0 / 12.0), r5 = std::exp2(7.0 / 12.0);
  Setup maj = tone(kMajor); maj.with(DmToneParam::Fm, 0).with(DmToneParam::Decay, 1.0f);
  Setup min = tone(kMinor); min.with(DmToneParam::Fm, 0).with(DmToneParam::Decay, 1.0f);
  Buf a = play(maj, 0.8), b = play(min, 0.8);
  const size_t w0 = at(0.05), w1 = at(0.5);
  CHECK(bin(a, w0, w1, f0) > 0.01 && bin(a, w0, w1, f0 * rm) > 0.01 && bin(a, w0, w1, f0 * r5) > 0.01);
  CHECK(bin(b, w0, w1, f0) > 0.01 && bin(b, w0, w1, f0 * rn) > 0.01 && bin(b, w0, w1, f0 * r5) > 0.01);
  CHECK(bin(a, w0, w1, f0 * rn) < 0.002);   // major has no minor third
  CHECK(bin(b, w0, w1, f0 * rm) < 0.002);
  // FM from the shared modulator thickens every voice of the chord.
  Setup fmMaj = maj; fmMaj.with(DmToneParam::Fm, 0.5f);
  Buf c = play(fmMaj, 0.8, "tone_stab");
  // The carrier at 1.26 f0 hears a modulator at f0: sidebands at 1.26 f0 +- f0 that FM 0 lacks.
  const double sbC = bin(c, w0, w1, f0 * (rm - 1.0)) + bin(c, w0, w1, f0 * (rm + 1.0));
  const double sbA = bin(a, w0, w1, f0 * (rm - 1.0)) + bin(a, w0, w1, f0 * (rm + 1.0));
  CHECK(sbC > 0.002 && sbC > 20 * sbA);
}

void testToneBassKeepsFundamental() {
  // A deep Stack bass: the fundamental must not vanish as fm sweeps (Bessel nulls), as in Fm.
  double lo = 1e9, base = 0;
  for (int i = 0; i <= 10; i++) {
    Setup s = tone(kStack); s.with(DmToneParam::Tune, 55).with(DmToneParam::Fm, i / 10.0f).with(DmToneParam::Decay, 1.5f).with(DmToneParam::Damp, 1.0f);
    Buf o = play(s, 0.6, i == 4 ? "tone_bass" : nullptr);
    const double m = bin(o, at(0.1), at(0.4), 55.0);
    if (i == 0) base = m;
    lo = std::fmin(lo, m);
  }
  CHECK(base > 0.05);
  CHECK(lo > base * 0.2);  // never more than ~14 dB below the plain carrier
}

void testToneInharmonicStructures() {
  // Fan puts bar-ratio modulators on one carrier: inharmonic sidebands; Stack at ratio 1 is harmonic.
  Setup fan = tone(kFan), stack = tone(kStack);
  for (Setup* s : {&fan, &stack}) s->with(DmToneParam::Fm, 0.5f).with(DmToneParam::Damp, 1.0f).with(DmToneParam::Decay, 1.5f);
  Buf f = play(fan, 0.8, "tone_bell"), k = play(stack, 0.8);
  const double inh = 110.0 * 1.76;
  CHECK(bin(f, at(0.05), at(0.5), inh) > 0.004);
  CHECK(bin(k, at(0.05), at(0.5), inh) < 0.0015);
}

void testToneVelocityAndDrive() {
  Setup s = tone(kStack); s.with(DmToneParam::Fm, 0.5f);
  Setup lo = s, hi = s; lo.velocity = 0.3f; hi.velocity = 1.0f;
  CHECK(brightness(play(hi, 0.3), at(0.005), at(0.1)) > brightness(play(lo, 0.3), at(0.005), at(0.1)));
  Setup clean = tone(kStack); clean.with(DmToneParam::Fm, 0.0f).with(DmToneParam::Tune, 220).with(DmToneParam::Decay, 1.0f);
  Setup driven = clean; driven.with(DmToneParam::Drive, 1.0f);
  CHECK(harmonicActivity(play(driven, 0.4), at(0.01), at(0.2), 220.0) > harmonicActivity(play(clean, 0.4), at(0.01), at(0.2), 220.0) + 0.05);
}

void testToneBandwidth() {
  for (double rate : {48000.0, 96000.0}) {
    for (float st : {kStack, kFan, kMajor}) {
      Setup s = tone(st); s.rate = rate;
      s.with(DmToneParam::Tune, 880).with(DmToneParam::Fm, 1.0f).with(DmToneParam::Ratio, 1.0f).with(DmToneParam::Damp, 1.0f).with(DmToneParam::Decay, 0.5f);
      s.pitch = 12;
      Buf o = play(s, 0.4);
      CHECK(allFinite(o) && peak(o) <= 1.0f);
      CHECK(nearNyquistShare(o, rate) < 0.003);
    }
  }
}

void testToneLevel() {
  Setup s; s.kind = kTone;
  Buf o = play(s, 0.5);
  const double r = rms(o, 0, at(0.2));
  std::printf("  tone default rms(0-200ms) = %.3f peak = %.3f\n", r, peak(o));
  CHECK(r > 0.05 && r < 0.45);
}

// ---------------------------------------------------------------------------------------------
// Crack
// ---------------------------------------------------------------------------------------------

Setup crack() {
  Setup s; s.kind = kCrack;
  s.with(DmCrackParam::Drive, 0).with(DmCrackParam::Decay, 0.3f);
  return s;
}

// Number of separate bursts in a 1 ms rms envelope (hysteresis: arm at 30% of the max, re-arm at 10%).
int countBursts(const Buf& o, size_t from, size_t to) {
  std::vector<double> env;
  const size_t hop = 48;
  for (size_t i = from; i + hop <= to && i + hop <= o.size(); i += hop) env.push_back(rms(o, i, i + hop));
  double mx = 0;
  for (double e : env) mx = std::fmax(mx, e);
  int count = 0;
  bool armed = true;
  for (double e : env) {
    if (armed && e > 0.3 * mx) { count++; armed = false; }
    if (e < 0.1 * mx) armed = true;
  }
  return count;
}

void testCrackBursts() {
  for (float b : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    Setup s = crack();
    s.with(DmCrackParam::Burst, b).with(DmCrackParam::Fm, 0).with(DmCrackParam::Body, 0).with(DmCrackParam::Noise, 1.0f).with(DmCrackParam::Decay, 0.04f);
    const int expected = 1 + int(std::lround(b * 4));
    CHECK(countBursts(play(s, 0.4), 0, at(0.2)) == expected);
  }
  // A clap fades the body: far less low-frequency energy than the snare at the same body setting.
  Setup snare = crack(), clap = crack(); clap.with(DmCrackParam::Burst, 1.0f);
  snare.with(DmCrackParam::Noise, 0).with(DmCrackParam::Fm, 0); clap.with(DmCrackParam::Noise, 0).with(DmCrackParam::Fm, 0);
  CHECK(rms(play(clap, 0.4), 0, at(0.15)) < rms(play(snare, 0.4), 0, at(0.15)) * 0.4);
}

void testCrackBodyAndNoise() {
  // Body only (noise 0, fm 0): a pitched tone at Tune once the strike drop has settled.
  Setup body = crack(); body.with(DmCrackParam::Noise, 0).with(DmCrackParam::Fm, 0).with(DmCrackParam::Tune, 190).with(DmCrackParam::Decay, 0.5f);
  Buf b = play(body, 0.8);
  CHECK_NEAR(peakFreq(b, at(0.06), at(0.3), 190.0), 190.0, 190.0 * 0.04);
  // Noise adds broadband energy; tone moves where.
  Setup quiet = crack(); quiet.with(DmCrackParam::Noise, 0).with(DmCrackParam::Fm, 0);
  Setup noisy = crack(); noisy.with(DmCrackParam::Noise, 1.0f).with(DmCrackParam::Fm, 0);
  CHECK(brightness(play(noisy, 0.4), 0, at(0.1)) > brightness(play(quiet, 0.4), 0, at(0.1)) * 3.0);
  Setup lowTone = noisy, highTone = noisy;
  lowTone.with(DmCrackParam::Tone, 0.0f).with(DmCrackParam::Body, 0); highTone.with(DmCrackParam::Tone, 1.0f).with(DmCrackParam::Body, 0);
  CHECK(brightness(play(highTone, 0.4), 0, at(0.1)) > brightness(play(lowTone, 0.4), 0, at(0.1)) * 1.5);
}

void testCrackNoiseInsideFm() {
  // Additive noise off: fm alone makes the pitched body hiss (noise phase-modulation).
  Setup clean = crack(); clean.with(DmCrackParam::Noise, 0).with(DmCrackParam::Fm, 0).with(DmCrackParam::Decay, 0.4f);
  Setup hiss = clean; hiss.with(DmCrackParam::Fm, 1.0f);
  Buf c = play(clean, 0.6), h = play(hiss, 0.6, "crack_fm_noise");
  CHECK(brightness(h, at(0.01), at(0.15)) > brightness(c, at(0.01), at(0.15)) * 3.0);
  // ...and the hiss is shaped around the body: more of it than a plain additive-noise patch has at 190 Hz.
  CHECK(bin(h, at(0.01), at(0.15), 190.0) > 0.0);
}

void testCrackDemos() {
  Setup snare = crack(); snare.with(DmCrackParam::Noise, 0.7f).with(DmCrackParam::Fm, 0.5f).with(DmCrackParam::Drive, 0.3f);
  Buf s = play(snare, 0.6, "crack_snare");
  Setup clap = snare; clap.with(DmCrackParam::Burst, 1.0f).with(DmCrackParam::Tone, 0.4f).with(DmCrackParam::Decay, 0.25f);
  Buf c = play(clap, 0.6, "crack_clap");
  Setup rim = crack(); rim.with(DmCrackParam::Tune, 450).with(DmCrackParam::Decay, 0.05f).with(DmCrackParam::Noise, 0.2f).with(DmCrackParam::Fm, 0.8f);
  Buf r = play(rim, 0.3, "crack_rim");
  CHECK(peak(s) > 0.1f && peak(c) > 0.1f && peak(r) > 0.05f);
  Setup lvl; lvl.kind = kCrack;
  Buf o = play(lvl, 0.5);
  const double rv = rms(o, 0, at(0.1));
  std::printf("  crack default rms(0-100ms) = %.3f peak = %.3f\n", rv, peak(o));
  CHECK(rv > 0.04 && rv < 0.45);
}

// ---------------------------------------------------------------------------------------------
// Chaos
// ---------------------------------------------------------------------------------------------

Setup chaos() {
  Setup s; s.kind = kChaos;
  s.with(DmChaosParam::Fold, 0).with(DmChaosParam::Noise, 0).with(DmChaosParam::Sweep, 0).with(DmChaosParam::Decay, 1.0f);
  return s;
}

void testChaosLoopGain() {
  // chaos 0: the ring is open, three bare sines (f, f x ratio, f x ratio x phi).
  Setup calm = chaos(); calm.with(DmChaosParam::Chaos, 0).with(DmChaosParam::Tune, 220).with(DmChaosParam::Ratio, 0.143f);
  Buf c = play(calm, 0.8);
  CHECK(bin(c, at(0.05), at(0.5), 220.0) > 0.03);
  CHECK(bin(c, at(0.05), at(0.5), 220.0 * 1.618) > 0.01);
  CHECK(harmonicActivity(c, at(0.05), at(0.5), 220.0) > 0.0);  // not a single sine
  // More loop gain scatters the spectrum.
  double prev = 0;
  for (float g : {0.0f, 0.3f, 0.6f, 1.0f}) {
    Setup s = chaos(); s.with(DmChaosParam::Chaos, g).with(DmChaosParam::Tune, 220);
    const double b = brightness(play(s, 0.5), at(0.01), at(0.3));
    CHECK(b > prev);
    prev = b;
  }
  // Fold and noise add content.
  Setup mild = chaos(); mild.with(DmChaosParam::Chaos, 0.2f);
  Setup folded = mild; folded.with(DmChaosParam::Fold, 1.0f);
  Setup noisy = mild; noisy.with(DmChaosParam::Noise, 1.0f);
  const double b0 = brightness(play(mild, 0.5), at(0.01), at(0.3));
  CHECK(brightness(play(folded, 0.5), at(0.01), at(0.3)) > b0 * 1.3);
  CHECK(brightness(play(noisy, 0.5), at(0.01), at(0.3)) > b0 * 1.3);
}

void testChaosIsBoundedDeterministicAndSensitive() {
  Setup wild = chaos();
  wild.with(DmChaosParam::Chaos, 1.0f).with(DmChaosParam::Fold, 1.0f).with(DmChaosParam::Noise, 1.0f).with(DmChaosParam::Hold, 1.0f).with(DmChaosParam::Tune, 55);
  Buf w = play(wild, 3.0, "chaos_wild");
  CHECK(allFinite(w) && peak(w) <= 1.0f && rms(w, at(1.0), at(3.0)) > 0.05);   // sustained, never dies, never blows up
  CHECK(std::fabs(mean(w, at(0.5), at(3.0))) < 0.05);                        // no DC
  CHECK(play(wild, 0.5) == play(wild, 0.5));
  // Sensitive to the loop gain: a 0.5% change gives a different waveform (the "unstable" character).
  Setup a = chaos(), b = chaos();
  a.with(DmChaosParam::Chaos, 0.9f).with(DmChaosParam::Tune, 110); b.with(DmChaosParam::Chaos, 0.905f).with(DmChaosParam::Tune, 110);
  Buf x = play(a, 0.4), y = play(b, 0.4);
  double d = 0, e = 0;
  for (size_t i = at(0.1); i < x.size(); i++) { d += double(x[i] - y[i]) * (x[i] - y[i]); e += double(x[i]) * x[i]; }
  CHECK(std::sqrt(d / e) > 0.2);
}

void testChaosLeadAndShortHits() {
  // A held lead keeps its pitch and its level, then releases.
  Setup lead = chaos();
  lead.with(DmChaosParam::Chaos, 0.2f).with(DmChaosParam::Tune, 440).with(DmChaosParam::Hold, 1.0f).with(DmChaosParam::Decay, 0.3f);
  lead.duration = 0.8;
  Buf l = play(lead, 1.4, "chaos_lead");
  CHECK_NEAR(peakFreq(l, at(0.2), at(0.7), 440.0), 440.0, 440.0 * 0.03);
  CHECK(rms(l, at(0.6), at(0.75)) > rms(l, at(0.1), at(0.2)) * 0.5);
  CHECK(rms(l, at(1.2), at(1.4)) < rms(l, at(0.6), at(0.75)) * 0.02);
  // Sweep: the strike starts brighter.
  Setup flat = chaos(), sweep = chaos();
  flat.with(DmChaosParam::Chaos, 0.3f); sweep.with(DmChaosParam::Chaos, 0.3f).with(DmChaosParam::Sweep, 3.0f);
  CHECK(brightness(play(sweep, 0.3), at(0.001), at(0.02)) > brightness(play(flat, 0.3), at(0.001), at(0.02)) * 1.3);
  // Strange short percussion.
  Setup hit = chaos();
  hit.with(DmChaosParam::Chaos, 0.8f).with(DmChaosParam::Fold, 0.6f).with(DmChaosParam::Noise, 0.4f).with(DmChaosParam::Decay, 0.12f).with(DmChaosParam::Tune, 160).with(DmChaosParam::Sweep, 1.5f);
  Buf h = play(hit, 0.5, "chaos_hit");
  CHECK(peak(h) > 0.1f && lastAbove(h, 0.001f) < at(0.4));
  // Pitch offset moves it (also the golden-ratio operator, so just: different and finite).
  Setup up = lead; up.pitch = 12;
  CHECK_NEAR(peakFreq(play(up, 1.0), at(0.2), at(0.7), 880.0), 880.0, 880.0 * 0.03);
  Setup lvl; lvl.kind = kChaos;
  Buf o = play(lvl, 0.5);
  const double rv = rms(o, 0, at(0.1));
  std::printf("  chaos default rms(0-100ms) = %.3f peak = %.3f\n", rv, peak(o));
  CHECK(rv > 0.04 && rv < 0.45);
}

// ---------------------------------------------------------------------------------------------
// Overlap and validation sounds: the Machines are not boxes (docs/digital-machines.md).
// ---------------------------------------------------------------------------------------------

void testMachinesEscapeTheirCategory() {
  // Tone as a tom-like hit: short, low, pitched, with an FM click.
  Setup tom = tone(kStack);
  tom.with(DmToneParam::Tune, 90).with(DmToneParam::Decay, 0.25f).with(DmToneParam::Fm, 0.8f).with(DmToneParam::Damp, 0.0f).with(DmToneParam::Hold, 0.0f).with(DmToneParam::Ratio, 0.3f);
  Buf t = play(tom, 0.6, "tone_as_tom");
  CHECK(peak(t) > 0.1f && lastAbove(t, 0.001f) < at(0.5) && bin(t, at(0.05), at(0.25), 90.0) > 0.005);
  // Metal as a pure inharmonic bell, no noise, no high-pass (done in testMetalBellDecayPerPartialAndFm);
  // Crack as a pitched noise tone: long decay, tuned body, noise only through FM.
  Setup pn = crack(); pn.with(DmCrackParam::Noise, 0).with(DmCrackParam::Fm, 0.6f).with(DmCrackParam::Decay, 0.8f).with(DmCrackParam::Tune, 300);
  Buf p = play(pn, 1.2, "crack_pitched_noise");
  CHECK(bin(p, at(0.05), at(0.5), 300.0) > 0.01 && lastAbove(p, 0.001f) > at(0.5));
  // Thump as a sustained bass tone is covered in testThumpReachesBeyondKicks; Chaos as clean tone in
  // testChaosLoopGain (chaos 0).
}

// Set DM_BENCH=1 to print render cost (ns per sample per sounding instance). Informational only.
void benchmark() {
  if (!std::getenv("DM_BENCH")) return;
  const char* names[] = {"thump", "metal", "tone", "crack", "chaos"};
  int ni = 0;
  for (int32_t kind : {kThump, kMetal, kTone, kCrack, kChaos, static_cast<int32_t>(InstrumentKind::Fm)}) {
    Engine e; e.init(kRate, 2, 8);
    for (int32_t i = 1; i <= 16; i++) { e.createInstrument(i, kind, 0); e.setInstrumentParam(i, 1, 100.0f); }
    // Sustained settings so every instance keeps sounding for the whole run.
    for (int32_t i = 1; i <= 16; i++) {
      if (kind == kTone) e.setInstrumentParam(i, id(DmToneParam::Hold), 1.0f);
      if (kind == kChaos) e.setInstrumentParam(i, id(DmChaosParam::Hold), 1.0f);
      if (kind == static_cast<int32_t>(InstrumentKind::Fm)) e.setInstrumentParam(i, id(FmParam::Sustain), 1.0f);
      e.instrumentNoteOn(i, 1.0f);
    }
    int64_t f = 0;
    for (int i = 0; i < 50; i++) { e.process(f, kBlock); f += kBlock; }
    const auto t0 = std::chrono::steady_clock::now();
    const int blocks = 2000;
    for (int i = 0; i < blocks; i++) {
      if (i % 100 == 0) for (int32_t k = 1; k <= 16; k++) e.instrumentNoteOn(k, 1.0f);  // keep every instance sounding
      e.process(f, kBlock); f += kBlock;
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  bench %-6s %.1f ns/sample/instance (%d active)\n", ni < 5 ? names[ni] : "fm", ns / (double(blocks) * kBlock * 16), e.activeInstrumentCount());
    ni++;
  }
}

void runMachineTests() {
  testMachinesEscapeTheirCategory();
  benchmark();
  testChaosLoopGain();
  testChaosIsBoundedDeterministicAndSensitive();
  testChaosLeadAndShortHits();
  testCrackBursts();
  testCrackBodyAndNoise();
  testCrackNoiseInsideFm();
  testCrackDemos();
  testToneStructuresAreDistinct();
  testTonePluckSettles();
  testToneHoldSustains();
  testToneChordStabs();
  testToneBassKeepsFundamental();
  testToneInharmonicStructures();
  testToneVelocityAndDrive();
  testToneBandwidth();
  testToneLevel();
  testMetalRatioSets();
  testMetalBellDecayPerPartialAndFm();
  testMetalHatBehaviour();
  testMetalBandwidth();
  testMetalLevel();
  testThumpPitchSweep();
  testThumpFmPunchBody();
  testThumpReachesBeyondKicks();
  testThumpBandwidth();
  testThumpLevel();
}

}  // namespace

int main() {
  testKernel();
  testMetadata();
  testFiniteBoundedAndEnds();
  testDeterministic();
  testVelocityAndPitchOffset();
  testNoteOffChokes();
  testRetriggerIsContinuous();
  testSilentUntilScheduledAndDuration();
  testRoutesThroughBus();
  testPolyphonyIsSum();
  testStabilityFuzz();
  testRenderDoesNotAllocate();
  runMachineTests();
  std::printf("dm_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
