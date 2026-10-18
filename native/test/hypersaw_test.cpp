#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the synthesized Hypersaw (instruments/hypersaw.h) through the Engine's instrument API: lifecycle,
// detune, spread, density, stereo/mono behaviour, bounds, polyphony and allocation.
// Set HYPERSAW_WAV_DIR=<dir> to dump the stereo cases as 16-bit WAVs for listening (docs/hypersaw-instrument.md).
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
constexpr int32_t kHs = static_cast<int32_t>(InstrumentKind::Hypersaw);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
struct Stereo { Buf l, r; };
int32_t id(HypersawParam p) { return static_cast<int32_t>(p); }
size_t at(double s) { return static_cast<size_t>(s * kRate); }

void run(Engine& e, int64_t& frame, double seconds, Stereo* o) {
  const int32_t frames = static_cast<int32_t>(std::ceil(seconds * kRate / kBlock)) * kBlock;
  for (int32_t d = 0; d < frames; d += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    o->l.insert(o->l.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
    o->r.insert(o->r.end(), e.outputChannel(1), e.outputChannel(1) + kBlock);
  }
}

void writeWav(const char* name, const Stereo& s) {
  const char* dir = std::getenv("HYPERSAW_WAV_DIR");
  if (!dir) return;
  std::string path = std::string(dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(s.l.size()), bytes = n * 4, rate = 48000;
  const uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
  const uint32_t riff = 36 + bytes, fmt = 16, byteRate = rate * 4;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    for (const Buf* b : {&s.l, &s.r}) {
      const int16_t v = static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, (*b)[i])) * 32767);
      std::fwrite(&v, 2, 1, f);
    }
  }
  std::fclose(f);
}

struct Rig {
  Engine e;
  int64_t f = 0;
  Stereo o;
  explicit Rig(int32_t bus = 0) { e.init(kRate, 2, 8); e.createInstrument(1, kHs, bus); }
  void set(HypersawParam p, float v, int32_t inst = 1) { e.setInstrumentParam(inst, id(p), v); }
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
// Strongest DFT magnitude within +-3% of f (the detuned saws spread around it).
double band(const Buf& v, size_t a, size_t b, double f) {
  double best = 0;
  for (double hz = f * 0.97; hz <= f * 1.03; hz += 0.25) best = std::fmax(best, bin(v, a, b, hz));
  return best;
}
// Left + right. Single-channel level wobbles with the stereo ensemble (each channel hears a
// different mix of the detuned saws), so level-over-time checks use the sum.
Buf mono(const Stereo& s) {
  Buf m(s.l.size());
  for (size_t i = 0; i < m.size(); i++) m[i] = 0.5f * (s.l[i] + s.r[i]);
  return m;
}
bool finite(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}


using P = HypersawParam;

// Energy of L and R (sum and difference): mid/side. Returns side/mid RMS ratio.
double sideOverMid(const Stereo& s, size_t a, size_t b) {
  Buf m(s.l.size()), d(s.l.size());
  for (size_t i = 0; i < m.size(); i++) { m[i] = 0.5f * (s.l[i] + s.r[i]); d[i] = 0.5f * (s.l[i] - s.r[i]); }
  return rms(d, a, b) / std::fmax(rms(m, a, b), 1e-9);
}

Stereo render(const std::vector<std::pair<P, float>>& params, double seconds = 1.5, float pitch = 0.0f) {
  Rig r;
  r.set(P::Attack, 0.005f);
  for (auto& [p, v] : params) r.set(p, v);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}

void testRegistrationAndDefaults() {
  const InstrumentKindMeta* m = instrumentKindMeta(kHs);
  CHECK(m && std::strcmp(m->id, "hypersaw") == 0 && std::strcmp(m->name, "Hypersaw") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(instrumentParamCount(kHs) == kHypersawParamCount && kHypersawParamCount == 9);
  auto in = makeInstrument(kHs);
  CHECK(in && in->kind() == InstrumentKind::Hypersaw && !in->isActive());
  for (int32_t i = 0; i < kHypersawParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kHs, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max);
  }
  CHECK(instrumentParamInfo(kHs, kHypersawParamCount) == nullptr);
  auto find = [](const char* n) { for (int32_t i = 0; i < kHypersawParamCount; i++) if (!std::strcmp(kHypersawParams[i].name, n)) return &kHypersawParams[i]; return (const InstrumentParamInfo*)nullptr; };
  CHECK_NEAR(find("detune")->def, 25.0, 1e-6);
  CHECK(std::strcmp(find("detune")->unit, "ct") == 0);
  CHECK(resolveModDomain(*find("detune")) == ModDomain::Linear);
  CHECK(resolveModDomain(*find("cutoff")) == ModDomain::Log);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kHs, 0));
}

