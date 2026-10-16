#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "../src/dsp/rhythm.h"
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the Pulse instrument (instruments/pulse.h), its rhythm primitives (dsp/rhythm.h) and the
// engine transport it reads. Set PULSE_WAV_DIR=<dir> to dump cases as 16-bit WAVs for listening.
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
constexpr int32_t kPulse = static_cast<int32_t>(InstrumentKind::Pulse);

using Buf = std::vector<float>;
int32_t id(PulseParam p) { return static_cast<int32_t>(p); }

struct Setup {
  float tune = 440, cutoff = 6000, res = 0, pump = 0, repeat = 0, rate = 2, diffuse = 0,
        attack = 0.001f, release = 0.08f;
  float velocity = 0.5f, pitch = 0.0f;
  double duration = -1;       // seconds; < 0 = held
  double bpm = -1;            // > 0: set the transport at frame 0
  double changeAtFrame = -1;  // > 0: a second transport change (bpm2, anchored at that frame)
  double bpm2 = 90;
  int64_t noteFrame = 0;      // 0 = immediate note-on at frame 0, else scheduled
};

void apply(Engine& e, const Setup& s) {
  auto set = [&](PulseParam p, float v) { e.setInstrumentParam(1, id(p), v); };
  set(PulseParam::Tune, s.tune); set(PulseParam::Cutoff, s.cutoff);
  set(PulseParam::Resonance, s.res); set(PulseParam::Pump, s.pump);
  set(PulseParam::Repeat, s.repeat); set(PulseParam::Rate, s.rate);
  set(PulseParam::Diffuse, s.diffuse); set(PulseParam::Attack, s.attack);
  set(PulseParam::Release, s.release);
}

