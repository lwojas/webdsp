#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "../src/dsp/biquad_filter.h"
#include "../src/dsp/character_filter.h"
#include "../src/fx_chain.h"
#include "../src/fx_params.h"
#include "../src/instrument_registry.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the Character Filter primitive (dsp/character_filter.h) and its effect node
// (dsp/character_filter_fx.h): the half-band oversampler, stability under extremes and rapid modulation,
// the four modes, self-oscillation (pitch, level, decay), what makes it more than a clean filter (level-
// dependent harmonics, resonance compression, even harmonics without DC, bass keeping), drive, aliasing,
// the FX node (envelope, stereo, parameters, registration) and a musical validation matrix over the
// engine's own instruments. DSP checks are measurements with margin, never exact samples.
// Set CHARACTER_FILTER_WAV_DIR=<dir> to dump the validation matrix as 16-bit WAVs for listening
// (docs/character-filter.md).
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

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int32_t kBlock = 64;
using Buf = std::vector<float>;

int32_t P(NodeParam p) { return static_cast<int32_t>(p); }

Buf sine(double hz, double amp, int32_t n) {
  Buf b(n);
  for (int32_t i = 0; i < n; i++) b[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / kRate));
  return b;
}
Buf noise(int32_t n, double amp = 0.5, uint32_t seed = 12345) {
  Buf b(n);
  uint32_t s = seed;
  for (int32_t i = 0; i < n; i++) {
    s = s * 1664525u + 1013904223u;
    b[i] = static_cast<float>(amp * ((s >> 8) * (1.0 / 8388608.0) - 1.0));
  }
  return b;
}
Buf saw(double hz, double amp, int32_t n) {
  Buf b(n, 0.0f);
  for (int32_t h = 1; h * hz < 0.45 * kRate; h++)  // band-limited by construction
    for (int32_t i = 0; i < n; i++)
      b[i] += static_cast<float>(amp * (2.0 / kPi) / h * std::sin(2.0 * kPi * h * hz * i / kRate));
  return b;
}

bool finite(const Buf& b) {
  for (float v : b) if (!std::isfinite(v)) return false;
  return true;
}
double peakAbs(const Buf& b, size_t from = 0, size_t to = ~size_t(0)) {
  double m = 0;
  for (size_t i = from; i < std::min(to, b.size()); i++) m = std::max(m, static_cast<double>(std::fabs(b[i])));
  return m;
}
double rms(const Buf& b, size_t from, size_t to) {
  to = std::min(to, b.size());
  double s = 0;
  for (size_t i = from; i < to; i++) s += static_cast<double>(b[i]) * b[i];
  return std::sqrt(s / std::max<size_t>(1, to - from));
}
double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }
double mean(const Buf& b, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; i++) s += b[i];
  return s / static_cast<double>(to - from);
}
// Amplitude of the component at `hz` over [from, to) (single-bin DFT).
double bin(const Buf& b, size_t from, size_t to, double hz) {
  std::complex<double> a = 0;
  for (size_t i = from; i < to; i++) a += static_cast<double>(b[i]) * std::polar(1.0, -2.0 * kPi * hz * i / kRate);
  return 2.0 * std::abs(a) / static_cast<double>(to - from);
}
// Harmonic distortion of a sine at `f0`: everything at 2..8 x f0 against the fundamental, in dB.
double thdDb(const Buf& b, double f0, size_t from, size_t to) {
  double h = 0;
  for (int32_t k = 2; k <= 8; k++) h += std::pow(bin(b, from, to, k * f0), 2);
  return db(std::sqrt(h) / std::max(1e-12, bin(b, from, to, f0)));
}

struct Setup {
  int32_t mode = 0;
  float res = 0.0f, drive = 0.0f, character = 0.4f;
};

CharacterFilter make(const Setup& s) {
  CharacterFilter f;
  f.configure(kRate);
  f.setMode(s.mode);
  f.setResonance(s.res);
  f.setDrive(s.drive);
  f.setCharacter(s.character);
  return f;
}
void run(CharacterFilter& f, Buf& b, float cutoff, int32_t block = kBlock) {
  for (size_t i = 0; i < b.size(); i += block)
    f.process(b.data() + i, static_cast<int32_t>(std::min<size_t>(block, b.size() - i)), cutoff);
}
Buf filtered(const Setup& s, Buf in, float cutoff) {
  CharacterFilter f = make(s);
  run(f, in, cutoff);
  return in;
}
// Steady-state gain in dB of a sine through the filter (second half of one second).
double gainDb(const Setup& s, double hz, double amp, float cutoff) {
  const Buf out = filtered(s, sine(hz, amp, 48000), cutoff);
  return db(bin(out, 24000, 48000, hz) / amp);
}

