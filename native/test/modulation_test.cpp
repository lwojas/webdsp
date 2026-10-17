#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Engine-level LFO modulation (modulation.h through Engine::setInstrumentLfo) on the real Bass and
// Pulse, plus the guarantees that matter for protecting the synths: with no (or zero-amount) LFO
// the audio is bit-identical, and instruments that declare no targets reject modulation.
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
constexpr int32_t kBass = static_cast<int32_t>(InstrumentKind::Bass);
constexpr int32_t kPulse = static_cast<int32_t>(InstrumentKind::Pulse);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
constexpr int32_t B(BassParam p) { return static_cast<int32_t>(p); }
constexpr int32_t P(PulseParam p) { return static_cast<int32_t>(p); }

void run(Engine& e, int64_t& frame, double seconds, Buf* l = nullptr) {
  const int32_t blocks = static_cast<int32_t>(seconds * kRate / kBlock);
  for (int32_t i = 0; i < blocks; i++) {
    e.process(frame, kBlock);
    frame += kBlock;
    if (l) l->insert(l->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
}

LfoConfig lfo(int32_t target, LfoWaveform w, float amount, float hz = 1.0f) {
  LfoConfig c;
  c.enabled = true;
  c.target = target;
  c.waveform = w;
  c.amount = amount;
  c.rateHz = hz;
  return c;
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
double centroid(const Buf& v, size_t a, size_t b, double f0) {
  double num = 0, den = 0;
  for (int h = 1; h <= 40; h++) {
    const double m = bin(v, a, b, f0 * h);
    num += f0 * h * m;
    den += m;
  }
  return den > 0 ? num / den : 0;
}
double peakFreq(const Buf& v, size_t a, size_t b, double nominal) {
  double best = 0, bestHz = 0;
  for (double hz = nominal * 0.9; hz <= nominal * 1.15; hz += 0.1) {
    const double m = bin(v, a, b, hz);
    if (m > best) { best = m; bestHz = hz; }
  }
  return bestHz;
}
size_t at(double s) { return static_cast<size_t>(s * kRate); }
bool finiteBounded(const Buf& v, float limit = 4.0f) {
  for (float x : v) if (!std::isfinite(x) || std::fabs(x) > limit) return false;
  return true;
}

// A held Bass note (bright saw so cutoff is audible), optionally with a modulation setup.
struct Rig {
  Engine e;
  int64_t f = 0;
  explicit Rig(int32_t kind = kBass) {
    e.init(kRate, 2, 8);
    e.createInstrument(1, kind, 0);
    if (kind == kBass) {
      e.setInstrumentParam(1, B(BassParam::Body), 1.0f);
      e.setInstrumentParam(1, B(BassParam::Cutoff), 400.0f);
      e.setInstrumentParam(1, B(BassParam::Sustain), 1.0f);
      e.setInstrumentParam(1, B(BassParam::Bite), 0.0f);
    } else {
      e.setInstrumentParam(1, P(PulseParam::Cutoff), 500.0f);
      e.setInstrumentParam(1, P(PulseParam::Pump), 0.0f);
      e.setInstrumentParam(1, P(PulseParam::Repeat), 0.0f);
    }
  }
};

// --- audio is untouched unless an LFO actually modulates --------------------------------------

Buf render(int32_t kind, bool withZeroLfo, double seconds) {
  Rig r(kind);
  if (withZeroLfo) {
    // Zero amount on several targets: the chunked render path must be bit-identical to the plain one.
    const int32_t cut = kind == kBass ? B(BassParam::Cutoff) : P(PulseParam::Cutoff);
    const int32_t tune = kind == kBass ? B(BassParam::Tune) : P(PulseParam::Tune);
    CHECK(r.e.setInstrumentLfo(1, 0, lfo(cut, LfoWaveform::Sine, 0.0f, 3.0f)));
    CHECK(r.e.setInstrumentLfo(1, 1, lfo(tune, LfoWaveform::Square, 0.0f, 7.0f)));
  }
  r.e.scheduleTransport(0, 0.0, 124.0);
  r.e.instrumentNoteOn(1, 0.9f, 0.0f, 0.0f, static_cast<int32_t>(0.6 * kRate));
  Buf out;
  run(r.e, r.f, seconds * 0.5, &out);
  r.e.scheduleInstrumentNote(r.f + 300, 1, 0.7f, 0.0f, 5.0f, 9000);  // mid-block retrigger
  run(r.e, r.f, seconds * 0.5, &out);
  return out;
}

void testNoLfoAndZeroAmountAreBitIdentical() {
  for (int32_t kind : {kBass, kPulse}) {
    const Buf plain = render(kind, false, 1.5), zero = render(kind, true, 1.5);
    CHECK(plain.size() == zero.size());
    CHECK(std::memcmp(plain.data(), zero.data(), plain.size() * sizeof(float)) == 0);
    CHECK(finiteBounded(plain, 1.5f));
  }
}

// --- base / effective through the engine -------------------------------------------------------

void testBaseEffectiveAndRestore() {
  Rig r;
  const int32_t cut = B(BassParam::Cutoff);
  CHECK(r.e.setInstrumentLfo(1, 0, lfo(cut, LfoWaveform::Square, 0.5f, 2.0f)));
  r.e.instrumentNoteOn(1, 1.0f);
  Buf out;
  run(r.e, r.f, 0.1, &out);  // square is in its +half
  CHECK(r.e.instrumentBaseParam(1, cut) == 400.0f);
  const float up = r.e.instrumentEffectiveParam(1, cut);
  CHECK(up > 400.0f * 2.0f);  // +0.25 x 6.64 octaves ~ 3.2x
  run(r.e, r.f, 0.3, &out);   // 0.4 s: second half of a 2 Hz square (0.25..0.5 s)
  const float down = r.e.instrumentEffectiveParam(1, cut);
  CHECK(down < 400.0f / 2.0f);
  CHECK(r.e.instrumentBaseParam(1, cut) == 400.0f);
  // Writing the targeted param edits the base; the swing follows it.
  r.e.setInstrumentParam(1, cut, 800.0f);
  CHECK(r.e.instrumentBaseParam(1, cut) == 800.0f);
  run(r.e, r.f, 0.05, &out);
  CHECK(r.e.instrumentEffectiveParam(1, cut) < 800.0f / 2.0f);
  // Removing the LFO hands the base back, and nothing keeps modulating.
  LfoConfig off = lfo(cut, LfoWaveform::Square, 0.5f, 2.0f);
  off.enabled = false;
  CHECK(r.e.setInstrumentLfo(1, 0, off));
  CHECK(r.e.instrumentEffectiveParam(1, cut) == 800.0f);
  run(r.e, r.f, 0.2, &out);
  CHECK(r.e.instrumentEffectiveParam(1, cut) == 800.0f);
  CHECK(finiteBounded(out, 1.5f));
}

void testTargetValidationAndUnsupportedInstruments() {
  Rig r;
  CHECK(!r.e.setInstrumentLfo(99, 0, lfo(B(BassParam::Cutoff), LfoWaveform::Sine, 1.0f)));    // no instrument
  CHECK(!r.e.setInstrumentLfo(1, 0, lfo(B(BassParam::Attack), LfoWaveform::Sine, 1.0f)));     // not modulatable
  CHECK(!r.e.setInstrumentLfo(1, 0, lfo(B(BassParam::Legato), LfoWaveform::Sine, 1.0f)));
  CHECK(!r.e.setInstrumentLfo(1, kMaxLfos, lfo(B(BassParam::Cutoff), LfoWaveform::Sine, 1.0f)));
  CHECK(r.e.setInstrumentLfo(1, kMaxLfos - 1, lfo(B(BassParam::Cutoff), LfoWaveform::Sine, 1.0f)));
  // Pad, Fm and the drums declare no modulation targets: their synthesis is not touched.
  for (int32_t kind : {static_cast<int32_t>(InstrumentKind::Pad), static_cast<int32_t>(InstrumentKind::Fm),
                       static_cast<int32_t>(InstrumentKind::Kick)}) {
    Engine e;
    e.init(kRate, 2, 8);
    CHECK(e.createInstrument(1, kind, 0));
    for (int p = 0; p < instrumentParamCount(kind); p++) {
      CHECK(!e.setInstrumentLfo(1, 0, lfo(instrumentParamInfo(kind, p)->id, LfoWaveform::Sine, 1.0f)));
    }
  }
}

// --- audible modulation -------------------------------------------------------------------------

void testBassCutoffLfoMovesTimbreAroundTheBase() {
  auto play = [&](bool mod) {
    Rig r;
    if (mod) r.e.setInstrumentLfo(1, 0, lfo(B(BassParam::Cutoff), LfoWaveform::Sine, 0.4f, 1.0f));
    r.e.instrumentNoteOn(1, 1.0f);
    Buf out;
    run(r.e, r.f, 1.0, &out);
    return out;
  };
  const Buf dry = play(false), wet = play(true);
  // Sine peak at 0.25 s (brighter), trough at 0.75 s (darker), relative to the same moments dry.
  const double hi = centroid(wet, at(0.2), at(0.3), 55.0) / centroid(dry, at(0.2), at(0.3), 55.0);
  const double lo = centroid(wet, at(0.7), at(0.8), 55.0) / centroid(dry, at(0.7), at(0.8), 55.0);
  CHECK(hi > 1.15);
  CHECK(lo < 0.85);
  CHECK(finiteBounded(wet, 1.5f));
}

void testPitchLfoUsesOctaveDomain() {
  Rig r;
  const int32_t tune = B(BassParam::Tune);
  r.e.setInstrumentParam(1, B(BassParam::Body), 0.0f);  // clean sine fundamental
  // Square, rate 1: +half then -half. Tune range 27.5..110 -> +-1 oct at 100%, so 10% = +-0.1 oct.
  r.e.setInstrumentLfo(1, 0, lfo(tune, LfoWaveform::Square, 0.1f, 1.0f));
  r.e.instrumentNoteOn(1, 1.0f);
  Buf out;
  run(r.e, r.f, 1.0, &out);
  const double up = peakFreq(out, at(0.15), at(0.45), 55.0);
  const double down = peakFreq(out, at(0.65), at(0.95), 55.0);
  CHECK_NEAR(up, 55.0 * std::exp2(0.1), 55.0 * 0.01);
  CHECK_NEAR(down, 55.0 * std::exp2(-0.1), 55.0 * 0.01);
  CHECK(r.e.instrumentBaseParam(1, tune) == 55.0f);
}

void testPulseSyncedLfoLocksToTransportAndFollowsBpm() {
  Rig r(kPulse);
  const int32_t cut = P(PulseParam::Cutoff);
  r.e.scheduleTransport(0, 0.0, 120.0);
  LfoConfig c = lfo(cut, LfoWaveform::Sine, 0.5f);
  c.sync = true;
  c.division = 2;  // 1/1: four beats = 2 s at 120 BPM
  CHECK(r.e.setInstrumentLfo(1, 0, c));
  r.e.instrumentNoteOn(1, 1.0f);
  Buf out;
  const double half = 0.5 * std::log2(6000.0 / 60.0);
  auto expectAt = [&](double phase) { return 500.0 * std::exp2(0.5 * half * std::sin(2 * kPi * phase)); };
  run(r.e, r.f, 0.5, &out);  // quarter cycle: sine peak
  CHECK_NEAR(r.e.instrumentEffectiveParam(1, cut) / expectAt(0.25), 1.0, 0.05);
  run(r.e, r.f, 1.0, &out);  // three quarters: trough
  CHECK_NEAR(r.e.instrumentEffectiveParam(1, cut) / expectAt(0.75), 1.0, 0.05);
  // Tempo halves, re-anchored on the current frame: the cycle restarts and now lasts 4 s.
  const int64_t change = r.f;
  r.e.scheduleTransport(change, static_cast<double>(change), 60.0);
  run(r.e, r.f, 1.0, &out);  // 1 s of a 4 s cycle: sine peak again
  CHECK_NEAR(r.e.instrumentEffectiveParam(1, cut) / expectAt(0.25), 1.0, 0.05);
  CHECK(finiteBounded(out, 1.5f));
}

void testFreeLfoKeepsRunningWhileTheInstrumentIsIdle() {
  Rig r;
  const int32_t body = B(BassParam::Body);
  r.e.setInstrumentParam(1, body, 0.5f);
  LfoConfig c = lfo(body, LfoWaveform::Saw, 1.0f, 0.5f);
  CHECK(r.e.setInstrumentLfo(1, 0, c));
  run(r.e, r.f, 3.1);  // silence: no note. 3.1 s x 0.5 Hz = 1.55 cycles
  r.e.instrumentNoteOn(1, 1.0f);
  run(r.e, r.f, 0.01);
  // saw(0.55) = 0.1 -> body = 0.5 + 0.5 x 0.1 (plus 10 ms of travel)
  CHECK_NEAR(r.e.instrumentEffectiveParam(1, body), 0.55, 0.02);
}

void testRetriggerOptionRestartsAtNoteStart() {
  Rig r;
  const int32_t body = B(BassParam::Body);
  r.e.setInstrumentParam(1, body, 0.5f);
  LfoConfig c = lfo(body, LfoWaveform::Saw, 1.0f, 1.0f);
  c.retrigger = true;
  r.e.setInstrumentLfo(1, 0, c);
  r.e.instrumentNoteOn(1, 1.0f);
  run(r.e, r.f, 0.4);
  const int64_t noteAt = r.f + 4 * kBlock;
  r.e.scheduleInstrumentNote(noteAt, 1, 1.0f);
  run(r.e, r.f, 0.1);
  // 0.1 s - 4 blocks (~10.7 ms) after the note start: saw ~ -1 + 2 x 0.1 = -0.8 -> body ~0.1
  const double elapsed = double(r.f - noteAt) / kRate;
  CHECK_NEAR(r.e.instrumentEffectiveParam(1, body), 0.5 + 0.5 * (2 * elapsed - 1.0), 0.03);
}

// --- several LFOs ------------------------------------------------------------------------------

void testFourLfosOnDifferentTargets() {
  Rig r;
  const int32_t targets[] = {B(BassParam::Cutoff), B(BassParam::Body), B(BassParam::Bite),
                             B(BassParam::Resonance)};
  const LfoWaveform waves[] = {LfoWaveform::Sine, LfoWaveform::Triangle, LfoWaveform::Square, LfoWaveform::Saw};
  const float rates[] = {0.9f, 1.7f, 2.3f, 0.6f};
  for (int i = 0; i < kMaxLfos; i++) {
    CHECK(r.e.setInstrumentLfo(1, i, lfo(targets[i], waves[i], 0.3f, rates[i])));
  }
  r.e.instrumentNoteOn(1, 1.0f);
  Buf out;
  float seen[4][2];
  for (auto& s : seen) { s[0] = 1e9f; s[1] = -1e9f; }
  for (int k = 0; k < 400; k++) {
    run(r.e, r.f, 0.005, &out);
    for (int i = 0; i < 4; i++) {
      const float v = r.e.instrumentEffectiveParam(1, targets[i]);
      seen[i][0] = std::fmin(seen[i][0], v);
      seen[i][1] = std::fmax(seen[i][1], v);
    }
  }
  for (int i = 0; i < 4; i++) CHECK(seen[i][1] > seen[i][0]);  // every target moves on its own
  CHECK(finiteBounded(out, 1.5f));
  // Remove two: the survivors keep going, the removed return to base.
  LfoConfig off = lfo(targets[0], LfoWaveform::Sine, 0.3f);
  off.enabled = false;
  r.e.setInstrumentLfo(1, 0, off);
  off.target = targets[2];
  r.e.setInstrumentLfo(1, 2, off);
  CHECK(r.e.instrumentEffectiveParam(1, targets[0]) == r.e.instrumentBaseParam(1, targets[0]));
  CHECK(r.e.instrumentEffectiveParam(1, targets[2]) == r.e.instrumentBaseParam(1, targets[2]));
  const float b1 = r.e.instrumentEffectiveParam(1, targets[1]);
  run(r.e, r.f, 0.1, &out);
  CHECK(r.e.instrumentEffectiveParam(1, targets[1]) != b1);
}

// --- bounds and filter safety --------------------------------------------------------------------

void testExtremesStayFiniteAndInRange() {
  for (int32_t kind : {kBass, kPulse}) {
    for (LfoWaveform w : {LfoWaveform::Square, LfoWaveform::Saw, LfoWaveform::Triangle, LfoWaveform::Sine}) {
      Rig r(kind);
      const int32_t cut = kind == kBass ? B(BassParam::Cutoff) : P(PulseParam::Cutoff);
      const int32_t res = kind == kBass ? B(BassParam::Resonance) : P(PulseParam::Resonance);
      const int32_t tune = kind == kBass ? B(BassParam::Tune) : P(PulseParam::Tune);
      r.e.setInstrumentParam(1, res, 1.0f);
      if (kind == kBass) { r.e.setInstrumentParam(1, B(BassParam::Body), 1.0f); r.e.setInstrumentParam(1, B(BassParam::Bite), 1.0f); }
      r.e.setInstrumentLfo(1, 0, lfo(cut, w, 1.0f, 40.0f));
      r.e.setInstrumentLfo(1, 1, lfo(res, w, 1.0f, 33.0f));
      r.e.setInstrumentLfo(1, 2, lfo(tune, w, 1.0f, 17.0f));
      r.e.setInstrumentLfo(1, 3, lfo(cut, w, -1.0f, 29.0f));
      r.e.instrumentNoteOn(1, 1.0f);
      Buf out;
      for (int k = 0; k < 200; k++) {
        run(r.e, r.f, 0.01, &out);
        for (int32_t p : {cut, res, tune}) {
          const InstrumentParamInfo* info = nullptr;
          for (int i = 0; i < instrumentParamCount(kind); i++) {
            if (instrumentParamInfo(kind, i)->id == p) info = instrumentParamInfo(kind, i);
          }
          const float v = r.e.instrumentEffectiveParam(1, p);
          CHECK(v >= info->min && v <= info->max);
        }
      }
      CHECK(finiteBounded(out, 1.5f));
    }
  }
}

void testOutOfRangeBaseWithFullModulationStaysInRange() {
  Rig r;
  const int32_t cut = B(BassParam::Cutoff);
  r.e.setInstrumentParam(1, cut, 100000.0f);  // clamped into the base (4000)
  CHECK(r.e.instrumentBaseParam(1, cut) == 4000.0f);
  r.e.setInstrumentLfo(1, 0, lfo(cut, LfoWaveform::Square, 1.0f, 5.0f));
  r.e.instrumentNoteOn(1, 1.0f);
  Buf out;
  for (int k = 0; k < 100; k++) {
    run(r.e, r.f, 0.01, &out);
    CHECK(r.e.instrumentEffectiveParam(1, cut) <= 4000.0f && r.e.instrumentEffectiveParam(1, cut) >= 40.0f);
  }
}

// --- realtime safety -----------------------------------------------------------------------------

void testModulationDoesNotAllocate() {
  Rig r(kPulse);
  r.e.scheduleTransport(0, 0.0, 128.0);
  LfoConfig c = lfo(P(PulseParam::Cutoff), LfoWaveform::Sine, 0.5f);
  c.sync = true;
  r.e.setInstrumentLfo(1, 0, c);
  r.e.setInstrumentLfo(1, 1, lfo(P(PulseParam::Resonance), LfoWaveform::Saw, 0.4f, 3.0f));
  r.e.instrumentNoteOn(1, 1.0f);
  run(r.e, r.f, 0.1);
  const long before = g_allocs;
  for (int i = 0; i < 600; i++) {
    if (i % 100 == 50) r.e.instrumentNoteOn(1, 0.8f, 0, 3, 4000);
    if (i == 200) r.e.setInstrumentLfo(1, 2, lfo(P(PulseParam::Tune), LfoWaveform::Triangle, 0.1f, 5.0f));
    if (i == 300) r.e.setInstrumentParam(1, P(PulseParam::Cutoff), 900.0f);
    if (i == 400) { LfoConfig o = c; o.enabled = false; r.e.setInstrumentLfo(1, 0, o); }
    run(r.e, r.f, kBlock / kRate);
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testNoLfoAndZeroAmountAreBitIdentical();
  testBaseEffectiveAndRestore();
  testTargetValidationAndUnsupportedInstruments();
  testBassCutoffLfoMovesTimbreAroundTheBase();
  testPitchLfoUsesOctaveDomain();
  testPulseSyncedLfoLocksToTransportAndFollowsBpm();
  testFreeLfoKeepsRunningWhileTheInstrumentIsIdle();
  testRetriggerOptionRestartsAtNoteStart();
  testFourLfosOnDifferentTargets();
  testExtremesStayFiniteAndInRange();
  testOutOfRangeBaseWithFullModulationStaysInRange();
  testModulationDoesNotAllocate();
  std::printf("modulation_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