void testNoteStartAndRelease() {
  Rig r;
  r.set(P::Attack, 0.02f); r.set(P::Release, 0.3f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.8 * kRate));
  r.go(0.05);
  CHECK(r.e.activeInstrumentCount() == 1);
  CHECK(peak(r.o.l, 0, 8) < 0.01f);               // starts from silence: no click
  r.go(0.75);
  const double held = rms(r.o.l, at(0.5), at(0.75));
  CHECK(held > 0.05 && held < 0.7);
  r.go(0.6);
  CHECK(r.e.activeInstrumentCount() == 0);          // 0.8 s note + 0.3 s release is over
  CHECK(peak(r.o.l, at(1.3), r.o.l.size()) == 0.0f);
  CHECK(finite(r.o.l) && finite(r.o.r));
  writeWav("default_note", r.o);
}

void testDeterministic() {
  const Stereo a = render({}, 0.6), b = render({}, 0.6);
  CHECK(a.l == b.l && a.r == b.r);
}

void testNoteOffReleasesAndSustainZeroEnds() {
  Rig r;
  r.set(P::Release, 0.2f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.5);
  CHECK(r.e.activeInstrumentCount() == 1);
  r.e.instrumentNoteOff(1);
  r.go(0.5);
  CHECK(r.e.activeInstrumentCount() == 0);
  Rig z;  // sustain 0: the note decays away by itself
  z.set(P::Sustain, 0.0f); z.set(P::Decay, 0.1f);
  z.e.instrumentNoteOn(1, 1.0f);
  z.go(0.6);
  CHECK(z.e.activeInstrumentCount() == 0);
}

void testRetriggerIsContinuous() {
  Rig r;
  r.set(P::Attack, 0.01f); r.set(P::Cutoff, 2000.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.5);
  const size_t t = r.o.l.size();
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 7.0f);   // a note on a held note: carries phase/filter/level
  r.go(0.3);
  CHECK(finite(r.o.l) && finite(r.o.r));
  double steady = 0, around = 0;                  // largest sample step, steady vs across the retrigger
  for (size_t i = at(0.3) + 1; i < t - 1; i++) steady = std::fmax(steady, std::fabs(r.o.l[i] - r.o.l[i - 1]));
  for (size_t i = t - 64; i < t + 64; i++) around = std::fmax(around, std::fabs(r.o.l[i] - r.o.l[i - 1]));
  CHECK(around <= steady * 1.5);
}

void testMultipleSimultaneousNotes() {
  Engine e; e.init(kRate, 2, 8);
  const float chord[] = {0, 4, 7, 11, 14, 19};
  for (int i = 0; i < 6; i++) {
    e.createInstrument(10 + i, kHs, 0);
    e.setInstrumentParam(10 + i, id(P::Attack), 0.05f);
    e.setInstrumentParam(10 + i, id(P::Release), 0.3f);
  }
  int64_t f = 0; Stereo o;
  for (int i = 0; i < 6; i++) e.instrumentNoteOn(10 + i, 0.8f, (i - 2.5f) * 0.1f, chord[i]);
  run(e, f, 1.0, &o);
  CHECK(e.activeInstrumentCount() == 6);
  CHECK(finite(o.l) && finite(o.r));
  CHECK(peak(o.l) <= 6.0f && peak(o.r) <= 6.0f);
  CHECK(rms(o.l, at(0.5), at(1.0)) > 0.1);
  for (int i = 0; i < 6; i++) e.instrumentNoteOff(10 + i);
  run(e, f, 0.6, &o);
  CHECK(e.activeInstrumentCount() == 0);
  writeWav("chord", o);
}