// --- oversampler ---

void testOversampler() {
  Oversampler2x os;
  // Round trip: a unit impulse comes back after 15 input samples, and a tone passes flat to ~17 kHz.
  Buf out(256);
  for (int32_t i = 0; i < 256; i++) {
    float a, b;
    os.up(i == 10 ? 1.0f : 0.0f, a, b);
    out[i] = os.down(a, b);
  }
  int32_t at = 0;
  for (int32_t i = 0; i < 256; i++) if (std::fabs(out[i]) > std::fabs(out[at])) at = i;
  CHECK(at >= 22 && at <= 26);  // 10 + 15 samples of group delay (the impulse response is two-sided around it)
  for (double hz : {1000.0, 10000.0, 17000.0}) {
    Oversampler2x o;
    Buf x = sine(hz, 1.0, 4800), y(4800);
    for (int32_t i = 0; i < 4800; i++) {
      float a, b;
      o.up(x[i], a, b);
      y[i] = o.down(a, b);
    }
    CHECK_NEAR(db(bin(y, 2400, 4800, hz)), 0.0, 0.15);
  }
  // Above the passband the up-sampler's image is rejected: 10 kHz in, no energy at its 38 kHz mirror.
  Oversampler2x o;
  Buf u(9600);
  const Buf x = sine(10000.0, 1.0, 4800);
  for (int32_t i = 0; i < 4800; i++) o.up(x[i], u[2 * i], u[2 * i + 1]);
  std::complex<double> img = 0;
  for (int32_t i = 4800; i < 9600; i++) img += static_cast<double>(u[i]) * std::polar(1.0, -2.0 * kPi * 38000.0 * i / (2.0 * kRate));
  CHECK(db(2.0 * std::abs(img) / 4800.0) < -80.0);
}

// --- stability ---

void testSilenceAndNonFinite() {
  for (int32_t mode = 0; mode < 4; mode++) {
    for (float res : {0.0f, 0.6f, 1.0f}) {
      Setup s{mode, res, 1.0f, 1.0f};
      Buf z(48000, 0.0f);
      CharacterFilter f = make(s);
      run(f, z, 800.0f);
      CHECK(peakAbs(z) == 0.0);  // a fresh filter with no input stays at exactly zero
    }
  }
  // A burst decays away below the self-oscillation zone, in every mode.
  for (int32_t mode = 0; mode < 4; mode++) {
    Buf b = noise(4800, 0.9);
    b.resize(96000, 0.0f);
    CharacterFilter f = make({mode, 0.7f, 0.5f, 0.5f});
    run(f, b, 1500.0f);
    CHECK(finite(b));
    CHECK(peakAbs(b, 72000) < 1e-4);
  }
  // NaN / inf input is treated as zero and the filter recovers.
  Buf b = sine(300, 0.5, 4800);
  b[1000] = std::nanf("");
  b[2000] = INFINITY;
  b[2001] = -INFINITY;
  CharacterFilter f = make({0, 0.8f, 0.5f, 0.5f});
  run(f, b, 1000.0f);
  CHECK(finite(b));
  CHECK(peakAbs(b, 3000) > 0.05);
  // NaN cutoff does not poison the state.
  Buf c = sine(300, 0.5, 4800);
  f.process(c.data(), 4800, std::nanf(""));
  CHECK(finite(c));
}

void testExtremesStayBoundedAndFinite() {
  int combos = 0;
  for (int32_t mode = 0; mode < 4; mode++)
    for (float res : {0.0f, 0.5f, 1.0f})
      for (float drive : {0.0f, 1.0f})
        for (float chr : {0.0f, 0.5f, 1.0f})
          for (float cutoff : {20.0f, 1000.0f, 18000.0f, 1e9f}) {
            Buf in = noise(9600, 1.0, 7 + combos);
            for (int32_t i = 2400; i < 4800; i++) in[i] = (i / 50) % 2 ? 1.0f : -1.0f;  // a full-scale square burst
            const Buf out = filtered({mode, res, drive, chr}, in, cutoff);
            CHECK(finite(out));
            CHECK(peakAbs(out) <= 1.0001);
            combos++;
          }
  CHECK(combos == 4 * 3 * 2 * 3 * 4);
}

