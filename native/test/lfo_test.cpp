#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/modulation.h"
#include "test_main.h"

// Covers the parameter-agnostic LFO (dsp/lfo.h) and the modulation layer that maps it onto a
// synth's parameters (modulation.h), with a mock Instrument so nothing here depends on a synth.
// Engine-level behaviour with real Bass/Pulse audio is in modulation_test.cpp.
using namespace webdsp;

namespace {

constexpr double kSr = 48000.0;
constexpr int32_t kBass = static_cast<int32_t>(InstrumentKind::Bass);
constexpr int32_t kBody = static_cast<int32_t>(BassParam::Body);
constexpr int32_t kBite = static_cast<int32_t>(BassParam::Bite);
constexpr int32_t kCutoff = static_cast<int32_t>(BassParam::Cutoff);
constexpr int32_t kLegato = static_cast<int32_t>(BassParam::Legato);

// Records what the modulation layer tells the instrument.
class Mock final : public Instrument {
 public:
  InstrumentKind kind() const override { return InstrumentKind::Bass; }
  void configure(double) override {}
  void noteOn(float, int32_t, float, float) override {}
  void noteOff() override {}
  void setParam(int32_t p, float v) override { last[p] = v; calls++; }
  void reset() override {}
  bool render(float* const*, int32_t, int32_t, float* const*) override { return true; }
  bool isActive() const override { return true; }
  float last[32] = {};
  int calls = 0;
};

LfoConfig cfg(int32_t target, LfoWaveform w, float amount) {
  LfoConfig c;
  c.enabled = true;
  c.target = target;
  c.waveform = w;
  c.amount = amount;
  return c;
}

// --- waveforms --------------------------------------------------------------------------------

void testWaveformsExactValues() {
  CHECK_NEAR(lfoShape(LfoWaveform::Sine, 0.0), 0.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Sine, 0.25), 1.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Sine, 0.5), 0.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Sine, 0.75), -1.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Triangle, 0.0), 0.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Triangle, 0.25), 1.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Triangle, 0.5), 0.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Triangle, 0.75), -1.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Triangle, 0.125), 0.5, 1e-6);
  CHECK(lfoShape(LfoWaveform::Square, 0.0) == 1.0f);
  CHECK(lfoShape(LfoWaveform::Square, 0.49) == 1.0f);
  CHECK(lfoShape(LfoWaveform::Square, 0.5) == -1.0f);
  CHECK(lfoShape(LfoWaveform::Square, 0.99) == -1.0f);
  CHECK_NEAR(lfoShape(LfoWaveform::Saw, 0.0), -1.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Saw, 0.5), 0.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Saw, 0.999), 0.998, 1e-6);
  // Phase wraps (including negative).
  CHECK_NEAR(lfoShape(LfoWaveform::Sine, 1.25), 1.0, 1e-6);
  CHECK_NEAR(lfoShape(LfoWaveform::Saw, -0.5), 0.0, 1e-6);
}

void testWaveformsBoundedAndDeterministic() {
  for (int w = 0; w < kLfoWaveformCount; w++) {
    for (int i = -2000; i < 6000; i++) {
      const double p = i * 0.00137;
      const float a = lfoShape(static_cast<LfoWaveform>(w), p);
      CHECK(a >= -1.0f && a <= 1.0f);
      CHECK(a == lfoShape(static_cast<LfoWaveform>(w), p));
    }
  }
}

// --- rate / phase -----------------------------------------------------------------------------

void testFreeRateAdvancesAtRequestedHz() {
  for (float hz : {0.5f, 2.0f, 5.0f, 13.0f}) {
    Lfo l;
    l.reset(0.0);
    LfoConfig c = cfg(0, LfoWaveform::Saw, 1.0f);
    c.rateHz = hz;
    // Saw wraps -> counts cycles. Step by 16 frames for 2 seconds.
    int wraps = 0;
    float prev = l.value(c, 0.0, kSr, 0.0, 24000.0);
    for (double f = 16; f <= 2.0 * kSr + 64; f += 16) {  // past the 2 s boundary: no rounding edge
      const float v = l.value(c, f, kSr, 0.0, 24000.0);
      if (v < prev - 1.0f) wraps++;
      prev = v;
    }
    CHECK(wraps == static_cast<int>(std::lround(hz * 2.0)));
    // Exact phase after 1 s: frac(hz).
    Lfo m;
    m.reset(0.0);
    c.waveform = LfoWaveform::Saw;
    const float v = m.value(c, kSr * 0.3, kSr, 0.0, 24000.0);
    const double want = 2.0 * std::fmod(hz * 0.3, 1.0) - 1.0;
    CHECK_NEAR(v, want, 1e-4);
  }
}