// Detune is in cents: 0 collapses the seven saws onto one line; the outer pair lands exactly
// +-detune cents from the note.
void testDetuneParameter() {
  const double f0 = 110.0;
  const Stereo tight = render({{P::Detune, 0.0f}, {P::Density, 1.0f}, {P::Cutoff, 16000.0f}}, 2.0);
  const Stereo wide = render({{P::Detune, 100.0f}, {P::Density, 1.0f}, {P::Cutoff, 16000.0f}}, 2.0);
  const double hi = f0 * std::pow(2.0, 100.0 / 1200.0), lo = f0 * std::pow(2.0, -100.0 / 1200.0);
  const Buf mt = mono(tight), mw = mono(wide);
  CHECK(bin(mt, at(0.5), at(2.0), f0) > 0.5 * rms(mt, at(0.5), at(2.0)));  // one full-strength line
  CHECK(bin(mt, at(0.5), at(2.0), hi) < 0.03 * bin(mt, at(0.5), at(2.0), f0));
  CHECK(bin(mw, at(0.5), at(2.0), hi) > 0.1 * rms(mw, at(0.5), at(2.0)));  // outer pair at +100 ct
  CHECK(bin(mw, at(0.5), at(2.0), lo) > 0.1 * rms(mw, at(0.5), at(2.0)));  // ... and -100 ct (symmetric)
  CHECK(bin(mw, at(0.5), at(2.0), f0) < 0.5 * rms(mw, at(0.5), at(2.0)));  // the energy left the unison
  // Loudness is the same from unison to extreme detune (no jump as the saws stop being coherent).
  // In between, slow beating legitimately swings the level, so only the ends are held tight.
  const double rt = rms(mt, at(0.5), at(2.0)), rw = rms(mw, at(0.5), at(2.0));
  CHECK(rt > rw * 0.7 && rt < rw * 1.4);
  for (float ct : {1.0f, 4.0f, 10.0f, 30.0f}) {
    const double r = rms(mono(render({{P::Detune, ct}, {P::Density, 1.0f}, {P::Cutoff, 16000.0f}}, 3.0)), at(0.5), at(3.0));
    CHECK(r > rw * 0.25 && r < rw * 1.5);   // beating dips, never a jump past the wide stack
  }
  CHECK_NEAR(bin(mw, at(0.5), at(2.0), hi) / bin(mw, at(0.5), at(2.0), lo), 1.0, 0.35);
  CHECK(finite(wide.l) && finite(wide.r));
  writeWav("detune_100ct", wide);
}

void testSpreadAndStereoBehaviour() {
  // spread 0: every oscillator on the centre: left and right are identical.
  const Stereo mid = render({{P::Spread, 0.0f}});
  CHECK(mid.l == mid.r);
  // spread 1: an actual stereo image.
  const Stereo wide = render({{P::Spread, 1.0f}});
  CHECK(sideOverMid(wide, at(0.5), at(1.5)) > 0.15);
  // spread is monotonic.
  const Stereo half = render({{P::Spread, 0.5f}});
  CHECK(sideOverMid(half, at(0.5), at(1.5)) < sideOverMid(wide, at(0.5), at(1.5)));
  CHECK(sideOverMid(half, at(0.5), at(1.5)) > 0.02);
  // Width follows detune (the centre oscillator is never panned).
  const Stereo small = render({{P::Spread, 1.0f}, {P::Detune, 3.0f}});
  const Stereo large = render({{P::Spread, 1.0f}, {P::Detune, 60.0f}});
  CHECK(sideOverMid(small, at(0.5), at(1.5)) < sideOverMid(large, at(0.5), at(1.5)));
  // Mono compatibility: folding to mono costs well under 3 dB against the centred version.
  const double centred = rms(mono(mid), at(0.5), at(1.5)), folded = rms(mono(wide), at(0.5), at(1.5));
  CHECK(folded > centred * 0.7 && folded < centred * 1.2);
  // Balanced image: neither side is favoured.
  CHECK_NEAR(rms(wide.l, at(0.5), at(1.5)) / rms(wide.r, at(0.5), at(1.5)), 1.0, 0.25);
  writeWav("spread_1", wide);
}