// The cutoff jumps across its whole range at several rates, at full resonance with loud input.
void testRapidCutoffModulation() {
  for (int32_t period : {8, 16, 64, 256}) {
    for (int32_t mode = 0; mode < 4; mode++) {
      for (float chr : {0.0f, 0.6f, 1.0f}) {
        CharacterFilter f = make({mode, 1.0f, 0.5f, chr});
        Buf in = noise(48000, 0.8);
        Buf out(in);
        for (size_t i = 0; i + period <= out.size(); i += period)
          f.process(out.data() + i, period, ((i / period) & 1) ? 30.0f : 16000.0f);
        CHECK(finite(out));
        CHECK(peakAbs(out) <= 1.0001);
      }
    }
  }
  // A smooth full-range sweep leaves no discontinuity: the sample-to-sample step stays small for a
  // slow input (a 100 Hz sine), including at resonance 0.8.
  Buf in = sine(100, 0.3, 96000);
  CharacterFilter f = make({0, 0.8f, 0.2f, 0.4f});
  double maxStep = 0;
  float prev = 0;
  for (size_t i = 0; i < in.size(); i += 16) {
    const double t = static_cast<double>(i) / in.size();
    f.process(in.data() + i, 16, static_cast<float>(60.0 * std::pow(250.0, 0.5 - 0.5 * std::cos(2.0 * kPi * 3.0 * t))));
    for (size_t j = i; j < i + 16; j++) {
      if (i > 4800) maxStep = std::max(maxStep, static_cast<double>(std::fabs(in[j] - prev)));
      prev = in[j];
    }
  }
  CHECK(finite(in));
  CHECK(maxStep < 0.25);
}

// --- response ---

void testModes() {
  // LP 24: -3 dB below the cutoff, ~24 dB/oct above it; the passband is unity.
  CHECK_NEAR(gainDb({0}, 50, 0.05, 1000), 0.0, 0.5);
  CHECK(gainDb({0}, 2000, 0.05, 1000) < -22.0 && gainDb({0}, 2000, 0.05, 1000) > -34.0);
  CHECK(gainDb({0}, 4000, 0.05, 1000) < -42.0);
  // LP 12 falls half as fast.
  const double lp12 = gainDb({1}, 4000, 0.05, 1000) - gainDb({1}, 2000, 0.05, 1000);
  CHECK(lp12 < -9.0 && lp12 > -15.0);
  // HP: stopband below, unity above.
  CHECK(gainDb({3}, 5000, 0.05, 1000) > -1.5);
  CHECK(gainDb({3}, 500, 0.05, 1000) < -15.0);
  CHECK(gainDb({3}, 125, 0.05, 1000) < -40.0);
  // BP: peak near the cutoff (unity-ish), both sides down.
  const double bpc = gainDb({2, 0.0f}, 1000, 0.05, 1000);
  CHECK(bpc > -3.0 && bpc < 3.0);
  CHECK(gainDb({2}, 100, 0.05, 1000) < bpc - 20.0);
  CHECK(gainDb({2}, 10000, 0.05, 1000) < bpc - 20.0);
  // Resonance raises the peak at the cutoff in every mode, monotonically, while the passband stays level.
  for (int32_t mode = 0; mode < 4; mode++) {
    double last = -100;
    for (float r : {0.0f, 0.4f, 0.7f, 0.85f}) {
      const double g = gainDb({mode, r}, 1000, 0.02, 1000);
      CHECK(g > last);
      last = g;
    }
  }
  const double lowGain = gainDb({0, 0.0f}, 60, 0.05, 1000), lowRes = gainDb({0, 0.7f}, 60, 0.05, 1000);
  CHECK(std::fabs(lowRes - lowGain) < 5.0);  // level compensation keeps the passband within a few dB
  CHECK(gainDb({0, 0.7f}, 1000, 0.02, 1000) > lowRes + 5.0);  // while the peak stands out
}

// A cutoff that is not at the very edge: the response follows it over the range, up to 16 kHz.
void testCutoffRange() {
  for (float fc : {250.0f, 1000.0f, 4000.0f, 12000.0f}) {
    const double atFc = gainDb({0, 0.0f}, fc, 0.05, fc);
    CHECK(atFc < -9.0 && atFc > -15.0);  // ~ -12 dB at the cutoff for 4 poles
    CHECK(gainDb({0, 0.0f}, fc * 0.1, 0.05, fc) > -0.7);
  }
}

// --- self-oscillation ---

