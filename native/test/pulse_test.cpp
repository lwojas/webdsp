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
  // -1 = follow `repeat` (the pre-filterMod coupling: repeat x 3 oct = 0.75 x repeat of 4 oct).
  float filterMod = -1;
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
  set(PulseParam::FilterMod, s.filterMod >= 0 ? s.filterMod : 0.75f * s.repeat);
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
  CHECK_NEAR(rhythm::pumpGain(0.0f, 1.0f), std::pow(10.0, -rhythm::kPumpMaxDb / 20.0), 1e-5);
  CHECK_NEAR(rhythm::pumpGain(0.0f, 0.5f), std::pow(10.0, -0.5 * rhythm::kPumpMaxDb / 20.0), 1e-5);
  CHECK(rhythm::pumpGain(0.0f, 0.5f) < 0.2f);  // half the knob is already a clear (-15 dB) duck
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

// The pump is a pure output multiplier: pumped[i] = plain[i] * g[i] with g in [0, 1] and g set by
// the grid alone. Sample-level ratio, valid wherever the plain signal is not near a zero crossing.
Buf pumpRatio(const Buf& pumped, const Buf& plain, float floorLevel = 0.02f) {
  Buf r(plain.size(), -1.0f);  // -1 = undefined
  for (size_t i = 0; i < plain.size(); i++) {
    if (std::fabs(plain[i]) > floorLevel) r[i] = pumped[i] / plain[i];
  }
  return r;
}