void run(Engine& e, int64_t& frame, int32_t frames, Buf* l) {
  for (int32_t d = 0; d + kBlock <= frames; d += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    l->insert(l->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
}

void writeWav(const char* name, const Buf& b) {
  const char* dir = std::getenv("PULSE_WAV_DIR");
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

Buf play(const Setup& s, int32_t frames, const char* wav = nullptr) {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kPulse, 0);
  apply(e, s);
  if (s.bpm > 0) e.scheduleTransport(0, 0.0, s.bpm);
  if (s.changeAtFrame > 0) {
    e.scheduleTransport(static_cast<int64_t>(s.changeAtFrame), s.changeAtFrame, s.bpm2);
  }
  const int32_t dur = s.duration < 0 ? -1 : static_cast<int32_t>(std::lround(s.duration * kRate));
  if (s.noteFrame <= 0) {
    e.instrumentNoteOn(1, s.velocity, 0.0f, s.pitch, dur);
  } else {
    e.scheduleInstrumentNote(s.noteFrame, 1, s.velocity, 0.0f, s.pitch, dur);
  }
  int64_t f = 0;
  Buf out;
  run(e, f, frames, &out);
  if (wav) writeWav(wav, out);
  return out;
}

double rms(const Buf& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; i++) s += double(v[i]) * v[i];
  return std::sqrt(s / double(b - a));
}
// Brightness: rms of the first difference over rms — rises as the filter opens.
double brightness(const Buf& v, size_t a, size_t b) {
  double d = 0;
  for (size_t i = a + 1; i < b; i++) d += double(v[i] - v[i - 1]) * (v[i] - v[i - 1]);
  const double r = rms(v, a, b);
  return r > 0 ? std::sqrt(d / double(b - a - 1)) / r : 0;
}

// Mean pump gain the grid predicts over [a, a + w).
double expectedGain(size_t a, size_t w, double anchor, double period, float amount) {
  double s = 0;
  for (size_t i = a; i < a + w; i++) {
    s += rhythm::pumpGain(static_cast<float>(rhythm::phaseAt(double(i), anchor, period)), amount);
  }
  return s / double(w);
}
// Measured pump gain: the pumped render over the identical unpumped render (same oscillators,
// filter and envelope), so everything but the pump cancels.
double measuredGain(const Buf& pumped, const Buf& plain, size_t a, size_t w) {
  const double d = rms(plain, a, a + w);
  return d > 0 ? rms(pumped, a, a + w) / d : 0;
}

void testRhythmPrimitives() {
  CHECK(rhythm::rateStep(0.0f) == 0);
  CHECK(rhythm::rateStep(4.6f) == 5);
  CHECK(rhythm::rateStep(99.0f) == 5);
  CHECK(rhythm::rateStep(-3.0f) == 0);
  CHECK_NEAR(rhythm::periodFrames(24000.0, 0), 96000.0, 1e-9);   // 1/1 = 4 beats
  CHECK_NEAR(rhythm::periodFrames(24000.0, 2), 24000.0, 1e-9);   // 1/4 = 1 beat
  CHECK_NEAR(rhythm::periodFrames(24000.0, 4), 6000.0, 1e-9);    // 1/16
  CHECK_NEAR(rhythm::phaseAt(24000.0, 0.0, 24000.0), 0.0, 1e-12);
  CHECK_NEAR(rhythm::phaseAt(30000.0, 0.0, 24000.0), 0.25, 1e-12);
  CHECK_NEAR(rhythm::phaseAt(-6000.0, 0.0, 24000.0), 0.75, 1e-12);  // grid extends backwards

  // Pump: amount 0 is exactly 1 everywhere; depth and recovery follow amount.
  for (int i = 0; i <= 100; i++) CHECK(rhythm::pumpGain(i / 100.0f, 0.0f) == 1.0f);
  CHECK_NEAR(rhythm::pumpGain(0.0f, 1.0f), 1.0 - rhythm::kPumpMaxDepth, 1e-6);
  CHECK_NEAR(rhythm::pumpGain(0.0f, 0.5f), 1.0 - 0.5 * rhythm::kPumpMaxDepth, 1e-6);
  CHECK(rhythm::pumpGain(rhythm::pumpRecovery(1.0f) + 0.01f, 1.0f) == 1.0f);
  float prev = 0.0f;
  bool monotone = true, bounded = true;
  for (int i = 0; i <= 1000; i++) {
    const float g = rhythm::pumpGain(i / 1000.0f, 0.8f);
    if (g < prev - 1e-6f) monotone = false;
    if (g < 0.0f || g > 1.0f) bounded = false;
    prev = g;
  }
  CHECK(monotone);
  CHECK(bounded);
  CHECK(rhythm::pumpRecovery(1.0f) > rhythm::pumpRecovery(0.0f));

  // Repeat: zero at both ends of the cycle (wraps without a step), peak 1 near kRepeatPeak.
  CHECK_NEAR(rhythm::repeatContour(0.0f), 0.0, 1e-6);
  CHECK_NEAR(rhythm::repeatContour(1.0f), 0.0, 1e-4);
  CHECK_NEAR(rhythm::repeatContour(rhythm::kRepeatPeak), 1.0, 1e-4);
  float best = 0.0f;
  bool inRange = true;
  for (int i = 0; i <= 1000; i++) {
    const float c = rhythm::repeatContour(i / 1000.0f);
    if (c < 0.0f || c > 1.0f) inRange = false;
    best = std::fmax(best, c);
  }
  CHECK(inRange);
  CHECK_NEAR(best, 1.0, 1e-3);
}

void testTransport() {
  Engine e;
  e.init(kRate, 2, 4);
  CHECK_NEAR(e.transportFramesPerBeat(), 24000.0, 1e-9);  // 120 BPM default
  e.scheduleTransport(0, 1000.0, 60.0);
  e.process(0, kBlock);
  CHECK_NEAR(e.transportFramesPerBeat(), 48000.0, 1e-9);
  CHECK_NEAR(e.transportAnchorFrame(), 1000.0, 1e-9);
  e.scheduleTransport(100000, 100000.0, 240.0);  // future: not yet
  e.process(kBlock, kBlock);
  CHECK_NEAR(e.transportFramesPerBeat(), 48000.0, 1e-9);
  e.process(100000, kBlock);
  CHECK_NEAR(e.transportFramesPerBeat(), 12000.0, 1e-9);
  e.scheduleTransport(0, 0.0, std::nan(""));  // malformed: ignored
  e.process(100128, kBlock);
  CHECK_NEAR(e.transportFramesPerBeat(), 12000.0, 1e-9);
}

// The pumped render over the unpumped one tracks the grid's predicted gain at every rate.
void testPumpFollowsRate() {
  for (int step = 0; step < rhythm::kRateSteps; step++) {
    Setup s;
    s.rate = static_cast<float>(step);
    s.pump = 1.0f;
    const double period = rhythm::periodFrames(24000.0, step);
    const int32_t frames = static_cast<int32_t>(2.0 * period) + 2 * kBlock;
    Setup plain = s;
    plain.pump = 0.0f;
    const Buf a = play(s, frames, step == 2 ? "pump_quarter" : nullptr);
    const Buf b = play(plain, frames);
    const size_t w = static_cast<size_t>(std::fmax(period / 20.0, 64.0));
    for (double ph : {0.1, 0.35, 0.6, 0.85}) {
      const size_t at = static_cast<size_t>(period + ph * period);
      const double want = expectedGain(at, w, 0.0, period, 1.0f);
      const double got = measuredGain(a, b, at, w);
      CHECK_NEAR(got, want, 0.08);
    }
    // The duck is real: right after the hit is far quieter than the recovered level.
    const size_t hit = static_cast<size_t>(period + 0.1 * period);
    const size_t rec = static_cast<size_t>(period + 0.85 * period);
    CHECK(measuredGain(a, b, hit, w) < 0.6 * measuredGain(a, b, rec, w));
    CHECK_NEAR(measuredGain(a, b, rec, w), 1.0, 0.05);  // returns to the unmodulated level
  }
}

void testPumpDepthScalesWithAmount() {
  Setup s;
  s.rate = 2;
  const int32_t frames = 60000;
  Setup plain = s;
  const Buf b = play(plain, frames);
  double prev = 2.0;
  for (float amount : {0.25f, 0.5f, 0.75f, 1.0f}) {
    s.pump = amount;
    const Buf a = play(s, frames);
    const double g = measuredGain(a, b, 24000 + 1200, 480);
    CHECK(g < prev);
    prev = g;
  }
}

void testZeroModulationIsTempoIndependent() {
  Setup s;  // pump 0, repeat 0
  s.bpm = 120;
  const Buf a = play(s, 48000);
  s.bpm = 73;
  const Buf b = play(s, 48000);
  s.rate = 5;
  const Buf c = play(s, 48000);
  CHECK(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
  CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0);
}

void testRepeatOpensFilterOnTheGrid() {
  Setup s;
  s.tune = 110; s.cutoff = 300; s.rate = 2;
  s.repeat = 1.0f;
  const double period = 24000.0;
  const Buf on = play(s, 60000, "repeat_quarter");
  Setup off = s;
  off.repeat = 0.0f;
  const Buf flat = play(off, 60000);
  const size_t w = 2400;
  const size_t peakAt = static_cast<size_t>(period + rhythm::kRepeatPeak * period) - w / 2;
  const size_t restAt = static_cast<size_t>(period + 0.9 * period) - w / 2;
  CHECK(brightness(on, peakAt, peakAt + w) > 1.4 * brightness(on, restAt, restAt + w));
  // Without repeat the same windows are equally bright (within noise).
  const double r = brightness(flat, peakAt, peakAt + w) / brightness(flat, restAt, restAt + w);
  CHECK_NEAR(r, 1.0, 0.1);
  // Resting windows of the modulated render match the unmodulated brightness: it returns to base.
  CHECK_NEAR(brightness(on, restAt, restAt + w) / brightness(flat, restAt, restAt + w), 1.0, 0.1);
}

void testTempoChangeMidNote() {
  Setup s;
  s.rate = 2; s.pump = 1.0f;
  s.bpm = 120;
  s.changeAtFrame = 48000;  // 90 BPM from here, anchored at the change
  s.bpm2 = 90;
  const int32_t frames = 48000 + 2 * 32000 + 2 * kBlock;
  Setup plain = s;
  plain.pump = 0.0f;
  const Buf a = play(s, frames, "pump_tempo_change");
  const Buf b = play(plain, frames);
  const double period = 32000.0;  // 1/4 at 90 BPM
  const size_t w = 1600;
  for (double ph : {0.1, 0.4, 0.7}) {
    const size_t at = static_cast<size_t>(48000.0 + period + ph * period);
    CHECK_NEAR(measuredGain(a, b, at, w), expectedGain(at, w, 48000.0, period, 1.0f), 0.08);
  }
}

// The pump belongs to the grid, not the note: a note that starts off the grid, or is
// retriggered mid-cycle, ducks at the same frames as one started on the beat.
void testGridNotNote() {
  Setup onBeat;
  onBeat.rate = 2; onBeat.pump = 1.0f;
  Setup offBeat = onBeat;
  offBeat.noteFrame = 13000;  // mid-cycle
  Setup plain = onBeat;
  plain.pump = 0.0f;
  Setup plainOff = offBeat;
  plainOff.pump = 0.0f;
  const int32_t frames = 80000;
  const Buf a = play(onBeat, frames), pa = play(plain, frames);
  const Buf b = play(offBeat, frames), pb = play(plainOff, frames);
  const size_t w = 1200;
  for (size_t at : {size_t(24000 + 2400), size_t(48000 + 9600), size_t(72000 + 1200)}) {
    const double want = expectedGain(at, w, 0.0, 24000.0, 1.0f);
    CHECK_NEAR(measuredGain(a, pa, at, w), want, 0.08);
    CHECK_NEAR(measuredGain(b, pb, at, w), want, 0.08);
  }

  // Retrigger at a non-beat frame: pumping continues on the grid.
  Engine e;
  e.init(kRate, 2, 4);
  e.createInstrument(1, kPulse, 0);
  apply(e, onBeat);
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1);
  int64_t f = 0;
  Buf re;
  run(e, f, 30080, &re);
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1);
  run(e, f, 50000 - 30080, &re);
  const size_t at = 48000 + 1200;  // just after the beat at 48000
  CHECK(rms(re, at, at + w) < 0.6 * rms(re, 42000, 42000 + w));
}