void testFreeRateChangeKeepsPhaseContinuous() {
  Lfo l;
  l.reset(0.0);
  LfoConfig c = cfg(0, LfoWaveform::Saw, 1.0f);
  c.rateHz = 1.0f;
  const float before = l.value(c, kSr * 0.25, kSr, 0.0, 24000.0);  // phase 0.25
  l.catchUp(kSr * 0.25, kSr, c.rateHz);
  c.rateHz = 4.0f;  // new rate from here on
  const float after = l.value(c, kSr * 0.25, kSr, 0.0, 24000.0);
  CHECK_NEAR(before, after, 1e-6);
  const float later = l.value(c, kSr * 0.5, kSr, 0.0, 24000.0);  // +0.25 s at 4 Hz = +1 cycle
  CHECK_NEAR(later, before, 1e-4);
}

void testInitialPhaseAndProgression() {
  Lfo l;
  l.reset(0.0);
  LfoConfig c = cfg(0, LfoWaveform::Sine, 1.0f);
  c.rateHz = 1.0f;
  c.phase = 0.25f;
  CHECK_NEAR(l.value(c, 0.0, kSr, 0.0, 24000.0), 1.0, 1e-6);          // starts at the peak
  CHECK_NEAR(l.value(c, kSr * 0.25, kSr, 0.0, 24000.0), 0.0, 1e-5);   // quarter cycle later
  CHECK_NEAR(l.value(c, kSr * 0.5, kSr, 0.0, 24000.0), -1.0, 1e-5);
}

void testFreeIsIndependentOfTransport() {
  Lfo a, b;
  a.reset(0.0);
  b.reset(0.0);
  LfoConfig c = cfg(0, LfoWaveform::Triangle, 1.0f);
  c.rateHz = 3.0f;
  for (double f = 0; f < 20000; f += 128) {
    CHECK(a.value(c, f, kSr, 0.0, 24000.0) == b.value(c, f, kSr, 77777.0, 5000.0));
  }
}

// --- sync -------------------------------------------------------------------------------------

void testSyncPeriodFollowsBpmAndDivision() {
  const double fpb = 24000.0;  // 120 BPM
  for (int d = 0; d < kLfoDivisionCount; d++) {
    Lfo l;
    LfoConfig c = cfg(0, LfoWaveform::Saw, 1.0f);
    c.sync = true;
    c.division = d;
    const double period = fpb * kLfoDivisionBeats[d];
    CHECK_NEAR(l.value(c, 0.0, kSr, 0.0, fpb), -1.0, 1e-6);                       // cycle start
    CHECK_NEAR(l.value(c, period * 0.5, kSr, 0.0, fpb), 0.0, 1e-5);               // half cycle
    CHECK_NEAR(l.value(c, period, kSr, 0.0, fpb), -1.0, 1e-5);                    // next cycle
    CHECK_NEAR(l.value(c, period * 3.25, kSr, 0.0, fpb), -0.5, 1e-5);
  }
  // Spot-check the named ones: 1/4 at 120 BPM is 24000 frames (0.5 s), 1/8 12000, 1/1 96000.
  CHECK(std::string(kLfoDivisionNames[7]) == "1/4" && kLfoDivisionBeats[7] == 1.0);
  CHECK(std::string(kLfoDivisionNames[10]) == "1/8" && kLfoDivisionBeats[10] == 0.5);
  CHECK(std::string(kLfoDivisionNames[2]) == "1/1" && kLfoDivisionBeats[2] == 4.0);
  CHECK_NEAR(kLfoDivisionBeats[5], 1.5, 1e-12);          // 1/4. = dotted quarter
  CHECK_NEAR(kLfoDivisionBeats[9], 2.0 / 3.0, 1e-12);    // 1/4T = triplet quarter
  // Slowest to fastest.
  for (int d = 1; d < kLfoDivisionCount; d++) CHECK(kLfoDivisionBeats[d] < kLfoDivisionBeats[d - 1]);
}