void testSelfOscillation() {
  for (float chr : {0.0f, 1.0f / 3, 2.0f / 3, 1.0f}) {
    for (float fc : {100.0f, 440.0f, 2000.0f, 6000.0f}) {
      CharacterFilter f = make({0, 1.0f, 0.0f, chr});
      Buf b(96000, 0.0f);
      b[0] = 0.2f;  // a tick starts it
      run(f, b, fc);
      CHECK(finite(b));
      // It sustains at a controlled level...
      const double late = peakAbs(b, 72000);
      CHECK(late > 0.15 && late < 0.9);
      // ...and sits on the cutoff, within ~20 cents.
      int32_t first = -1, last = -1, n = 0;
      for (size_t i = 48001; i < b.size(); i++)
        if (b[i - 1] < 0 && b[i] >= 0) { if (first < 0) first = static_cast<int32_t>(i); last = static_cast<int32_t>(i); n++; }
      const double hz = (n - 1) * kRate / (last - first);
      CHECK(std::fabs(1200.0 * std::log2(hz / fc)) < 25.0);
    }
    // Lowering the resonance lets it die away; below ~0.85 a tick just rings.
    CharacterFilter f = make({0, 1.0f, 0.0f, chr});
    Buf b(96000, 0.0f);
    b[0] = 0.2f;
    run(f, b, 1000.0f);
    f.setResonance(0.7f);
    Buf c(96000, 0.0f);
    run(f, c, 1000.0f);
    CHECK(peakAbs(c, 72000) < 1e-3);
    CharacterFilter g = make({0, 0.8f, 0.0f, chr});
    Buf d(96000, 0.0f);
    d[0] = 0.5f;
    run(g, d, 1000.0f);
    CHECK(peakAbs(d, 0, 500) > 0.01);   // rings...
    CHECK(peakAbs(d, 72000) < 1e-3);    // ...and ends
  }
  // Sweeping a self-oscillating filter glides in pitch and stays bounded and smooth.
  CharacterFilter f = make({0, 1.0f, 0.0f, 0.5f});
  Buf b(96000, 0.0f);
  b[0] = 0.2f;
  for (size_t i = 0; i < b.size(); i += 16)
    f.process(b.data() + i, 16, static_cast<float>(100.0 * std::pow(40.0, static_cast<double>(i) / b.size())));
  CHECK(finite(b));
  CHECK(peakAbs(b, 4800) > 0.1 && peakAbs(b) < 0.95);
}

// --- character: why this is not a clean filter with a distortion on it ---

// A plain 24 dB Butterworth low-pass (the repo's biquad) for comparison.
Buf plainLowPass(Buf in, float cutoff) {
  BiquadFilter bq(kRate);
  bq.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), cutoff);
  bq.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 24.0f);
  bq.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), 0.7071f);
  for (size_t i = 0; i < in.size(); i += kBlock) {
    float* ch[1] = {in.data() + i};
    bq.process(ch, 1, static_cast<int32_t>(std::min<size_t>(kBlock, in.size() - i)));
  }
  return in;
}

void testDistinctFromCleanFilter() {
  // At default drive and a mid character, a -6 dB sine through an open filter already comes out
  // coloured, where the plain biquad leaves it untouched.
  const Buf in = sine(200, 0.5, 48000);
  const double plain = thdDb(plainLowPass(in, 3000), 200, 24000, 48000);
  const double ours = thdDb(filtered({0, 0.2f, 0.0f, 0.4f}, in, 3000), 200, 24000, 48000);
  CHECK(plain < -90.0);
  CHECK(ours > -40.0 && ours < -15.0);  // audible colour, not distortion
  // The colour follows the level: 30 dB lower, the same filter is nearly linear again.
  const Buf quiet = sine(200, 0.016, 48000);
  const double quietThd = thdDb(filtered({0, 0.2f, 0.0f, 0.4f}, quiet, 3000), 200, 24000, 48000);
  CHECK(quietThd < ours - 18.0);
  // Clean really is clean.
  CHECK(thdDb(filtered({0, 0.2f, 0.0f, 0.0f}, in, 3000), 200, 24000, 48000) < -48.0);
}

void testResonanceCompression() {
  // A loud input pulls the resonant peak down; a quiet one lets it stand (per unit of input).
  for (float chr : {0.4f, 0.7f, 1.0f}) {
    const double quiet = gainDb({0, 0.8f, 0.0f, chr}, 1000, 0.01, 1000);
    const double loud = gainDb({0, 0.8f, 0.0f, chr}, 1000, 0.3, 1000);
    CHECK(loud < quiet - 3.0);
  }
}

void testEvenHarmonicsWithoutDc() {
  const Buf in = sine(200, 0.4, 48000);
  const Buf clean = filtered({0, 0.0f, 0.3f, 0.0f}, in, 4000);
  const Buf warm = filtered({0, 0.0f, 0.3f, 1.0f / 3}, in, 4000);
  const double h2Clean = db(bin(clean, 24000, 48000, 400) / bin(clean, 24000, 48000, 200));
  const double h2Warm = db(bin(warm, 24000, 48000, 400) / bin(warm, 24000, 48000, 200));
  CHECK(h2Warm > h2Clean + 40.0);  // second harmonic appears
  CHECK(h2Warm > -40.0);
  for (float chr : {0.0f, 0.33f, 0.66f, 1.0f}) {
    const Buf o = filtered({0, 0.4f, 0.8f, chr}, in, 4000);
    CHECK(std::fabs(mean(o, 24000, 48000)) < 2e-3);  // asymmetry adds no DC
  }
}

