#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the synthesized Pad (instruments/pad.h) through the Engine's instrument API: lifecycle,
// attack/release shape, overlapping notes, slow movement, space, bounds and polyphony.
// Set PAD_WAV_DIR=<dir> to dump the stereo cases as 16-bit WAVs for listening (docs/pad-instrument.md).
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
constexpr int32_t kPad = static_cast<int32_t>(InstrumentKind::Pad);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
struct Stereo { Buf l, r; };
int32_t id(PadParam p) { return static_cast<int32_t>(p); }
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
  const char* dir = std::getenv("PAD_WAV_DIR");
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
  explicit Rig(int32_t bus = 0) { e.init(kRate, 2, 8); e.createInstrument(1, kPad, bus); }
  void set(PadParam p, float v, int32_t inst = 1) { e.setInstrumentParam(inst, id(p), v); }
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
double centroid(const Buf& v, size_t a, size_t b, double f0) {
  double num = 0, den = 0;
  for (int h = 1; h <= 40; h++) {
    const double m = band(v, a, b, f0 * h);
    num += f0 * h * m;
    den += m;
  }
  return den > 0 ? num / den : 0;
}
double windowedRmsSpread(const Buf& v, size_t a, size_t b, size_t win) {
  double lo = 1e9, hi = 0;
  for (size_t i = a; i + win <= b; i += win) {
    const double r = rms(v, i, i + win);
    lo = std::fmin(lo, r);
    hi = std::fmax(hi, r);
  }
  return hi / std::fmax(lo, 1e-9);
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

void testBasicNoteLifecycle() {
  Rig r;
  r.set(PadParam::Attack, 0.3f); r.set(PadParam::Release, 1.0f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(2.0 * kRate));
  CHECK(r.e.activeInstrumentCount() == 1);
  r.go(2.0);
  const double held = rms(r.o.l, at(1.0), at(2.0));
  CHECK(held > 0.03);
  CHECK(finite(r.o.l) && finite(r.o.r));
  CHECK(peak(r.o.l) <= 1.0f && peak(r.o.r) <= 1.0f);
  r.go(2.0);
  CHECK(rms(r.o.l, at(3.5), at(4.0)) < 1e-4);
  CHECK(r.e.activeInstrumentCount() == 0);
  writeWav("basic", r.o);
}

void testDeterministic() {
  Rig a, b;
  for (Rig* r : {&a, &b}) {
    r->e.instrumentNoteOn(1, 0.9f, 0.0f, 3.0f, static_cast<int32_t>(1.0 * kRate));
    r->go(2.0);
  }
  CHECK(a.o.l == b.o.l && a.o.r == b.o.r);
}

void testSmoothAttack() {
  Rig r;
  r.set(PadParam::Attack, 1.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(1.5);
  // No spike at the start: the first 5 ms are nearly silent, the swell is monotone in level.
  CHECK(peak(r.o.l, 0, at(0.005)) < 0.005f);
  double prev = 0;
  bool monotone = true;
  for (double t = 0; t < 0.9; t += 0.1) {
    const double v = rms(r.o.l, at(t), at(t + 0.1));
    if (v < prev * 0.98) monotone = false;
    prev = v;
  }
  CHECK(monotone);
  CHECK(rms(r.o.l, at(0.1), at(0.2)) < 0.15 * rms(r.o.l, at(1.2), at(1.5)));
  // Sample-to-sample step stays small through the whole swell (no click).
  float worst = 0;
  for (size_t i = 1; i < r.o.l.size(); i++) worst = std::fmax(worst, std::fabs(r.o.l[i] - r.o.l[i - 1]));
  CHECK(worst < 0.15f);
}

void testLongReleaseRunsItsCourse() {
  Rig r;
  r.set(PadParam::Attack, 0.1f); r.set(PadParam::Release, 6.0f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(1.0 * kRate));
  r.go(1.0);
  // Detuned saws beat against each other, so compare spans of seconds rather than instants.
  const double held = rms(mono(r.o), at(0.3), at(1.0));
  r.go(2.0);  // t = 1..3 s
  const double early = rms(mono(r.o), at(1.0), at(3.0));
  CHECK(early > 0.3 * held);            // a released note keeps sounding for seconds
  CHECK(r.e.activeInstrumentCount() == 1);
  r.go(3.0);  // t = 6 s, 1 s before the end
  const double late = rms(mono(r.o), at(4.5), at(6.0));
  CHECK(late < early);                  // and is falling
  CHECK(rms(mono(r.o), at(5.5), at(6.0)) > 1e-4);  // tail still alive near its end
  r.go(1.5);
  CHECK(rms(r.o.l, at(7.3), at(7.5)) < 1e-4);
  CHECK(r.e.activeInstrumentCount() == 0);  // reclaimable
  // No step anywhere in the tail.
  float worst = 0;
  for (size_t i = at(1.0); i < r.o.l.size(); i++) worst = std::fmax(worst, std::fabs(r.o.l[i] - r.o.l[i - 1]));
  CHECK(worst < 0.1f);
}

void testNoteOnHeldNoteOverlapsInsteadOfCutting() {
  Rig r;
  r.set(PadParam::Attack, 0.2f); r.set(PadParam::Release, 4.0f);
  r.set(PadParam::Detune, 0.0f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);       // 110 Hz
  r.go(1.5);
  const size_t seam = r.o.l.size();
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 7.0f);       // 164.8 Hz, no note-off in between
  r.go(1.5);
  const double f2 = 110.0 * std::pow(2.0, 7.0 / 12.0);
  // Right after the second note's swell the first note is still there, fading under it.
  const double oldNote = band(r.o.l, seam + at(0.8), seam + at(1.5), 110.0);
  const double newNote = band(r.o.l, seam + at(0.8), seam + at(1.5), f2);
  CHECK(oldNote > 0.02);
  CHECK(newNote > 0.02);
  // The old note's level keeps falling while the new one is still rising (a crossfade).
  const double oldEarly = band(r.o.l, seam + at(0.1), seam + at(0.6), 110.0);
  CHECK(oldNote < oldEarly);
  // The seam itself is continuous.
  float worst = 0;
  for (size_t i = seam - 2; i < seam + 2000; i++) worst = std::fmax(worst, std::fabs(r.o.l[i + 1] - r.o.l[i]));
  CHECK(worst < 0.15f);
  writeWav("overlap", r.o);
}

void testManyRapidNotesStayStableAndReclaim() {
  Rig r;
  r.set(PadParam::Attack, 0.05f); r.set(PadParam::Release, 6.0f);
  for (int n = 0; n < 12; n++) {  // more notes than slots: the quietest tail is taken over
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, float((n * 5) % 12));
    r.go(0.2);
  }
  CHECK(finite(r.o.l) && peak(r.o.l) <= 1.0f && peak(r.o.r) <= 1.0f);
  float worst = 0;
  for (size_t i = 1; i < r.o.l.size(); i++) worst = std::fmax(worst, std::fabs(r.o.l[i] - r.o.l[i - 1]));
  CHECK(worst < 0.25f);
  r.e.instrumentNoteOff(1);
  r.go(7.0);
  CHECK(rms(r.o.l, r.o.l.size() - at(0.2), r.o.l.size()) < 1e-4);
  CHECK(r.e.activeInstrumentCount() == 0);
}