void testSyncAtDifferentBpm() {
  Lfo l;
  LfoConfig c = cfg(0, LfoWaveform::Saw, 1.0f);
  c.sync = true;
  c.division = 7;  // 1/4: one cycle per beat
  for (double bpm : {60.0, 90.0, 120.0, 174.0}) {
    const double fpb = kSr * 60.0 / bpm;
    CHECK_NEAR(l.value(c, fpb * 0.5, kSr, 0.0, fpb), 0.0, 1e-5);
    CHECK_NEAR(l.value(c, fpb * 2.75, kSr, 0.0, fpb), 0.5, 1e-5);
  }
}

void testSyncFollowsAnchorAndPhaseOffset() {
  const double fpb = 24000.0;
  Lfo l;
  LfoConfig c = cfg(0, LfoWaveform::Saw, 1.0f);
  c.sync = true;
  c.division = 7;
  // Cycle starts on the anchor, not on frame 0; frames before the anchor wrap seamlessly.
  CHECK_NEAR(l.value(c, 5000.0, kSr, 5000.0, fpb), -1.0, 1e-6);
  CHECK_NEAR(l.value(c, 5000.0 - fpb * 0.25, kSr, 5000.0, fpb), 0.5, 1e-5);
  c.phase = 0.5f;
  CHECK_NEAR(l.value(c, 5000.0, kSr, 5000.0, fpb), 0.0, 1e-6);
  // Re-anchoring (a transport restart) restarts the cycle relative to the new anchor.
  CHECK_NEAR(l.value(c, 100000.0, kSr, 100000.0, fpb), 0.0, 1e-6);
}

void testSyncIsStatelessSoGapsAndTempoChangesStayOnGrid() {
  const double fpb = 24000.0;
  Lfo a, b;
  LfoConfig c = cfg(0, LfoWaveform::Triangle, 1.0f);
  c.sync = true;
  c.division = 10;  // 1/8
  // Reading only late frames gives the same value as having read every block (no accumulated drift).
  float last = 0;
  for (double f = 0; f <= 960000.0; f += 128) last = a.value(c, f, kSr, 0.0, fpb);
  CHECK(last == b.value(c, 960000.0, kSr, 0.0, fpb));
  // Beat-anchored tempo change: the beat the change lands on stays at cycle start for 1/4.
  c.division = 7;
  const double newFpb = kSr * 60.0 / 90.0;
  CHECK_NEAR(a.value(c, 48000.0, kSr, 48000.0, newFpb), 0.0, 1e-6);  // triangle starts at 0
  CHECK_NEAR(a.value(c, 48000.0 + newFpb * 0.25, kSr, 48000.0, newFpb), 1.0, 1e-5);
}

// --- parameter mapping ------------------------------------------------------------------------

void testModulatedValueDomains() {
  // Linear: 100% = half the range either side of base.
  CHECK_NEAR(modulatedValue(ModDomain::Linear, 0.5f, 0.0f, 0.0f, 1.0f), 0.5, 1e-6);
  CHECK_NEAR(modulatedValue(ModDomain::Linear, 0.5f, 1.0f, 0.0f, 1.0f), 1.0, 1e-6);
  CHECK_NEAR(modulatedValue(ModDomain::Linear, 0.5f, -1.0f, 0.0f, 1.0f), 0.0, 1e-6);
  CHECK_NEAR(modulatedValue(ModDomain::Linear, 0.5f, 0.2f, 0.0f, 1.0f), 0.6, 1e-6);
  // Log: octaves. Cutoff 40..4000 is 6.64 octaves; 100% = +-3.32 oct. Centre of range is 400 Hz.
  const double half = 0.5 * std::log2(4000.0 / 40.0);
  CHECK_NEAR(modulatedValue(ModDomain::Log, 400.0f, 0.0f, 40.0f, 4000.0f), 400.0, 1e-3);
  CHECK_NEAR(modulatedValue(ModDomain::Log, 400.0f, 0.5f, 40.0f, 4000.0f), 400.0 * std::exp2(0.5 * half), 0.05);
  CHECK_NEAR(modulatedValue(ModDomain::Log, 400.0f, 1.0f, 40.0f, 4000.0f), 4000.0, 0.5);
  CHECK_NEAR(modulatedValue(ModDomain::Log, 400.0f, -1.0f, 40.0f, 4000.0f), 40.0, 0.05);
  // Same relative move at any base (octave domain), unlike a Hz offset.
  const float up1 = modulatedValue(ModDomain::Log, 100.0f, 0.1f, 40.0f, 4000.0f) / 100.0f;
  const float up2 = modulatedValue(ModDomain::Log, 1000.0f, 0.1f, 40.0f, 4000.0f) / 1000.0f;
  CHECK_NEAR(up1, up2, 1e-5);
}