void testDrive() {
  // More drive, more harmonics (monotonic), while the level stays within a few dB.
  const Buf in = saw(110, 0.5, 48000);
  double lastThd = -200, minL = 1e9, maxL = -1e9;
  for (float chr : {0.0f, 0.4f, 0.7f, 1.0f}) {
    minL = 1e9;
    maxL = -1e9;
    for (float drive : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      const Buf o = filtered({0, 0.3f, drive, chr}, in, 1500);
      const double l = db(rms(o, 24000, 48000));
      minL = std::min(minL, l);
      maxL = std::max(maxL, l);
    }
    CHECK(maxL - minL < 4.5);
  }
  const Buf s = sine(150, 0.2, 48000);
  for (float drive : {0.0f, 0.3f, 0.6f, 1.0f}) {
    const double t = thdDb(filtered({0, 0.2f, drive, 0.6f}, s, 6000), 150, 24000, 48000);
    CHECK(t > lastThd);
    lastThd = t;
  }
  CHECK(lastThd > -15.0);  // full drive is properly saturated
  // Drive 0 has a different feel from drive 1 even at the same character: the nonlinearity lives inside.
  CHECK(std::fabs(gainDb({0, 0.2f, 1.0f, 0.6f}, 150, 0.2, 6000)) < 8.0);
}

void testBassKeeping() {
  // With resonance up, Clean loses low end like a classic ladder; Punchy keeps most of it.
  const double cleanRes = gainDb({0, 0.7f, 0.0f, 0.0f}, 60, 0.05, 1000) - gainDb({0, 0.0f, 0.0f, 0.0f}, 60, 0.05, 1000);
  const double punchyRes = gainDb({0, 0.7f, 0.0f, 2.0f / 3}, 60, 0.05, 1000) - gainDb({0, 0.0f, 0.0f, 2.0f / 3}, 60, 0.05, 1000);
  CHECK(punchyRes > cleanRes + 1.0);
  CHECK(punchyRes > -2.0);
}

// A high-pitched sine at 5 kHz, hard driven: nothing may appear at the 13 kHz alias of its 7th harmonic.
void testAliasing() {
  const Buf in = sine(5000, 0.6, 48000);
  const Buf o = filtered({0, 0.0f, 1.0f, 1.0f}, in, 18000);
  const double fund = bin(o, 24000, 48000, 5000);
  CHECK(db(bin(o, 24000, 48000, 13000) / fund) < -45.0);
  CHECK(db(bin(o, 24000, 48000, 7000) / fund) < -30.0);  // 9th harmonic folds to 3 kHz; 7 kHz is the 8th-order image of 35 kHz
}

void testModeSwitchIsSmooth() {
  CharacterFilter f = make({0, 0.4f, 0.3f, 0.5f});
  Buf in = sine(220, 0.4, 24000);
  double maxStep = 0;
  float prev = 0;
  for (size_t i = 0; i < in.size(); i += 32) {
    if (i == 12000) f.setMode(3);
    if (i == 16000) f.setMode(2);
    if (i == 20000) f.setMode(0);
    f.process(in.data() + i, 32, 600.0f);
    for (size_t j = i; j < i + 32; j++) { maxStep = std::max(maxStep, static_cast<double>(std::fabs(in[j] - prev))); prev = in[j]; }
  }
  CHECK(finite(in));
  CHECK(maxStep < 0.12);  // no click: a 220 Hz tone at 0.4 moves at most ~0.01 per sample
}

void testDeterminismAndBlockSize() {
  const Buf in = noise(24000, 0.5);
  const Setup s{0, 0.6f, 0.4f, 0.5f};
  Buf a = in, b = in, c = in;
  CharacterFilter f1 = make(s), f2 = make(s), f3 = make(s);
  run(f1, a, 900.0f, 64);
  run(f2, b, 900.0f, 64);
  run(f3, c, 900.0f, 7);
  for (size_t i = 0; i < in.size(); i++) CHECK(a[i] == b[i]);
  double diff = 0;
  for (size_t i = 0; i < in.size(); i++) diff = std::max(diff, static_cast<double>(std::fabs(a[i] - c[i])));
  CHECK(diff < 1e-4);  // block size is not audible
  f1.reset();
  Buf d = in;
  run(f1, d, 900.0f, 64);
  for (size_t i = 0; i < in.size(); i++) CHECK(a[i] == d[i]);  // reset restores the exact start
}