void testDensityChangesCharacterNotLoudness() {
  const Stereo thin = render({{P::Density, 0.0f}}), full = render({{P::Density, 1.0f}});
  const double a = rms(mono(thin), at(0.5), at(1.5)), b = rms(mono(full), at(0.5), at(1.5));
  CHECK(a > 0.02 && b > 0.02);
  CHECK(b > a * 0.5 && b < a * 2.0);
  // A lone centre saw has a narrow line; the full stack spreads energy off the fundamental.
  const Buf mt = mono(thin), mf = mono(full);
  const double thinSharp = bin(mt, at(0.5), at(1.5), 110.0) / rms(mt, at(0.5), at(1.5));
  const double fullSharp = bin(mf, at(0.5), at(1.5), 110.0) / rms(mf, at(0.5), at(1.5));
  CHECK(thinSharp > fullSharp * 1.5);
}

void testCutoffBrightens() {
  const Stereo dark = render({{P::Cutoff, 300.0f}}), bright = render({{P::Cutoff, 12000.0f}});
  const Buf d = mono(dark), b = mono(bright);
  CHECK(rms(b, at(0.5), at(1.5)) > rms(d, at(0.5), at(1.5)));
  CHECK(band(b, at(0.5), at(1.5), 110.0 * 20) > 5.0 * band(d, at(0.5), at(1.5), 110.0 * 20));
}

void testParamValidation() {
  Rig r;
  r.set(P::Detune, 1e6f);       // clamped
  r.set(P::Spread, NAN);        // ignored
  r.set(P::Density, -4.0f);
  r.e.instrumentNoteOn(1, NAN);  // non-finite velocity -> silence, not NaN
  r.go(0.3);
  CHECK(finite(r.o.l) && finite(r.o.r));
  CHECK(r.e.instrumentBaseParam(1, id(P::Detune)) <= 100.0f);
  // Extremes of every control together stay finite and bounded.
  for (int mask = 0; mask < 32; mask++) {
    Rig x;
    const bool hi = mask & 1;
    x.set(P::Detune, (mask & 2) ? 100.0f : 0.0f);
    x.set(P::Spread, (mask & 4) ? 1.0f : 0.0f);
    x.set(P::Density, (mask & 8) ? 1.0f : 0.0f);
    x.set(P::Cutoff, (mask & 16) ? 16000.0f : 200.0f);
    x.set(P::Tune, hi ? 440.0f : 27.5f);
    x.set(P::Attack, 0.001f); x.set(P::Release, 0.05f);
    x.e.instrumentNoteOn(1, 4.0f, 0.0f, hi ? 48.0f : -48.0f);
    x.go(0.6);
    CHECK(finite(x.o.l) && finite(x.o.r));
    CHECK(peak(x.o.l) <= 1.0f && peak(x.o.r) <= 1.0f);
  }
}

void testScheduledNoteStartsOnItsFrame() {
  Rig r;
  r.set(P::Attack, 0.005f);
  const int64_t at0 = 10000 + 37;
  r.e.scheduleInstrumentNote(at0, 1, 1.0f);
  r.go(0.4);
  CHECK(peak(r.o.l, 0, static_cast<size_t>(at0)) == 0.0f);
  CHECK(peak(r.o.l, static_cast<size_t>(at0), at(0.4)) > 0.01f);
}

void testRoutesThroughBusAndPan() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kHs, 0);
  e.setInstrumentParam(1, id(P::Attack), 0.01f);
  e.instrumentNoteOn(1, 1.0f, -1.0f);  // hard left balance
  int64_t f = 0; Stereo o;
  run(e, f, 0.5, &o);
  CHECK(rms(o.l, at(0.2), at(0.5)) > 0.02);
  CHECK(rms(o.r, at(0.2), at(0.5)) < 1e-4);
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kHs, 1);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 1200; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
    if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, 9);
    if (i == 250) e.setInstrumentParam(1, id(P::Detune), 70.0f);
    if (i == 300) e.instrumentNoteOn(1, 0.7f, 0, 2);
    if (i == 500) e.instrumentNoteOff(1);
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testRegistrationAndDefaults();
  testNoteStartAndRelease();
  testDeterministic();
  testNoteOffReleasesAndSustainZeroEnds();
  testRetriggerIsContinuous();
  testMultipleSimultaneousNotes();
  testDetuneParameter();
  testSpreadAndStereoBehaviour();
  testDensityChangesCharacterNotLoudness();
  testCutoffBrightens();
  testParamValidation();
  testScheduledNoteStartsOnItsFrame();
  testRoutesThroughBusAndPan();
  testRenderDoesNotAllocate();
  std::printf("hypersaw_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