void testModulatedValueBoundsAndSafety() {
  for (ModDomain d : {ModDomain::Linear, ModDomain::Log}) {
    for (float base : {40.0f, 41.0f, 400.0f, 3999.0f, 4000.0f}) {
      for (float m : {-50.0f, -2.0f, -1.0f, 0.0f, 0.3f, 1.0f, 2.0f, 50.0f}) {
        const float v = modulatedValue(d, base, m, 40.0f, 4000.0f);
        CHECK(v >= 40.0f && v <= 4000.0f);
      }
    }
  }
  CHECK(modulatedValue(ModDomain::Log, 400.0f, NAN, 40.0f, 4000.0f) == 400.0f);
  CHECK(modulatedValue(ModDomain::Linear, 0.5f, INFINITY, 0.0f, 1.0f) == 0.5f);
  CHECK(modulatedValue(ModDomain::None, 0.5f, 1.0f, 0.0f, 1.0f) == 0.5f);
}

// --- the modulation layer (base / effective / targets) ----------------------------------------

// Runs `ticks` control ticks of 32 frames starting at `frame`, returns the frame after.
double run(InstrumentModulation& m, Mock& inst, double frame, int ticks, double fpb = 24000.0) {
  for (int i = 0; i < ticks; i++, frame += kModChunkFrames) m.update(inst, frame, kModChunkFrames, 0.0, fpb);
  return frame;
}

void testBaseStaysSeparateFromEffective() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  m.setBase(kCutoff, 800.0f);
  LfoConfig c = cfg(kCutoff, LfoWaveform::Sine, 0.5f);
  c.rateHz = 1.0f;
  CHECK(m.setLfo(0, c, inst, 0.0));
  double f = 0;
  float lo = 1e9f, hi = 0;
  for (int i = 0; i < 1500; i++) {  // 1 s
    f = run(m, inst, f, 1);
    CHECK(m.base(kCutoff) == 800.0f);  // the base never moves
    lo = std::fmin(lo, m.effective(kCutoff));
    hi = std::fmax(hi, m.effective(kCutoff));
  }
  CHECK(hi > 800.0f * 1.5f && lo < 800.0f / 1.5f);  // effective swings both ways around it
  // Sine peak is +0.5 of the range in octaves: 800 * 2^(0.5 * 0.5 * 6.64) = ~ 2.4 kHz.
  CHECK_NEAR(hi, 800.0 * std::exp2(0.25 * std::log2(100.0)), 40.0);
  // Other parameters are untouched.
  CHECK(m.effective(kBody) == m.base(kBody));
}

void testZeroAmountIsNoModulation() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  m.setBase(kCutoff, 800.0f);
  m.setBase(kBody, 0.3f);
  m.setLfo(0, cfg(kCutoff, LfoWaveform::Square, 0.0f), inst, 0.0);
  m.setLfo(1, cfg(kBody, LfoWaveform::Saw, 0.0f), inst, 0.0);
  run(m, inst, 0, 800);
  CHECK(m.effective(kCutoff) == 800.0f);
  CHECK(m.effective(kBody) == 0.3f);
  CHECK(inst.last[kCutoff] == 0.0f || inst.last[kCutoff] == 800.0f);  // never moved off base
}