void testParamValidation() {
  Rig r;
  r.set(PadParam::Attack, 99.0f);
  r.set(PadParam::Tone, -4.0f);
  r.set(PadParam::Tone, NAN);       // ignored
  r.set(PadParam::Space, 5.0f);
  r.e.instrumentNoteOn(1, NAN);     // non-finite velocity -> silence, not NaN
  r.go(0.3);
  CHECK(finite(r.o.l) && finite(r.o.r));
  // Extremes of every control together stay finite and bounded.
  for (int mask = 0; mask < 16; mask++) {
    Rig x;
    const bool hi = mask & 1;
    x.set(PadParam::Tone, hi ? 1.0f : 0.0f);
    x.set(PadParam::Movement, (mask & 2) ? 1.0f : 0.0f);
    x.set(PadParam::Detune, (mask & 4) ? 1.0f : 0.0f);
    x.set(PadParam::Space, (mask & 8) ? 1.0f : 0.0f);
    x.set(PadParam::Tune, hi ? 440.0f : 27.5f);
    x.set(PadParam::Attack, 0.02f); x.set(PadParam::Release, 0.1f);
    x.e.instrumentNoteOn(1, 4.0f, 0.0f, hi ? 48.0f : -48.0f);
    x.go(1.0);
    CHECK(finite(x.o.l) && finite(x.o.r));
    CHECK(peak(x.o.l) <= 1.0f && peak(x.o.r) <= 1.0f);
  }
}

void testToneBrightens() {
  auto c = [](float tone) {
    Rig r;
    r.set(PadParam::Tone, tone); r.set(PadParam::Attack, 0.05f); r.set(PadParam::Movement, 0.0f);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(2.0);
    return centroid(r.o.l, at(1.0), at(2.0), 110.0);
  };
  CHECK(c(0.9f) > 1.5 * c(0.1f));
}

