#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the synthesized PWM Beast (instruments/pwmbeast.h) through the Engine's instrument API:
// registration, lifecycle, polyphony, pulse width, PWM sources, the harmonic character stage,
// drive, filter/envelope, bounds on extreme parameter sets, and allocation.
// Set PWMBEAST_WAV_DIR=<dir> to dump the cases as 16-bit WAVs for listening (docs/pwm-beast-instrument.md).
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
constexpr int32_t kPb = static_cast<int32_t>(InstrumentKind::PwmBeast);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
using P = PwmBeastParam;
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
  const char* dir = std::getenv("PWMBEAST_WAV_DIR");
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
  explicit Rig(int32_t bus = 0) { e.init(kRate, 2, 8); e.createInstrument(1, kPb, bus); }
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
// First-difference rms over rms: a cheap brightness measure (grows with high-frequency content).
double bright(const Buf& v, size_t a, size_t b) {
  double d = 0, s = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) { const double x = v[i] - v[i - 1]; d += x * x; s += double(v[i]) * v[i]; }
  return std::sqrt(d / std::fmax(s, 1e-12));
}
bool finite(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}

// A plain, filter-open, undriven pulse: the reference for measuring what each stage adds.
using Params = std::vector<std::pair<P, float>>;
const Params kClean = {{P::Tune, 110.0f}, {P::Pwm, 0.0f}, {P::Harmonics, 0.0f}, {P::Drive, 0.0f},
                       {P::Cutoff, 8000.0f}, {P::Resonance, 0.0f}, {P::EnvAmount, 0.0f}, {P::Sub, 0.0f},
                       {P::Attack, 0.002f}, {P::Sustain, 1.0f}};

Buf render(Params params, double seconds = 1.0, float pitch = 0.0f, const Params& base = kClean) {
  Rig r;
  for (auto& [p, v] : base) r.set(p, v);
  for (auto& [p, v] : params) r.set(p, v);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}

void testRegistrationAndDefaults() {
  const InstrumentKindMeta* m = instrumentKindMeta(kPb);
  CHECK(m && std::strcmp(m->id, "pwmbeast") == 0 && std::strcmp(m->name, "PWM Beast") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(instrumentParamCount(kPb) == kPwmBeastParamCount + 1 && kPwmBeastParamCount == 15);
  auto in = makeInstrument(kPb);
  CHECK(in && in->kind() == InstrumentKind::PwmBeast && !in->isActive());
  for (int32_t i = 0; i < kPwmBeastParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kPb, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max);
  }
  CHECK(instrumentParamInfo(kPb, kPwmBeastParamCount)->id == kInstrumentGainParam && instrumentParamInfo(kPb, kPwmBeastParamCount + 1) == nullptr);
  auto find = [](const char* n) { for (int32_t i = 0; i < kPwmBeastParamCount; i++) if (!std::strcmp(kPwmBeastParams[i].name, n)) return &kPwmBeastParams[i]; return (const InstrumentParamInfo*)nullptr; };
  CHECK_NEAR(find("width")->def, 0.3, 1e-6);
  CHECK_NEAR(find("tune")->def, 55.0, 1e-6);
  CHECK(find("width")->min >= 0.05f && find("width")->max <= 0.95f);  // safety margin from 0 / 100 %
  CHECK(resolveModDomain(*find("cutoff")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("rate")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("pwm")) == ModDomain::Linear);
  CHECK(resolveModDomain(*find("drive")) == ModDomain::Linear);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kPb, 0));
}

void testNoteStartAndRelease() {
  Rig r;
  r.set(P::Attack, 0.02f); r.set(P::Release, 0.3f);
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.8 * kRate));
  r.go(0.05);
  CHECK(r.e.activeInstrumentCount() == 1);
  CHECK(peak(r.o, 0, 8) < 0.01f);               // starts from silence: no click
  r.go(0.75);
  const double held = rms(r.o, at(0.5), at(0.75));
  CHECK(held > 0.05 && held < 0.7);
  r.go(0.6);
  CHECK(r.e.activeInstrumentCount() == 0);       // 0.8 s note + 0.3 s release is over
  CHECK(peak(r.o, at(1.3), r.o.size()) == 0.0f);
  CHECK(finite(r.o));
  writeWav("default_note", r.o);
}

void testDeterministic() {
  CHECK(render({}, 0.6, 0.0f, {}) == render({}, 0.6, 0.0f, {}));
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
  z.go(0.8);
  CHECK(z.e.activeInstrumentCount() == 0);
}