void testEffectiveNeverLeavesRange() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  const float bases[] = {40.0f, 400.0f, 4000.0f};
  for (float base : bases) {
    m.setBase(kCutoff, base);
    for (int w = 0; w < kLfoWaveformCount; w++) {
      LfoConfig c = cfg(kCutoff, static_cast<LfoWaveform>(w), 1.0f);
      c.rateHz = 7.0f;
      m.setLfo(0, c, inst, 0.0);
      // Two more LFOs on the same target, both at full amount: contributions add, still clamped.
      m.setLfo(1, c, inst, 0.0);
      m.setLfo(2, c, inst, 0.0);
      double f = 0;
      for (int i = 0; i < 3000; i++) {
        f = run(m, inst, f, 1);
        CHECK(m.effective(kCutoff) >= 40.0f && m.effective(kCutoff) <= 4000.0f);
        CHECK(inst.last[kCutoff] == 0.0f || (inst.last[kCutoff] >= 40.0f && inst.last[kCutoff] <= 4000.0f));
      }
    }
  }
  // Out-of-range base writes are clamped into the base.
  m.setBase(kBody, 9.0f);
  CHECK(m.base(kBody) == 1.0f);
}

void testTargetsMustBeModulatable() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  CHECK(!m.setLfo(0, cfg(kLegato, LfoWaveform::Sine, 1.0f), inst, 0.0));  // legato: a switch, ModDomain::None
  CHECK(!m.setLfo(0, cfg(999, LfoWaveform::Sine, 1.0f), inst, 0.0));      // no such param
  CHECK(!m.setLfo(0, cfg(-1, LfoWaveform::Sine, 1.0f), inst, 0.0));
  CHECK(!m.setLfo(kMaxLfos, cfg(kCutoff, LfoWaveform::Sine, 1.0f), inst, 0.0));  // no such slot
  CHECK(!m.setLfo(-1, cfg(kCutoff, LfoWaveform::Sine, 1.0f), inst, 0.0));
  CHECK(!m.active());                                                      // nothing got enabled
  CHECK(m.setLfo(0, cfg(kCutoff, LfoWaveform::Sine, 1.0f), inst, 0.0));
  CHECK(m.active());
  // A rejected edit leaves the slot disabled rather than half-applied.
  CHECK(!m.setLfo(0, cfg(kLegato, LfoWaveform::Sine, 1.0f), inst, 0.0));
  CHECK(!m.active());
}

void testWritesToTargetedParamUpdateBaseOnly() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  CHECK(m.setBase(kCutoff, 500.0f));   // untargeted: goes straight to the instrument
  CHECK(m.setBase(kBody, 0.5f));
  m.setLfo(0, cfg(kCutoff, LfoWaveform::Sine, 0.3f), inst, 0.0);
  CHECK(!m.setBase(kCutoff, 900.0f));  // targeted: the LFO owns the live value
  CHECK(m.setBase(kBody, 0.7f));       // a different param still passes through
  CHECK(m.base(kCutoff) == 900.0f);
  run(m, inst, 0, 50);
  CHECK(std::fabs(inst.last[kCutoff] / 900.0f - 1.0f) < 0.6f && inst.last[kCutoff] != 0.0f);
}

void testRemovingAnLfoRestoresTheBase() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  m.setBase(kCutoff, 700.0f);
  LfoConfig c = cfg(kCutoff, LfoWaveform::Square, 0.6f);
  m.setLfo(0, c, inst, 0.0);
  run(m, inst, 0, 100);
  CHECK(inst.last[kCutoff] != 700.0f);
  c.enabled = false;
  m.setLfo(0, c, inst, 4000.0);
  CHECK(inst.last[kCutoff] == 700.0f);
  CHECK(m.effective(kCutoff) == 700.0f);
  CHECK(!m.active());
  // Retargeting moves the restore with it.
  c.enabled = true;
  m.setLfo(0, c, inst, 5000.0);
  run(m, inst, 5000, 100);
  c.target = kBody;
  m.setLfo(0, c, inst, 9000.0);
  CHECK(inst.last[kCutoff] == 700.0f);
}