void testNoteOffAndDuration() {
  Setup s;
  s.duration = 0.25;
  s.release = 0.05f;
  s.pump = 0.5f; s.repeat = 0.5f;
  Engine e;
  e.init(kRate, 2, 4);
  e.createInstrument(1, kPulse, 0);
  apply(e, s);
  e.instrumentNoteOn(1, 0.8f, 0.0f, 0.0f, static_cast<int32_t>(0.25 * kRate));
  int64_t f = 0;
  Buf out;
  run(e, f, static_cast<int32_t>(0.15 * kRate), &out);
  CHECK(e.activeInstrumentCount() == 1);
  CHECK(rms(out, out.size() - 2000, out.size()) > 0.01);
  run(e, f, static_cast<int32_t>(0.4 * kRate), &out);
  CHECK(e.activeInstrumentCount() == 0);  // released and ended itself
  const size_t tail = out.size() - 1000;
  CHECK(rms(out, tail, out.size()) < 1e-6);
}

void testStability() {
  for (int step = 0; step < rhythm::kRateSteps; step++) {
    Setup s;
    s.tune = 55; s.cutoff = 6000; s.res = 1.0f; s.pump = 1.0f; s.repeat = 1.0f; s.diffuse = 1.0f;
    s.rate = static_cast<float>(step);
    s.velocity = 1.2f;
    const Buf o = play(s, 3 * 48000, step == 4 ? "stress_sixteenth" : nullptr);
    bool finite = true;
    for (float v : o) if (!std::isfinite(v)) finite = false;
    CHECK(finite);
    float pk = 0;
    for (float v : o) pk = std::fmax(pk, std::fabs(v));
    CHECK(pk <= 1.0001f);
    double mean = 0;
    for (size_t i = 48000; i < o.size(); i++) mean += o[i];
    mean /= double(o.size() - 48000);
    CHECK(std::fabs(mean) < 0.02);
    CHECK(rms(o, 48000, o.size()) > 0.005);  // alive, not collapsed
  }
}