void testTracking() {
  CHECK_NEAR(CharacterFilter::trackedCutoff(1000.0f, 261.6256f, 1.0f), 1000.0, 0.01);
  CHECK_NEAR(CharacterFilter::trackedCutoff(1000.0f, 523.2511f, 1.0f), 2000.0, 0.1);
  CHECK_NEAR(CharacterFilter::trackedCutoff(1000.0f, 523.2511f, 0.5f), 1414.2, 0.5);
  CHECK_NEAR(CharacterFilter::trackedCutoff(1000.0f, 523.2511f, 0.0f), 1000.0, 0.01);
  CHECK_NEAR(CharacterFilter::trackedCutoff(1000.0f, 0.0f, 1.0f), 1000.0, 0.01);
  CHECK_NEAR(CharacterFilter::trackedCutoff(1000.0f, std::nanf(""), 1.0f), 1000.0, 0.01);
}

void testNoAllocation() {
  CharacterFilter f = make({0, 0.9f, 0.5f, 0.5f});
  Buf b = noise(4800);
  const long before = g_allocs;
  run(f, b, 1000.0f);
  f.setMode(2);
  f.setResonance(0.3f);
  run(f, b, 3000.0f);
  CHECK(g_allocs == before);
}

// --- the effect node ---

struct Fx {
  FxChain chain;
  Fx() {
    chain.configure(kRate);
    chain.add(1, static_cast<int32_t>(FxKind::CharacterFilter), -1);
  }
  void set(NodeParam p, float v) { chain.setParam(1, P(p), v); }
  void run(std::vector<Buf>& ch) {
    for (size_t i = 0; i < ch[0].size(); i += kBlock) {
      float* p[2] = {ch[0].data() + i, ch.size() > 1 ? ch[1].data() + i : nullptr};
      chain.process(p, static_cast<int32_t>(ch.size()), static_cast<int32_t>(std::min<size_t>(kBlock, ch[0].size() - i)));
    }
  }
};

void testFxRegistration() {
  const int32_t kind = static_cast<int32_t>(FxKind::CharacterFilter);
  CHECK(kind == 13);
  CHECK(fxParamCount(kind) == 6);
  CHECK(fxParamInfo(kind, 0)->id == P(NodeParam::CharFilterMode));
  CHECK(fxParamInfo(kind, 5)->id == P(NodeParam::CharFilterEnvAmount));
  CHECK(std::strcmp(fxParamInfo(kind, 4)->name, "character") == 0);
  CHECK(fxKindForNodeParam(P(NodeParam::CharFilterMode)) == kind);
  CHECK(fxKindForNodeParam(P(NodeParam::CharFilterEnvAmount)) == kind);
  CHECK(resolveModDomain(*fxParamInfo(kind, 0)) == ModDomain::None);   // mode is a step
  CHECK(resolveModDomain(*fxParamInfo(kind, 1)) == ModDomain::Log);    // cutoff in octaves
  CHECK(resolveModDomain(*fxParamInfo(kind, 2)) == ModDomain::Linear);
}

void testFxParameters() {
  Fx fx;
  // Non-finite and out-of-range values are safe; everything stays finite and bounded.
  fx.set(NodeParam::CharFilterCutoff, std::nanf(""));
  fx.set(NodeParam::CharFilterResonance, 9.0f);
  fx.set(NodeParam::CharFilterDrive, -3.0f);
  fx.set(NodeParam::CharFilterCharacter, INFINITY);
  fx.set(NodeParam::CharFilterMode, 99.0f);
  fx.set(NodeParam::CharFilterCutoff, 1e9f);
  fx.set(NodeParam::CharFilterEnvAmount, 5.0f);
  std::vector<Buf> ch = {noise(24000, 0.9), noise(24000, 0.9, 99)};
  fx.run(ch);
  CHECK(finite(ch[0]) && finite(ch[1]));
  CHECK(peakAbs(ch[0]) <= 1.0001 && peakAbs(ch[1]) <= 1.0001);
}

void testFxStereo() {
  Fx fx;
  fx.set(NodeParam::CharFilterCutoff, 1000.0f);
  fx.set(NodeParam::CharFilterResonance, 0.5f);
  std::vector<Buf> ch = {sine(300, 0.4, 9600), Buf(9600, 0.0f)};
  fx.run(ch);
  CHECK(peakAbs(ch[0], 4800) > 0.1);
  CHECK(peakAbs(ch[1]) == 0.0);  // channels are independent
  // Mono works.
  Fx mono;
  std::vector<Buf> m = {sine(300, 0.4, 9600)};
  mono.run(m);
  CHECK(finite(m[0]) && peakAbs(m[0], 4800) > 0.1);
}