void testPumpIsPostEnvelopeMultiplier() {
  const int32_t frames = 96000;  // 2 s; 0.75 s note at 120 bpm, rate step 2 = quarter notes
  Setup base;
  base.velocity = 1.0f;  // loud enough that the soft clipper is working
  base.duration = 1.5;
  base.release = 0.3f;
  Setup plain = base;
  Setup pumped = base;
  pumped.pump = 1.0f;
  const Buf p0 = play(plain, frames), p1 = play(pumped, frames);
  const Buf g = pumpRatio(p1, p0);

  // Never louder than the unpumped signal, and exactly the same sign. Gain stays within 0..1.
  size_t defined = 0;
  float gMin = 2.0f, gMax = -1.0f;
  for (size_t i = 0; i < p0.size(); i++) {
    CHECK(std::fabs(p1[i]) <= std::fabs(p0[i]) + 1e-6f);
    if (g[i] < 0.0f) continue;
    defined++;
    CHECK(g[i] <= 1.0f + 1e-5f);
    gMin = std::min(gMin, g[i]);
    gMax = std::max(gMax, g[i]);
  }
  CHECK(defined > 20000);
  CHECK(gMin < 0.1f);   // deep duck
  CHECK(gMax > 0.99f);  // fully recovered, never boosted

  // Sustained: the unpumped envelope is flat while the pump moves (note held 1.5 s of the 2 s).
  const size_t w = 2400;
  const double lvlA = rms(p0, 12000, 12000 + w), lvlB = rms(p0, 36000, 36000 + w);
  CHECK_NEAR(lvlB / lvlA, 1.0, 0.1);
  const double pa = rms(p1, 24000 + 1200, 24000 + 1200 + w);   // just after a beat: ducked
  const double pb = rms(p1, 24000 + 20000, 24000 + 20000 + w);  // recovered
  CHECK(pa < 0.5 * pb);

  // Pump ratio is independent of the envelope: attack and release change the signal, not the
  // gain the pump applies at any sample both renders define.
  for (float attack : {0.001f, 0.1f}) {
    for (float release : {0.05f, 0.8f}) {
      Setup a = pumped, b = plain;
      a.attack = b.attack = attack;
      a.release = b.release = release;
      const Buf ga = pumpRatio(play(a, frames), play(b, frames));
      size_t common = 0;
      for (size_t i = 0; i < g.size(); i++) {
        if (g[i] < 0.0f || ga[i] < 0.0f) continue;
        common++;
        CHECK_NEAR(ga[i], g[i], 2e-3);
      }
      CHECK(common > 5000);
    }
  }

  // Release: after the note-off (1.5 s) the unpumped tail decays on the ADSR's release alone, and
  // the pump keeps modulating it. Pumped tail never exceeds the plain tail.
  const size_t off = 72000;
  CHECK(rms(p0, off + 2400, off + 4800) < rms(p0, off - 4800, off - 2400));
  Setup longRel = base;
  longRel.release = 1.0f;
  const Buf tail = play(longRel, frames);
  CHECK(rms(tail, off + 2400, off + 4800) > 1.3 * rms(p0, off + 2400, off + 4800));  // release time is the ADSR's
  Setup longRelPump = longRel;
  longRelPump.pump = 1.0f;
  const Buf gt = pumpRatio(play(longRelPump, frames), tail);
  float tMin = 2.0f, tMax = -1.0f;
  for (size_t i = off; i < tail.size(); i++) {
    if (gt[i] < 0.0f) continue;
    tMin = std::min(tMin, gt[i]);
    tMax = std::max(tMax, gt[i]);
  }
  CHECK(tMin < 0.3f && tMax > 0.9f && tMax <= 1.0f + 1e-5f);  // pump still moving during release
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

// Repeater: peak-window brightness over rest-window brightness for cycle `k` of a grid with the
// given anchor/period. > 1 = the filter opened on the grid. Brightness is level-independent, so
// a post-filter gain (the pump) does not change it.
double repeatOpening(const Buf& v, double anchor, double period, int k, size_t w = 2400) {
  const size_t peakAt = static_cast<size_t>(anchor + (k + rhythm::kRepeatPeak) * period) - w / 2;
  const size_t restAt = static_cast<size_t>(anchor + (k + 0.9) * period) - w / 2;
  return brightness(v, peakAt, peakAt + w) / brightness(v, restAt, restAt + w);
}

Setup repeatSetup() {
  Setup s;
  s.tune = 110; s.cutoff = 300; s.rate = 2; s.repeat = 1.0f;
  return s;
}

// Zero modulation is the base cutoff, exactly: with repeat = 0 the filter never moves, whatever
// the grid does (rate, tempo, even a running pump).
void testRepeatZeroDepthLeavesFilterAtBase() {
  Setup s = repeatSetup();
  s.repeat = 0.0f;
  const Buf a = play(s, 72000);
  s.rate = 4; s.bpm = 77;
  const Buf b = play(s, 72000);
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
  // With a pump running the filter is still static: brightness is flat across the cycle.
  s = repeatSetup();
  s.repeat = 0.0f; s.pump = 1.0f;
  const Buf p = play(s, 72000);
  for (int k = 1; k <= 2; k++) CHECK_NEAR(repeatOpening(p, 0.0, 24000.0, k), 1.0, 0.1);
  // Depth scales the opening monotonically; the maximum is plainly audible.
  double prev = 1.0;
  for (float r : {0.25f, 0.5f, 1.0f}) {
    s = repeatSetup();
    s.repeat = r;
    const double o = repeatOpening(play(s, 72000), 0.0, 24000.0, 1);
    CHECK(o > prev);
    prev = o;
  }
  CHECK(prev > 1.4);
}

// A sustained note keeps repeating on every grid cycle; a note-off does not stop it while the
// release tail rings, and it never touches the amplitude envelope's lifetime.
void testRepeatSustainedAndRelease() {
  Setup s = repeatSetup();
  const Buf held = play(s, 5 * 24000);
  for (int k = 1; k <= 3; k++) CHECK(repeatOpening(held, 0.0, 24000.0, k) > 1.4);

  // Note-off at 1.2 s with a long release: cycles 2 and 3 fall inside the tail and still open.
  Setup rel = s;
  rel.duration = 1.2; rel.release = 1.5f;
  const Buf tail = play(rel, 5 * 24000);
  CHECK(rms(tail, 24000 * 3, 24000 * 3 + 2400) > 0.002);
  for (int k = 2; k <= 3; k++) CHECK(repeatOpening(tail, 0.0, 24000.0, k) > 1.3);

  // ADSR independence: the voice lives exactly as long with repeat at 0, mid or full.
  size_t end0 = 0;
  for (float r : {0.0f, 0.5f, 1.0f}) {
    Setup t = s;
    t.repeat = r; t.duration = 0.4; t.release = 0.2f;
    const Buf o = play(t, 48000);
    size_t n = o.size();
    while (n > 0 && o[n - 1] == 0.0f) n--;
    if (r == 0.0f) end0 = n;
    CHECK(n > 19200 && n < 40000);
    CHECK(n == end0);
  }
}

// Repeater (filter) and Pumper (output) are separate: each runs with the other on or off, and
// neither changes what the other does.
void testRepeatAndPumpAreIndependent() {
  const int32_t frames = 5 * 24000;
  Setup s = repeatSetup();
  s.velocity = 1.0f;
  Setup rp = s; rp.pump = 1.0f;                    // both
  Setup r0 = s;                                    // repeat only
  Setup p0 = s; p0.repeat = 0.0f; p0.pump = 1.0f;  // pump only
  Setup n0 = s; n0.repeat = 0.0f;                  // neither
  const Buf both = play(rp, frames), repeatOnly = play(r0, frames);
  const Buf pumpOnly = play(p0, frames), neither = play(n0, frames);

  // Repeater still opens the filter with the pump running (the pump only scales the output).
  for (int k = 1; k <= 2; k++) {
    CHECK(repeatOpening(both, 0.0, 24000.0, k) > 1.4);
    CHECK_NEAR(repeatOpening(both, 0.0, 24000.0, k) / repeatOpening(repeatOnly, 0.0, 24000.0, k), 1.0, 0.1);
  }
  // The pump is the same output multiplier with the repeater on or off: the sample-wise ratio
  // pumped / unpumped is identical, so Pumper does not depend on filter movement, and (being
  // post-ADSR, post-filter) it cannot move the cutoff.
  const Buf gRepeat = pumpRatio(both, repeatOnly), gPlain = pumpRatio(pumpOnly, neither);
  size_t common = 0;
  float gMin = 2.0f;
  for (size_t i = 0; i < gRepeat.size(); i++) {
    if (gRepeat[i] < 0.0f || gPlain[i] < 0.0f) continue;
    common++;
    CHECK_NEAR(gRepeat[i], gPlain[i], 2e-3);
    CHECK(gRepeat[i] <= 1.0f + 1e-5f);
    gMin = std::min(gMin, gRepeat[i]);
  }
  CHECK(common > 20000);
  CHECK(gMin < 0.1f);  // the duck is its full depth with repeat running
  // Repeat does not stop the pump from being the last stage: never louder than without it.
  for (size_t i = 0; i < both.size(); i++) CHECK(std::fabs(both[i]) <= std::fabs(repeatOnly[i]) + 1e-6f);
}

// After a tempo change the repeat contour follows the new grid (anchored at the change).
void testRepeatFollowsTempoChange() {
  Setup s = repeatSetup();
  s.bpm = 120;
  s.changeAtFrame = 48000;
  s.bpm2 = 90;  // period 32000
  const Buf o = play(s, 48000 + 4 * 32000);
  CHECK(repeatOpening(o, 0.0, 24000.0, 0) > 1.4);      // old grid, before the change
  for (int k = 0; k <= 2; k++) CHECK(repeatOpening(o, 48000.0, 32000.0, k) > 1.4);
}

// filterMod is its own control: the filter opens on the grid whatever `repeat` is, and `repeat`
// alone never moves the cutoff.
void testFilterModIsIndependentOfRepeat() {
  Setup s = repeatSetup();
  // filterMod on, repeat 0: the filter still opens on the grid (no drive riding it).
  s.repeat = 0.0f; s.filterMod = 0.75f;
  CHECK(repeatOpening(play(s, 72000), 0.0, 24000.0, 1) > 1.4);
  // repeat on, filterMod 0: the cutoff is static. Drive rides the contour, so compare against
  // the plain filter-static case by brightness flatness within the cycle's drive wobble.
  Setup r = repeatSetup();
  r.repeat = 1.0f; r.filterMod = 0.0f;
  Setup full = repeatSetup();
  full.repeat = 1.0f; full.filterMod = 0.75f;
  CHECK(repeatOpening(play(r, 72000), 0.0, 24000.0, 1) < 0.6 * repeatOpening(play(full, 72000), 0.0, 24000.0, 1));
  // Both zero: bit-identical across tempo and rate (a plain ADSR synth).
  Setup z = repeatSetup();
  z.repeat = 0.0f; z.filterMod = 0.0f;
  const Buf a = play(z, 48000);
  z.bpm = 73; z.rate = 5;
  const Buf b = play(z, 48000);
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
  // filterMod does not touch the amplitude lifetime.
  Setup d0 = repeatSetup(); d0.filterMod = 0.0f; d0.duration = 0.4; d0.release = 0.2f;
  Setup d1 = d0; d1.filterMod = 1.0f;
  const Buf e0 = play(d0, 48000), e1 = play(d1, 48000);
  size_t n0 = e0.size(), n1 = e1.size();
  while (n0 > 0 && e0[n0 - 1] == 0.0f) n0--;
  while (n1 > 0 && e1[n1 - 1] == 0.0f) n1--;
  CHECK(n0 == n1 && n0 > 19200);
  // Max settings stay bounded with the full 4 octaves.
  Setup m = repeatSetup();
  m.res = 1.0f; m.filterMod = 1.0f; m.repeat = 1.0f; m.pump = 1.0f; m.rate = 5; m.velocity = 1.2f;
  const Buf o = play(m, 3 * 48000);
  float pk = 0;
  for (float v : o) { CHECK(std::isfinite(v)); pk = std::fmax(pk, std::fabs(v)); }
  CHECK(pk <= 1.0001f);
}

void testFilterModParamMetadata() {
  const InstrumentParamInfo* info = instrumentParamInfo(kPulse, id(PulseParam::FilterMod));
  CHECK(info != nullptr);
  if (info) {
    CHECK(std::string(info->name) == "filterMod");
    CHECK(info->min == 0.0f && info->max == 1.0f && info->def == 0.3f);
  }
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

// Two held notes back to back (the tracker's legato default): the second re-articulates, so
// `attack` is audible on sequenced lines, and it never clicks.
Buf legato(float attack) {
  Setup s;
  s.tune = 110; s.cutoff = 800; s.attack = attack;
  Engine e;
  e.init(kRate, 2, 4);
  e.createInstrument(1, kPulse, 0);
  apply(e, s);
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1);
  int64_t f = 0;
  Buf out;
  run(e, f, 24000, &out);  // first note settles to full level
  e.instrumentNoteOn(1, 0.5f, 0.0f, 0.0f, -1);
  run(e, f, 24000, &out);
  return out;
}

void testLegatoReArticulation() {
  const size_t at = 24000;
  const Buf slow = legato(0.1f);
  const Buf fast = legato(0.001f);
  const double before = rms(slow, at - 2400, at);
  // A slow attack fades the second note in: the first 20 ms are far below full level...
  CHECK(rms(slow, at, at + 960) < 0.35 * before);
  // ...and it arrives at full level after the attack.
  CHECK_NEAR(rms(slow, at + 7200, at + 9600) / before, 1.0, 0.1);
  // A 1 ms attack is a legato join: no audible dip.
  CHECK(rms(fast, at, at + 960) > 0.8 * before);

  // Click-free: the largest sample step across the join stays within the steady signal's own.
  for (const Buf* b : {&slow, &fast}) {
    float steady = 0.0f, join = 0.0f;
    for (size_t i = at - 4800; i < at - 2400; i++) steady = std::fmax(steady, std::fabs((*b)[i] - (*b)[i - 1]));
    for (size_t i = at - 8; i < at + 480; i++) join = std::fmax(join, std::fabs((*b)[i] - (*b)[i - 1]));
    CHECK(join <= 1.5f * steady);
  }
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

// A tracker-style line: every note held until the next (legato), for hearing attack and pump.
void renderLegatoFixtures() {
  if (!std::getenv("PULSE_WAV_DIR")) return;
  struct Case { const char* name; float pump; float attack; };
  for (const Case c : {Case{"legato_pump000_att003", 0.0f, 0.003f}, Case{"legato_pump050_att003", 0.5f, 0.003f},
                       Case{"legato_pump050_att080", 0.5f, 0.08f}, Case{"legato_pump100_att003", 1.0f, 0.003f}}) {
    Engine e;
    e.init(kRate, 2, 4);
    e.createInstrument(1, kPulse, 0);
    e.setInstrumentParam(1, id(PulseParam::Tune), 55.0f);
    e.setInstrumentParam(1, id(PulseParam::Cutoff), 700.0f);
    e.setInstrumentParam(1, id(PulseParam::Resonance), 0.15f);
    e.setInstrumentParam(1, id(PulseParam::Pump), c.pump);
    e.setInstrumentParam(1, id(PulseParam::Repeat), 0.5f);
    e.setInstrumentParam(1, id(PulseParam::Attack), c.attack);
    e.setInstrumentParam(1, id(PulseParam::Rate), 2.0f);
    e.scheduleTransport(0, 0.0, 124.0);
    const double step = 60.0 / 124.0 * kRate / 4.0;
    const float pitches[] = {0, 0, 0, 12, 0, 0, 7, 0};
    for (int i = 0; i < 32; i++) {
      // Notes of 2 steps each, back to back: duration = distance to the next note.
      if (i % 2) continue;
      e.scheduleInstrumentNote(static_cast<int64_t>(i * step), 1, 0.8f, 0.0f, pitches[(i / 2) % 8],
                               static_cast<int32_t>(step * 2));
    }
    int64_t f = 0;
    Buf out;
    run(e, f, static_cast<int32_t>(step * 32) + 24000, &out);
    writeWav(c.name, out);
  }
}

}  // namespace

int main() {
  testRhythmPrimitives();
  testTransport();
  testPumpFollowsRate();
  testPumpDepthScalesWithAmount();
  testPumpIsPostEnvelopeMultiplier();
  testZeroModulationIsTempoIndependent();
  testRepeatOpensFilterOnTheGrid();
  testRepeatZeroDepthLeavesFilterAtBase();
  testRepeatSustainedAndRelease();
  testRepeatAndPumpAreIndependent();
  testRepeatFollowsTempoChange();
  testFilterModIsIndependentOfRepeat();
  testFilterModParamMetadata();
  testTempoChangeMidNote();
  testGridNotNote();
  testLegatoReArticulation();
  testNoteOffAndDuration();
  testStability();
  testDeterminismAndNoAllocation();
  renderGrooveFixture();
  renderResonanceFixtures();
  renderLegatoFixtures();
  std::printf("pulse_test: %d checks, %d failures\n", webdsp::test::g_count,
              webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