void testRetriggerIsContinuous() {
  Rig r;
  r.set(P::Attack, 0.01f); r.set(P::Cutoff, 2000.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.5);
  const size_t t = r.o.size();
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 7.0f);   // a note on a held note: carries phase/filter/level
  r.go(0.3);
  CHECK(finite(r.o));
  double steady = 0, around = 0;                  // largest sample step, steady vs across the retrigger
  for (size_t i = at(0.3) + 1; i < t - 1; i++) steady = std::fmax(steady, std::fabs(r.o[i] - r.o[i - 1]));
  for (size_t i = t - 64; i < t + 64; i++) around = std::fmax(around, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(around <= steady * 1.5);
}

void testMultipleSimultaneousNotes() {
  Engine e; e.init(kRate, 2, 8);
  const float chord[] = {0, 3, 7, 10, 12};
  for (int i = 0; i < 5; i++) {
    e.createInstrument(10 + i, kPb, 0);
    e.setInstrumentParam(10 + i, id(P::Release), 0.3f);
  }
  int64_t f = 0; Buf o;
  for (int i = 0; i < 5; i++) e.instrumentNoteOn(10 + i, 0.8f, 0.0f, chord[i]);
  run(e, f, 1.0, &o);
  CHECK(e.activeInstrumentCount() == 5);
  CHECK(finite(o));
  CHECK(rms(o, at(0.5), at(1.0)) > 0.1);
  for (int i = 0; i < 5; i++) e.instrumentNoteOff(10 + i);
  run(e, f, 0.6, &o);
  CHECK(e.activeInstrumentCount() == 0);
  writeWav("chord", o);
}

// A clean pulse has harmonic n with weight |sin(n pi w)| / n: the harmonic comb IS the width.
void testPulseWidthShapesSpectrum() {
  const Buf sq = render({{P::Width, 0.5f}});
  const Buf qtr = render({{P::Width, 0.25f}});
  const double a = at(0.3), b = at(1.0);
  const double sq1 = bin(sq, a, b, 110), sq2 = bin(sq, a, b, 220), sq3 = bin(sq, a, b, 330);
  CHECK(sq1 > 0.1 && sq2 < 0.05 * sq1);          // square: no even harmonics
  CHECK_NEAR(sq3 / sq1, 1.0 / 3.0, 0.08);
  const double q1 = bin(qtr, a, b, 110), q2 = bin(qtr, a, b, 220), q4 = bin(qtr, a, b, 440);
  CHECK(q2 > 0.4 * q1);                           // 25 %: second present...
  CHECK(q4 < 0.06 * q1);                          // ... fourth nulled (sin(4 pi / 4) = 0)
  // Mirror images share a clean spectrum (a pulse and its inverse differ only in sign).
  const Buf inv = render({{P::Width, 0.75f}});
  CHECK_NEAR(bin(inv, a, b, 220) / bin(inv, a, b, 110), q2 / q1, 0.06);
  CHECK_NEAR(rms(qtr, a, b), rms(sq, a, b), rms(sq, a, b) * 0.35);   // narrow pulses are levelled
  writeWav("width_025", qtr);
}

void testWidthExtremesStayValid() {
  for (float w : {0.05f, 0.5f, 0.95f}) {
    for (float pitch : {-12.0f, 0.0f, 24.0f, 48.0f}) {
      const Buf b = render({{P::Width, w}, {P::Pwm, 1.0f}, {P::Rate, 40.0f}, {P::Harmonics, 1.0f}, {P::Drive, 1.0f}}, 0.4, pitch);
      CHECK(finite(b) && peak(b) <= 1.0001f && rms(b, at(0.1), at(0.4)) > 0.01);
    }
  }
}

// With pwm 0 the width is static; with pwm up the harmonic comb walks.
void testPwmModulation() {
  // Second-harmonic strength per 50 ms window. Rates here are deliberately not a multiple of the
  // window rate: windows that line up with the LFO sample it at symmetric points and look constant.
  auto h2Windows = [](const Buf& b) {
    std::vector<double> w;
    for (double s = 0.2; s + 0.05 <= 1.4; s += 0.05) w.push_back(bin(b, at(s), at(s + 0.05), 220));
    return w;
  };
  auto spread = [](const std::vector<double>& w) {
    double lo = 1e9, hi = 0;
    for (double x : w) { lo = std::fmin(lo, x); hi = std::fmax(hi, x); }
    return std::pair<double, double>(lo, hi);
  };
  const auto still = spread(h2Windows(render({{P::Width, 0.5f}, {P::Pwm, 0.0f}}, 1.5)));
  const auto moving = spread(h2Windows(render({{P::Width, 0.5f}, {P::Pwm, 1.0f}, {P::Rate, 1.3f}, {P::Shape, 0.0f}}, 1.5)));
  CHECK(still.second < 0.02);
  CHECK(moving.second > 0.1 && moving.first < 0.5 * moving.second);
  // More depth, more movement.
  const auto half = spread(h2Windows(render({{P::Width, 0.5f}, {P::Pwm, 0.3f}, {P::Rate, 1.3f}, {P::Shape, 0.0f}}, 1.5)));
  CHECK(half.second > still.second && half.second < moving.second);
  // The rate sets the speed: at 40 Hz a window holds two whole cycles, so windows level out.
  const auto fast = spread(h2Windows(render({{P::Width, 0.5f}, {P::Pwm, 1.0f}, {P::Rate, 40.0f}}, 1.5)));
  CHECK(fast.second - fast.first < moving.second - moving.first);
  writeWav("pwm_slow", render({{P::Width, 0.5f}, {P::Pwm, 1.0f}, {P::Rate, 2.5f}}, 2.0));
}