void testDeterminismAndNoAllocation() {
  Setup s;
  s.pump = 0.7f; s.repeat = 0.6f; s.diffuse = 0.5f; s.res = 0.5f;
  const Buf a = play(s, 24000);
  const Buf b = play(s, 24000);
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);

  Engine e;
  e.init(kRate, 2, 4);
  e.createInstrument(1, kPulse, 0);
  apply(e, s);
  e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, -1);
  int64_t f = 0;
  Buf scratch;
  scratch.reserve(200000);
  const long before = g_allocs;
  run(e, f, 48000, &scratch);
  CHECK(g_allocs == before);
}

// Same patch as the stress fixture, but a playable cutoff and a sweep of resonance, for choosing
// a default by ear.
void renderResonanceFixtures() {
  if (!std::getenv("PULSE_WAV_DIR")) return;
  for (float res : {0.0f, 0.15f, 0.3f, 0.5f}) {
    Setup s;
    s.tune = 55; s.cutoff = 1200; s.res = res; s.pump = 1.0f; s.repeat = 1.0f; s.diffuse = 1.0f;
    s.rate = 4.0f; s.velocity = 1.0f;
    char name[32];
    std::snprintf(name, sizeof name, "taste_res_%03d", static_cast<int>(res * 100));
    play(s, 3 * 48000, name);
  }
}