void testMovementEvolvesTimbreOverTime() {
  auto spread = [](float movement) {
    Rig r;
    r.set(PadParam::Movement, movement); r.set(PadParam::Attack, 0.05f);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(14.0);
    return windowedRmsSpread(r.o.l, at(1.0), at(14.0), at(0.5));
  };
  const double still = spread(0.0f), moving = spread(1.0f);
  CHECK(moving > still);
  CHECK(moving > 1.15);
  // Movement 0 with no detune and the centre image is a static signal: nothing drifts at all.
  Rig r;
  r.set(PadParam::Movement, 0.0f); r.set(PadParam::Detune, 0.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(4.0);
  CHECK(windowedRmsSpread(r.o.l, at(1.5), at(4.0), at(0.5)) < 1.05);
}

void testMovementIsBoundedAndSlow() {
  Rig r;
  r.set(PadParam::Movement, 1.0f); r.set(PadParam::Attack, 0.05f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(10.0);
  // Level never jumps between neighbouring 100 ms windows (the ensemble beating is slow).
  const Buf m = mono(r.o);
  double worstRatio = 1.0;
  for (size_t i = at(1.0); i + at(0.2) < m.size(); i += at(0.1)) {
    const double a = rms(m, i, i + at(0.1)), b = rms(m, i + at(0.1), i + at(0.2));
    worstRatio = std::fmax(worstRatio, std::fmax(a / b, b / a));
  }
  CHECK(worstRatio < 2.0);
}

void testSpaceWidensAndDiffuses() {
  auto widthOf = [](float space) {
    Rig r;
    r.set(PadParam::Space, space); r.set(PadParam::Movement, 0.0f); r.set(PadParam::Attack, 0.05f);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(1.5);
    double diff = 0, sum = 0;
    for (size_t i = at(0.5); i < at(1.5); i++) {
      const double m = r.o.l[i] + r.o.r[i], s = r.o.l[i] - r.o.r[i];
      sum += m * m; diff += s * s;
    }
    return diff / std::fmax(sum, 1e-12);
  };
  CHECK(widthOf(0.0f) < 1e-9);      // space 0, no drift: exactly centred
  CHECK(widthOf(1.0f) > 0.05);      // clearly stereo
  CHECK(widthOf(1.0f) > widthOf(0.4f));
}

void testScheduledNoteStartsOnItsFrame() {
  Rig r;
  r.set(PadParam::Attack, 0.02f);
  const int64_t at0 = 10000 + 37;
  r.e.scheduleInstrumentNote(at0, 1, 1.0f);
  r.go(0.4);
  CHECK(peak(r.o.l, 0, static_cast<size_t>(at0)) == 0.0f);
  CHECK(peak(r.o.l, static_cast<size_t>(at0), at(0.4)) > 0.01f);
}

void testNoteOffBeforePendingStartCancels() {
  Rig r;
  r.e.scheduleInstrumentNote(5000, 1, 1.0f);
  r.go(0.05);
  CHECK(peak(r.o.l) == 0.0f);
}

void testPolyphonyOfChordInstances() {
  Engine e; e.init(kRate, 2, 8);
  const float chord[] = {0, 4, 7, 11, 14, 19};
  for (int i = 0; i < 6; i++) {
    e.createInstrument(10 + i, kPad, 0);
    e.setInstrumentParam(10 + i, id(PadParam::Attack), 0.3f);
    e.setInstrumentParam(10 + i, id(PadParam::Release), 3.0f);
  }
  int64_t f = 0; Stereo o;
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 6; i++) e.instrumentNoteOn(10 + i, 0.8f, (i - 2.5f) * 0.1f, chord[i] + round * 2);
    run(e, f, 1.0, &o);
  }
  CHECK(e.activeInstrumentCount() == 6);
  CHECK(finite(o.l) && finite(o.r));
  CHECK(peak(o.l) <= 6.0f && peak(o.r) <= 6.0f);  // each instance is bounded by 1; the bus sums them
  CHECK(rms(o.l, at(2.0), at(3.0)) > 0.05);
  for (int i = 0; i < 6; i++) e.instrumentNoteOff(10 + i);
  run(e, f, 4.0, &o);
  CHECK(e.activeInstrumentCount() == 0);
  writeWav("chord", o);
}

void testRoutesThroughBusAndPan() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kPad, 0);
  e.setInstrumentParam(1, id(PadParam::Attack), 0.05f);
  e.instrumentNoteOn(1, 1.0f, -1.0f);  // hard left balance
  int64_t f = 0; Stereo o;
  run(e, f, 1.0, &o);
  CHECK(rms(o.l, at(0.5), at(1.0)) > 0.02);
  CHECK(rms(o.r, at(0.5), at(1.0)) < 1e-4);
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kPad, 1);
  e.setInstrumentParam(1, id(PadParam::Movement), 1.0f);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 1200; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
    if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, 9);
    if (i == 250) e.instrumentNoteOn(1, 0.7f, 0, 12);
    if (i == 300) e.instrumentNoteOn(1, 0.7f, 0, 2);
    if (i == 500) e.instrumentNoteOff(1);
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testBasicNoteLifecycle();
  testDeterministic();
  testSmoothAttack();
  testLongReleaseRunsItsCourse();
  testNoteOnHeldNoteOverlapsInsteadOfCutting();
  testManyRapidNotesStayStableAndReclaim();
  testParamValidation();
  testToneBrightens();
  testMovementEvolvesTimbreOverTime();
  testMovementIsBoundedAndSlow();
  testSpaceWidensAndDiffuses();
  testScheduledNoteStartsOnItsFrame();
  testNoteOffBeforePendingStartCancels();
  testPolyphonyOfChordInstances();
  testRoutesThroughBusAndPan();
  testRenderDoesNotAllocate();
  std::printf("pad_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