// Shape 1 swaps the LFO for the note's own timbre envelope: rate no longer matters, and the
// width sweeps once per note instead of cycling.
void testPwmSourceCanBeTheEnvelope() {
  const Params env = {{P::Width, 0.5f}, {P::Pwm, 1.0f}, {P::Shape, 1.0f}, {P::Decay, 0.3f}, {P::Sustain, 1.0f}};
  Params a = env, b = env;
  a.push_back({P::Rate, 0.3f}); b.push_back({P::Rate, 90.0f});
  const Buf ra = render(a, 0.8), rb = render(b, 0.8);
  double diff = 0, ref = 0;
  for (size_t i = 0; i < ra.size(); i++) { diff += std::fabs(ra[i] - rb[i]); ref += std::fabs(ra[i]); }
  CHECK(diff < 1e-3 * ref);
  // The envelope starts the width at 0.5 + 0.42 and settles at the sustain offset, so the width
  // sweeps through 0.5 once (second harmonic nulls there) and then stays put.
  const double late = bin(ra, at(0.5), at(0.53), 220);
  double dip = 1e9;
  for (double t = 0.0; t < 0.3; t += 0.03) dip = std::fmin(dip, bin(ra, at(t), at(t + 0.03), 220));
  CHECK(late > 0.05 && dip < 0.3 * late);
  CHECK_NEAR(bin(ra, at(0.6), at(0.63), 220), late, late * 0.1);
  // And the middle of the dial is the skewed ramp: different movement from the sine.
  const Buf sine = render({{P::Width, 0.5f}, {P::Pwm, 1.0f}, {P::Rate, 3.0f}, {P::Shape, 0.0f}}, 1.0);
  const Buf ramp = render({{P::Width, 0.5f}, {P::Pwm, 1.0f}, {P::Rate, 3.0f}, {P::Shape, 0.5f}}, 1.0);
  CHECK(sine != ramp);
  CHECK(finite(sine) && finite(ramp));
}

// The character stage is not a pulse with extra gain: it adds harmonics a pulse cannot have
// (even ones on a square), keeps the fundamental, and keeps the level in the same range.
void testHarmonicCharacter() {
  const Buf clean = render({{P::Width, 0.5f}, {P::Harmonics, 0.0f}});
  const Buf mid = render({{P::Width, 0.5f}, {P::Harmonics, 0.5f}});
  const Buf full = render({{P::Width, 0.5f}, {P::Harmonics, 1.0f}});
  const double a = at(0.3), b = at(1.0);
  CHECK(bin(clean, a, b, 220) < 0.05 * bin(clean, a, b, 110));
  CHECK(bin(mid, a, b, 220) > 0.05 * bin(mid, a, b, 110));
  CHECK(bin(full, a, b, 220) > 0.15 * bin(full, a, b, 110));      // asymmetry: even harmonics
  CHECK(bin(full, a, b, 110) > 0.2 * rms(full, a, b));            // the fundamental is not destroyed
  // Level stays in the same range across the dial.
  const double rc = rms(clean, a, b), rf = rms(full, a, b), rm = rms(mid, a, b);
  CHECK(rf > rc * 0.5 && rf < rc * 2.0 && rm > rc * 0.5 && rm < rc * 2.0);
  // Distinct waveforms, monotonic in how far they have left the pulse.
  double dMid = 0, dFull = 0;
  for (size_t i = a; i < b; i++) { dMid += std::fabs(mid[i] - clean[i]); dFull += std::fabs(full[i] - clean[i]); }
  CHECK(dMid > 0.05 * rc * double(b - a) && dFull > dMid);
  // Across the keyboard: still bounded and audible at the extremes.
  for (float pitch : {-24.0f, 0.0f, 24.0f, 48.0f}) {
    const Buf k = render({{P::Harmonics, 1.0f}}, 0.5, pitch);
    CHECK(finite(k) && peak(k) <= 1.0001f && rms(k, at(0.2), at(0.5)) > 0.02);
  }
  writeWav("harmonics_full", full);
}