// A short sequenced bassline for listening: sixteenth notes with accents, pump + repeat on.
void renderGrooveFixture() {
  if (!std::getenv("PULSE_WAV_DIR")) return;
  Engine e;
  e.init(kRate, 2, 4);
  e.createInstrument(1, kPulse, 0);
  e.setInstrumentParam(1, id(PulseParam::Tune), 55.0f);
  e.setInstrumentParam(1, id(PulseParam::Cutoff), 350.0f);
  e.setInstrumentParam(1, id(PulseParam::Resonance), 0.4f);
  e.setInstrumentParam(1, id(PulseParam::Pump), 0.7f);
  e.setInstrumentParam(1, id(PulseParam::Repeat), 0.6f);
  e.setInstrumentParam(1, id(PulseParam::Rate), 3.0f);
  e.scheduleTransport(0, 0.0, 124.0);
  const double step = 60.0 / 124.0 * kRate / 4.0;  // sixteenth
  const float pitches[] = {0, 0, 12, 0, 0, 7, 0, 10};
  for (int i = 0; i < 32; i++) {
    const float vel = (i % 4 == 0) ? 1.0f : 0.6f;
    e.scheduleInstrumentNote(static_cast<int64_t>(i * step), 1, vel, 0.0f, pitches[i % 8],
                             static_cast<int32_t>(step * 0.9));
  }
  int64_t f = 0;
  Buf out;
  run(e, f, static_cast<int32_t>(step * 32) + 24000, &out);
  writeWav("groove_124bpm", out);
}

}  // namespace

int main() {
  testRhythmPrimitives();
  testTransport();
  testPumpFollowsRate();
  testPumpDepthScalesWithAmount();
  testZeroModulationIsTempoIndependent();
  testRepeatOpensFilterOnTheGrid();
  testTempoChangeMidNote();
  testGridNotNote();
  testNoteOffAndDuration();
  testStability();
  testDeterminismAndNoAllocation();
  renderGrooveFixture();
  renderResonanceFixtures();
  std::printf("pulse_test: %d checks, %d failures\n", webdsp::test::g_count,
              webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