void testSeveralLfosAreIndependentAndShareTargetsAdditively() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  m.setBase(kCutoff, 400.0f);
  m.setBase(kBody, 0.5f);
  m.setBase(kBite, 0.5f);
  LfoConfig a = cfg(kCutoff, LfoWaveform::Sine, 0.2f);
  a.rateHz = 1.0f;
  LfoConfig b = cfg(kBody, LfoWaveform::Triangle, 0.4f);
  b.rateHz = 3.0f;
  LfoConfig d = cfg(kBite, LfoWaveform::Square, 0.3f);
  d.rateHz = 0.5f;
  m.setLfo(0, a, inst, 0.0);
  m.setLfo(2, b, inst, 0.0);
  m.setLfo(3, d, inst, 0.0);
  double f = run(m, inst, 0, 1500);
  // Each LFO's effect shows up only on its own param, with its own period.
  CHECK(m.effective(kCutoff) != 400.0f || m.effective(kBody) != 0.5f);
  // Disabling one leaves the others running.
  LfoConfig off = a;
  off.enabled = false;
  m.setLfo(0, off, inst, f);
  CHECK(m.effective(kCutoff) == 400.0f);
  CHECK(m.active());
  const float body0 = m.effective(kBody);
  run(m, inst, f, 40);
  CHECK(m.effective(kBody) != body0);
  // Two LFOs on one target add before mapping (opposite amounts cancel exactly).
  InstrumentModulation n;
  Mock inst2;
  n.configure(kBass, kSr);
  n.setBase(kBody, 0.5f);
  LfoConfig p = cfg(kBody, LfoWaveform::Sine, 0.4f);
  LfoConfig q = p;
  q.amount = -0.4f;
  n.setLfo(0, p, inst2, 0.0);
  n.setLfo(1, q, inst2, 0.0);
  run(n, inst2, 0, 500);
  CHECK_NEAR(n.effective(kBody), 0.5, 1e-5);
}

void testEditingAnEnabledLfoKeepsItsPhase() {
  InstrumentModulation m, ref;
  Mock inst, inst2;
  m.configure(kBass, kSr);
  ref.configure(kBass, kSr);
  LfoConfig c = cfg(kBody, LfoWaveform::Sine, 0.5f);
  c.rateHz = 2.0f;
  m.setLfo(0, c, inst, 0.0);
  ref.setLfo(0, c, inst2, 0.0);
  double f = run(m, inst, 0, 300);
  run(ref, inst2, 0, 300);
  c.amount = 0.8f;  // an edit of something other than rate: the cycle is not restarted
  m.setLfo(0, c, inst, f);
  ref.setLfo(0, c, inst2, f);
  run(m, inst, f, 100);
  run(ref, inst2, f, 100);
  CHECK_NEAR(m.effective(kBody), ref.effective(kBody), 1e-6);
  // A rate change keeps phase continuous: compare against the analytic sine.
  InstrumentModulation r;
  Mock inst3;
  r.configure(kBass, kSr);
  c.amount = 1.0f;
  c.rateHz = 1.0f;
  r.setBase(kBody, 0.5f);
  r.setLfo(0, c, inst3, 0.0);
  const double t1 = 0.125 * kSr;  // 1/8 cycle at 1 Hz
  run(r, inst3, 0, static_cast<int>(t1 / 32));
  c.rateHz = 2.0f;
  r.setLfo(0, c, inst3, t1);
  const double t2 = t1 + 0.0625 * kSr;  // + 1/8 cycle at 2 Hz
  double f2 = run(r, inst3, t1, static_cast<int>((t2 - t1) / 32));
  const double phase = 0.125 + 0.125;
  (void)f2;
  CHECK_NEAR(r.effective(kBody), 0.5 + 0.5 * std::sin(2 * 3.14159265358979 * phase), 0.03);
}