// Drive adds density, not volume.
void testDrive() {
  const Params base = {{P::Width, 0.3f}, {P::Harmonics, 0.5f}};
  auto with = [&](float d) { Params p = base; p.push_back({P::Drive, d}); return render(p); };
  const Buf d0 = with(0.0f), d3 = with(0.3f), d6 = with(0.6f), d10 = with(1.0f);
  const double a = at(0.3), b = at(1.0);
  CHECK(bright(d10, a, b) > bright(d0, a, b) * 1.1);
  CHECK(bright(d6, a, b) > bright(d0, a, b) * 1.03);
  CHECK(rms(d10, a, b) < rms(d0, a, b) * 1.7 && rms(d10, a, b) > rms(d0, a, b) * 0.6);   // compensated
  CHECK(rms(d6, a, b) < rms(d0, a, b) * 1.5 && rms(d6, a, b) > rms(d0, a, b) * 0.6);
  // A small amount already changes the waveform.
  double diff = 0;
  for (size_t i = a; i < b; i++) diff += std::fabs(d3[i] - d0[i]);
  CHECK(diff > 0.03 * rms(d0, a, b) * double(b - a));
  CHECK(finite(d10) && peak(d10) <= 1.0001f);
  writeWav("drive_full", d10);
}

void testFilterAndEnvelope() {
  // Cutoff brightens.
  const Buf dark = render({{P::Width, 0.3f}, {P::Cutoff, 200.0f}});
  const Buf open = render({{P::Width, 0.3f}, {P::Cutoff, 6000.0f}});
  CHECK(bright(open, at(0.3), at(1.0)) > 3.0 * bright(dark, at(0.3), at(1.0)));
  // Resonance puts a peak at the cutoff (a pulse harmonic: 6 x 110 = 660).
  const Buf flat = render({{P::Width, 0.3f}, {P::Cutoff, 660.0f}, {P::Resonance, 0.0f}});
  const Buf reson = render({{P::Width, 0.3f}, {P::Cutoff, 660.0f}, {P::Resonance, 1.0f}});
  CHECK(bin(reson, at(0.3), at(1.0), 660) > 1.5 * bin(flat, at(0.3), at(1.0), 660));
  CHECK(finite(reson) && peak(reson) <= 1.0001f);
  // The envelope opens the filter at the start of the note and lets it close: bright then round.
  const Params env = {{P::Width, 0.3f}, {P::Cutoff, 250.0f}, {P::Decay, 0.15f}, {P::Sustain, 0.6f}};
  Params off = env, on = env;
  off.push_back({P::EnvAmount, 0.0f}); on.push_back({P::EnvAmount, 1.0f});
  const Buf offB = render(off, 1.0), onB = render(on, 1.0);
  const double onEarly = bright(onB, at(0.01), at(0.08)), onLate = bright(onB, at(0.6), at(1.0));
  const double offEarly = bright(offB, at(0.01), at(0.08)), offLate = bright(offB, at(0.6), at(1.0));
  CHECK(onEarly > 1.5 * onLate);                    // opens, then settles
  CHECK(onEarly > 1.5 * offEarly);                  // more open than without the envelope
  CHECK_NEAR(onLate, offLate, offLate * 0.5);       // what remains is the sustained tone
  writeWav("filter_env", onB);
}

void testSub() {
  const Buf none = render({{P::Width, 0.5f}, {P::Sub, 0.0f}});
  const Buf full = render({{P::Width, 0.5f}, {P::Sub, 1.0f}});
  const double a = at(0.3), b = at(1.0);
  CHECK(bin(none, a, b, 55) < 0.01);
  CHECK(bin(full, a, b, 55) > 0.1);
  CHECK(bin(full, a, b, 55) < bin(full, a, b, 110) * 2.0);   // a support, not the sound
  CHECK(rms(full, a, b) < rms(none, a, b) * 1.7);
}