void testFxEnvelope() {
  // A decaying burst of broadband noise through a filter at 1.5 kHz. With a positive Env the filter
  // opens on the burst (far more energy above 3 kHz); with a negative Env it closes on it (less energy
  // around the cutoff); and the movement follows the sound, so late in the tail the opening has gone.
  auto band = [](float envAmount, size_t from, size_t to, double lo, double hi) {
    Fx fx;
    fx.set(NodeParam::CharFilterCutoff, 1500.0f);
    fx.set(NodeParam::CharFilterResonance, 0.2f);
    fx.set(NodeParam::CharFilterEnvAmount, envAmount);
    Buf n = noise(48000, 1.0);
    for (size_t i = 0; i < n.size(); i++) n[i] *= static_cast<float>(std::exp(-static_cast<double>(i) / (0.08 * kRate)));
    std::vector<Buf> ch = {n};
    fx.run(ch);
    double e = 0;
    for (double hz = lo; hz < hi; hz += 250) e += std::pow(bin(ch[0], from, to, hz), 2);
    return 10.0 * std::log10(e + 1e-20);
  };
  const double flatHi = band(0.0f, 480, 2400, 3000, 10000);
  const double upHi = band(1.0f, 480, 2400, 3000, 10000);
  CHECK(upHi > flatHi + 20.0);
  const double flatMid = band(0.0f, 480, 2400, 300, 2000);
  const double downMid = band(-1.0f, 480, 2400, 300, 2000);
  CHECK(downMid < flatMid - 6.0);
  // Late in the tail the follower has let go: the brightness relative to the early burst is gone.
  CHECK(band(1.0f, 24000, 28800, 3000, 10000) - band(1.0f, 24000, 28800, 100, 800) < upHi - band(1.0f, 480, 2400, 100, 800) - 15.0);
}

// --- musical validation matrix ---