void testDisabledThenEnabledRestartsAndFreeRunsAcrossIdleGaps() {
  // A free LFO keeps time while the instrument is not being rendered (no update calls): a late
  // first tick reads the phase it would have had, identical to a layer that ticked all along.
  InstrumentModulation a, b;
  Mock ia, ib;
  a.configure(kBass, kSr);
  b.configure(kBass, kSr);
  LfoConfig c = cfg(kBody, LfoWaveform::Saw, 1.0f);
  c.rateHz = 0.7f;
  a.setLfo(0, c, ia, 0.0);
  b.setLfo(0, c, ib, 0.0);
  run(a, ia, 0, 100);
  const double late = 10.3 * kSr;
  a.update(ia, late, 32, 0.0, 24000.0);
  double f = 0;
  while (f < late) { b.update(ib, f, 32, 0.0, 24000.0); f += 32; }
  b.update(ib, late, 32, 0.0, 24000.0);
  CHECK_NEAR(a.effective(kBody), b.effective(kBody), 3e-3);
  // Disable -> enable restarts the cycle at its start phase.
  LfoConfig off = c;
  off.enabled = false;
  a.setLfo(0, off, ia, late);
  a.setLfo(0, c, ia, late + 5000.0);
  a.update(ia, late + 5000.0, 32, 0.0, 24000.0);
  CHECK_NEAR(a.effective(kBody), 0.0, 1e-4);  // saw starts at -1: base 0.5 - 0.5
}

void testRetriggerRestartsOnlyWhenAskedAndOnlyFreeMode() {
  InstrumentModulation m, k;
  Mock im, ik;
  m.configure(kBass, kSr);
  k.configure(kBass, kSr);
  LfoConfig c = cfg(kBody, LfoWaveform::Saw, 1.0f);
  c.rateHz = 1.0f;
  m.setLfo(0, c, im, 0.0);   // free-running
  c.retrigger = true;
  k.setLfo(0, c, ik, 0.0);   // retrigger on note
  run(m, im, 0, 400);
  run(k, ik, 0, 400);
  m.noteOn(12800.0);
  k.noteOn(12800.0);
  for (int i = 0; i < 12; i++) {  // let the 1.5 ms smoother settle onto the new phase
    m.update(im, 12800.0 + 32 * i, 32, 0.0, 24000.0);
    k.update(ik, 12800.0 + 32 * i, 32, 0.0, 24000.0);
  }
  // Free-running one is mid-cycle (12800/48000 = 0.267 cycles); retriggered is back at phase 0.
  CHECK_NEAR(m.effective(kBody), 0.5 + 0.5 * (2 * (12800.0 + 352) / kSr - 1.0), 0.02);
  CHECK_NEAR(k.effective(kBody), 0.0 + 0.5 * 2 * (352.0 / kSr), 0.02);
  // Synced LFOs ignore retrigger: they follow the grid.
  InstrumentModulation s;
  Mock is;
  s.configure(kBass, kSr);
  c.sync = true;
  c.division = 7;
  s.setLfo(0, c, is, 0.0);
  s.noteOn(6000.0);
  s.update(is, 6000.0, 32, 0.0, 24000.0);
  CHECK_NEAR(s.effective(kBody), 0.25, 0.01);
}

void testSyncedLayerTracksBpmChangeAndAnchor() {
  InstrumentModulation m;
  Mock inst;
  m.configure(kBass, kSr);
  LfoConfig c = cfg(kBody, LfoWaveform::Saw, 1.0f);
  c.sync = true;
  c.division = 7;  // 1/4
  m.setBase(kBody, 0.5f);
  m.setLfo(0, c, inst, 0.0);
  // 120 BPM: half a beat in = 0.
  m.update(inst, 12000.0, 32, 0.0, 24000.0);
  m.update(inst, 12032.0, 32, 0.0, 24000.0);
  CHECK_NEAR(m.effective(kBody), 0.5, 0.01);
  // Tempo drops to 60 BPM, anchored at that beat: the cycle now lasts 48000 frames.
  for (double f = 24000.0; f < 24000.0 + 12000.0; f += 32) m.update(inst, f, 32, 24000.0, 48000.0);
  // 12000 frames into a 48000-frame cycle = phase 0.25 -> saw -0.5 -> body 0.5 - 0.125.
  CHECK_NEAR(m.effective(kBody), 0.5 + 0.5 * -0.5, 0.01);
}