// Switching every extra off must not collapse the instrument into its own clean pulse, and the
// default patch is a different object from that pulse.
void testCharacterIsNotAPlainPulse() {
  const Buf plain = render({{P::Width, 0.3f}, {P::Cutoff, 500.0f}});
  const Buf beastNoPwm = render({{P::Pwm, 0.0f}, {P::Harmonics, 0.5f}, {P::Drive, 0.4f}, {P::Sub, 0.25f}, {P::EnvAmount, 0.7f}, {P::Cutoff, 500.0f}, {P::Resonance, 0.45f}, {P::Sustain, 0.6f}});
  const double a = at(0.3), b = at(1.0);
  double diff = 0, ref = 0;
  for (size_t i = a; i < b; i++) { diff += std::fabs(beastNoPwm[i] - plain[i]); ref += std::fabs(plain[i]); }
  CHECK(diff > 0.3 * ref);
  CHECK(std::fabs(bright(beastNoPwm, a, b) - bright(plain, a, b)) > 0.1 * bright(plain, a, b));
}

void testDefaultPatchIsPunchyAndLevelled() {
  Rig r;
  r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(1.0 * kRate));
  r.go(1.6);
  CHECK(finite(r.o) && peak(r.o) <= 1.0001f);
  const double held = rms(r.o, at(0.5), at(0.9));
  CHECK(held > 0.08 && held < 0.35);
  CHECK(rms(r.o, at(0.0), at(0.1)) > held);   // transient above the sustain
  writeWav("default_bass", r.o);
}

// Valid-but-extreme parameter sets: every combination of the edges, plus deterministic scatter.
void testExtremesStayFiniteAndBounded() {
  const int32_t n = kPwmBeastParamCount;
  unsigned s = 12345;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / float(1 << 24); };
  for (int trial = 0; trial < 60; trial++) {
    Rig r;
    for (int32_t i = 0; i < n; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kPb, i);
      float v;
      if (trial == 0) v = p->min;
      else if (trial == 1) v = p->max;
      else if (trial < 10) v = (rnd() < 0.5f) ? p->min : p->max;
      else v = p->min + rnd() * (p->max - p->min);
      r.e.setInstrumentParam(1, i, v);
    }
    const float pitch = trial % 3 == 0 ? -24.0f : (trial % 3 == 1 ? 0.0f : 40.0f);
    r.e.instrumentNoteOn(1, 1.5f, 0.0f, pitch);
    r.go(0.4);
    r.e.instrumentNoteOff(1);
    r.go(0.3);
    CHECK(finite(r.o));
    CHECK(peak(r.o) <= 1.0001f);
  }
}

void testParamValidation() {
  Rig r;
  r.set(P::Width, 5.0f); r.set(P::Drive, -1.0f); r.set(P::Rate, 1e9f);   // clamped
  r.set(P::Cutoff, std::nanf("")); r.set(P::Pwm, INFINITY);              // ignored
  r.e.setInstrumentParam(1, 99, 1.0f);                                    // unknown id: no-op
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.3);
  CHECK(finite(r.o) && peak(r.o) > 0.0f && peak(r.o) <= 1.0001f);
}

void testScheduledNoteStartsOnItsFrame() {
  Rig r;
  r.set(P::Attack, 0.005f);
  const int64_t at0 = 10000 + 37;
  r.e.scheduleInstrumentNote(at0, 1, 1.0f);
  r.go(0.4);
  CHECK(peak(r.o, 0, static_cast<size_t>(at0)) == 0.0f);
  CHECK(peak(r.o, static_cast<size_t>(at0), at(0.4)) > 0.01f);
}

void testRoutesThroughBusAndPan() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kPb, 0);
  e.instrumentNoteOn(1, 1.0f, 1.0f);
  int64_t f = 0;
  double l = 0, rr = 0;
  for (int i = 0; i < 200; i++) {
    e.process(f, kBlock); f += kBlock;
    for (int k = 0; k < kBlock; k++) { l += std::fabs(e.outputChannel(0)[k]); rr += std::fabs(e.outputChannel(1)[k]); }
  }
  CHECK(rr > 4.0 * l);   // hard-right pan
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kPb, 1);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 1200; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
    if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, 9);
    if (i == 250) e.setInstrumentParam(1, id(P::Drive), 0.9f);
    if (i == 300) e.instrumentNoteOn(1, 0.7f, 0, 2);
    if (i == 400) e.setInstrumentParam(1, id(P::Resonance), 0.9f);
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
  testPulseWidthShapesSpectrum();
  testWidthExtremesStayValid();
  testPwmModulation();
  testPwmSourceCanBeTheEnvelope();
  testHarmonicCharacter();
  testDrive();
  testFilterAndEnvelope();
  testSub();
  testCharacterIsNotAPlainPulse();
  testDefaultPatchIsPunchyAndLevelled();
  testExtremesStayFiniteAndBounded();
  testParamValidation();
  testScheduledNoteStartsOnItsFrame();
  testRoutesThroughBusAndPan();
  testRenderDoesNotAllocate();
  std::printf("pwmbeast_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