void writeWav(const std::string& name, const Buf& b) {
  const char* dir = std::getenv("CHARACTER_FILTER_WAV_DIR");
  if (!dir) return;
  FILE* f = std::fopen((std::string(dir) + "/" + name + ".wav").c_str(), "wb");
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

// One engine instrument playing one note (or hit) for `seconds`, mono.
Buf instrumentNote(InstrumentKind kind, float tuneNote, double seconds, double releaseAt) {
  std::unique_ptr<Instrument> inst = makeInstrument(static_cast<int32_t>(kind));
  inst->configure(kRate);
  inst->noteOn(0.9f, 0, 0.0f, tuneNote);
  Buf out;
  const int32_t blocks = static_cast<int32_t>(seconds * kRate / 128);
  float l[128], r[128], s0[128], s1[128];
  for (int32_t b = 0; b < blocks; b++) {
    if (releaseAt > 0 && b == static_cast<int32_t>(releaseAt * kRate / 128)) inst->noteOff();
    std::memset(l, 0, sizeof l);
    std::memset(r, 0, sizeof r);
    float* bus[2] = {l, r};
    float* scratch[2] = {s0, s1};
    inst->render(bus, 2, 128, scratch);
    out.insert(out.end(), l, l + 128);
  }
  return out;
}

Buf pluck(double hz, double seconds) {  // Karplus-Strong: a stand-in for a recorded, acoustic-ish sample
  const int32_t n = static_cast<int32_t>(seconds * kRate), len = static_cast<int32_t>(kRate / hz);
  Buf line = noise(len, 0.8, 4242), out(n);
  for (int32_t i = 0; i < n; i++) {
    const int32_t j = i % len;
    out[i] = line[j];
    line[j] = 0.5f * (line[j] + line[(j + 1) % len]) * 0.998f;
  }
  return out;
}

struct Source {
  const char* name;
  Buf audio;
};

struct Corner {
  const char* name;
  float res, drive;
};

void testValidationMatrix() {
  std::vector<Source> sources;
  sources.push_back({"osc", saw(220, 0.5, 48000)});
  sources.push_back({"bass", instrumentNote(InstrumentKind::Bass, 0, 1.0, 0.7)});
  sources.push_back({"kick", instrumentNote(InstrumentKind::Kick, 0, 1.0, 0)});
  sources.push_back({"snare", instrumentNote(InstrumentKind::Snare, 0, 1.0, 0)});
  sources.push_back({"hat", instrumentNote(InstrumentKind::HiHat, 0, 1.0, 0)});
  sources.push_back({"fm", instrumentNote(InstrumentKind::Fm, 0, 1.0, 0.7)});
  sources.push_back({"sample", pluck(196, 1.0)});
  sources.push_back({"noise", instrumentNote(InstrumentKind::Noise, 0, 1.0, 0.7)});
  const Corner corners[] = {{"lo-res_lo-drive", 0.2f, 0.1f},
                            {"hi-res_lo-drive", 0.85f, 0.1f},
                            {"lo-res_hi-drive", 0.2f, 0.85f},
                            {"hi-res_hi-drive", 0.85f, 0.85f}};
  std::printf("  %-7s %-16s %8s %8s %8s %9s %9s\n", "source", "setting", "in dB", "out dB", "peak", "low<150Hz", "air>4kHz");
  for (const Source& src : sources) {
    CHECK(finite(src.audio));
    const double inRms = rms(src.audio, 0, src.audio.size());
    CHECK(inRms > 1e-4);  // the engine's instrument actually produced sound
    for (const Corner& c : corners) {
      for (int32_t mode : {0, 3}) {  // the low-pass voice, and the high-pass as a smoke check
        Fx fx;
        fx.set(NodeParam::CharFilterMode, static_cast<float>(mode));
        fx.set(NodeParam::CharFilterCutoff, mode == 0 ? 1400.0f : 400.0f);
        fx.set(NodeParam::CharFilterResonance, c.res);
        fx.set(NodeParam::CharFilterDrive, c.drive);
        fx.set(NodeParam::CharFilterCharacter, 0.55f);
        std::vector<Buf> ch = {src.audio};
        fx.run(ch);
        const Buf& o = ch[0];
        CHECK(finite(o));
        CHECK(peakAbs(o) <= 1.0001);
        CHECK(rms(o, 0, o.size()) > 1e-5);
        if (mode == 0) {
          double lo = 0, air = 0;
          for (double hz = 40; hz < 150; hz += 10) lo += std::pow(bin(o, 0, o.size(), hz), 2);
          for (double hz = 4000; hz < 12000; hz += 500) air += std::pow(bin(o, 0, o.size(), hz), 2);
          std::printf("  %-7s %-16s %8.1f %8.1f %8.2f %9.1f %9.1f\n", src.name, c.name, db(inRms), db(rms(o, 0, o.size())),
                      peakAbs(o), 10 * std::log10(lo + 1e-20), 10 * std::log10(air + 1e-20));
          // The result is level-sane: neither vanishing nor blown up relative to the source.
          CHECK(db(rms(o, 0, o.size())) > db(inRms) - 45.0);
          CHECK(db(rms(o, 0, o.size())) < db(inRms) + 18.0);
        }
        writeWav(std::string(src.name) + "_" + (mode == 0 ? "lp_" : "hp_") + c.name, o);
      }
    }
    // Rapid cutoff modulation on every source (LP, resonance high): an 8 Hz-ish square sweep plus a
    // 40 Hz triangle; must stay finite and bounded.
    for (const char* shape : {"sweep", "fast"}) {
      Fx fx;
      fx.set(NodeParam::CharFilterResonance, 0.85f);
      fx.set(NodeParam::CharFilterDrive, 0.4f);
      std::vector<Buf> ch = {src.audio};
      Buf& x = ch[0];
      for (size_t i = 0; i + kBlock <= x.size(); i += kBlock) {
        const double t = static_cast<double>(i) / kRate;
        const double u = std::strcmp(shape, "sweep") == 0 ? 0.5 + 0.5 * std::sin(2 * kPi * 3 * t)
                                                          : 0.5 + 0.5 * std::sin(2 * kPi * 60 * t);
        fx.set(NodeParam::CharFilterCutoff, static_cast<float>(150.0 * std::pow(60.0, u)));
        float* p[1] = {x.data() + i};
        fx.chain.process(p, 1, kBlock);
      }
      CHECK(finite(x));
      CHECK(peakAbs(x) <= 1.0001);
      writeWav(std::string(src.name) + "_mod_" + shape, x);
    }
  }
}

}  // namespace

int main() {
  testOversampler();
  testSilenceAndNonFinite();
  testExtremesStayBoundedAndFinite();
  testRapidCutoffModulation();
  testModes();
  testCutoffRange();
  testSelfOscillation();
  testDistinctFromCleanFilter();
  testResonanceCompression();
  testEvenHarmonicsWithoutDc();
  testDrive();
  testBassKeeping();
  testAliasing();
  testModeSwitchIsSmooth();
  testDeterminismAndBlockSize();
  testTracking();
  testNoAllocation();
  testFxRegistration();
  testFxParameters();
  testFxStereo();
  testFxEnvelope();
  testValidationMatrix();
  std::printf("character_filter_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