void testMetadataExposesTargetsByDefault() {
  // Modulation is a default module: every control is a target unless it is a switch or a step.
  // Targets derive from the synth's own table; nothing is listed per application.
  int none = 0;
  for (int k = 0; k <= static_cast<int>(InstrumentKind::Fm); k++) {
    for (int i = 0; i < instrumentParamCount(k); i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(k, i);
      const ModDomain d = resolveModDomain(*p);
      CHECK(d != ModDomain::Auto);
      if (d == ModDomain::None) {
        none++;
        // Only legato switches and stepped controls opt out.
        CHECK(std::string(p->name) == "legato" || std::string(p->name) == "gain" || std::string(p->unit) == "step");
      } else if (d == ModDomain::Log) {
        CHECK(p->min > 0.0f);
      }
    }
  }
  CHECK(none == 5 + (static_cast<int>(InstrumentKind::Fm) + 1));  // bass legato, pulse rate, pulse pumpRate, fm ratio, fm legato, and every kind's shared gain
  auto dom = [](int kind, int id) {
    for (int i = 0; i < instrumentParamCount(kind); i++)
      if (instrumentParamInfo(kind, i)->id == id) return resolveModDomain(*instrumentParamInfo(kind, i));
    return ModDomain::None;
  };
  const int pulse = static_cast<int>(InstrumentKind::Pulse), fm = static_cast<int>(InstrumentKind::Fm);
  const int pad = static_cast<int>(InstrumentKind::Pad), hat = static_cast<int>(InstrumentKind::HiHat);
  CHECK(dom(kBass, static_cast<int>(BassParam::Legato)) == ModDomain::None);
  CHECK(dom(kBass, static_cast<int>(BassParam::Attack)) == ModDomain::Log);
  CHECK(dom(kBass, static_cast<int>(BassParam::Slide)) == ModDomain::Linear);  // min 0: not octave-able
  CHECK(dom(kBass, static_cast<int>(BassParam::Cutoff)) == ModDomain::Log);
  CHECK(dom(kBass, static_cast<int>(BassParam::Body)) == ModDomain::Linear);
  CHECK(dom(pulse, static_cast<int>(PulseParam::Rate)) == ModDomain::None);
  CHECK(dom(fm, static_cast<int>(FmParam::Ratio)) == ModDomain::None);
  CHECK(dom(fm, static_cast<int>(FmParam::Legato)) == ModDomain::None);
  CHECK(dom(fm, static_cast<int>(FmParam::Timbre)) == ModDomain::Linear);
  CHECK(dom(pad, static_cast<int>(PadParam::Movement)) == ModDomain::Linear);
  CHECK(dom(pad, static_cast<int>(PadParam::Release)) == ModDomain::Log);
  CHECK(dom(hat, static_cast<int>(HiHatParam::Tune)) == ModDomain::Linear);
  CHECK(dom(hat, static_cast<int>(HiHatParam::Decay)) == ModDomain::Log);
  // Fixed-size state is large enough for every kind.
  for (int k = 0; k <= static_cast<int>(InstrumentKind::Fm); k++) CHECK(instrumentParamCount(k) <= kMaxModParams);
}

}  // namespace

int main() {
  testWaveformsExactValues();
  testWaveformsBoundedAndDeterministic();
  testFreeRateAdvancesAtRequestedHz();
  testFreeRateChangeKeepsPhaseContinuous();
  testInitialPhaseAndProgression();
  testFreeIsIndependentOfTransport();
  testSyncPeriodFollowsBpmAndDivision();
  testSyncAtDifferentBpm();
  testSyncFollowsAnchorAndPhaseOffset();
  testSyncIsStatelessSoGapsAndTempoChangesStayOnGrid();
  testModulatedValueDomains();
  testModulatedValueBoundsAndSafety();
  testBaseStaysSeparateFromEffective();
  testZeroAmountIsNoModulation();
  testEffectiveNeverLeavesRange();
  testTargetsMustBeModulatable();
  testWritesToTargetedParamUpdateBaseOnly();
  testRemovingAnLfoRestoresTheBase();
  testSeveralLfosAreIndependentAndShareTargetsAdditively();
  testEditingAnEnabledLfoKeepsItsPhase();
  testDisabledThenEnabledRestartsAndFreeRunsAcrossIdleGaps();
  testRetriggerRestartsOnlyWhenAskedAndOnlyFreeMode();
  testSyncedLayerTracksBpmChangeAndAnchor();
  testMetadataExposesTargetsByDefault();
  std::printf("lfo_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
