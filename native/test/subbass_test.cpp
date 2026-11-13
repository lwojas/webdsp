#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the Sub Bass instrument (instruments/subbass.h) and its primitives (dsp/harmonic_osc.h, dsp/adaa_tanh.h):
// registration and table, the harmonic spectrum (shape 0 is a pure sine, harmonics open in order, band limit),
// spectral cleanliness at 30..440 Hz (THD, inharmonic residual = aliasing/noise floor, DC), clicks at note
// start / stop / retrigger, aliasing under Shape and Drive, what each control does, stability under extreme and
// randomised parameters, loudness, other sample rates, and zero allocation in the audio path.
// The instrument is driven directly for DSP measurements (no master limiter in the way) and through the Engine for
// registration, routing and allocation. Set SUBBASS_WAV_DIR=<dir> to dump the presets as WAVs for listening.
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
constexpr int32_t kSb = static_cast<int32_t>(InstrumentKind::SubBass);
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kFft = 65536;

using Buf = std::vector<float>;
using P = SubBassParam;
int32_t id(P p) { return static_cast<int32_t>(p); }
size_t at(double s, double rate = kRate) { return static_cast<size_t>(s * rate); }

using Params = std::map<P, float>;

// Renders one note straight out of the instrument: `seconds` long, note-off at `offAt` (negative = held).
Buf renderNote(const Params& params, double seconds, double offAt = -1.0, float pitch = 0.0f, float vel = 1.0f,
               double rate = kRate) {
  SubBassInstrument inst;
  inst.configure(rate);
  for (const auto& kv : params) inst.setParam(id(kv.first), kv.second);
  inst.noteOn(vel, 0, 0.0f, pitch);
  Buf out;
  const size_t total = at(seconds, rate);
  const size_t off = offAt >= 0.0 ? at(offAt, rate) : total + 1;
  float l[kBlock], r[kBlock], s0[kBlock], s1[kBlock];
  float* bus[2] = {l, r};
  float* scratch[2] = {s0, s1};
  size_t done = 0;
  bool offSent = false;
  while (done < total) {
    if (!offSent && done >= off) { inst.noteOff(); offSent = true; }
    std::memset(l, 0, sizeof(l));
    std::memset(r, 0, sizeof(r));
    inst.render(bus, 2, kBlock, scratch);
    out.insert(out.end(), l, l + kBlock);
    done += kBlock;
  }
  return out;
}

// Frequency (Hz) that lands exactly on FFT bin k of kFft samples at 48 kHz, as a float-exact value.
double binHz(int k) { return k * kRate / static_cast<double>(kFft); }

void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0);
      for (size_t j = 0; j < len / 2; j++) {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Power spectrum (Hann) of kFft samples starting at `from`; p[k] is the power in bin k.
std::vector<double> powerSpectrum(const Buf& b, size_t from) {
  std::vector<std::complex<double>> a(kFft);
  for (size_t i = 0; i < kFft; i++) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kFft));
    a[i] = w * b[from + i];
  }
  fft(a);
  std::vector<double> p(kFft / 2);
  for (size_t k = 0; k < p.size(); k++) p[k] = std::norm(a[k]);
  return p;
}

struct Clean {
  double fundDb = 0;      // fundamental power, dB
  double harmDb = 0;      // everything within +-3 bins of 2f..Nf, relative to the fundamental
  double residualDb = 0;  // everything else (aliasing, noise), relative to the fundamental
};

// Splits a bin-centred periodic signal into fundamental / harmonics / the rest.
Clean cleanliness(const Buf& b, int k0, size_t from) {
  const auto p = powerSpectrum(b, from);
  std::vector<char> harm(p.size(), 0), fund(p.size(), 0);
  for (size_t k = 0; k < p.size(); k++) {
    const double nn = std::round(static_cast<double>(k) / k0);
    if (nn < 1.0) { if (k > 3) {} }
    if (nn >= 1.0 && std::fabs(static_cast<double>(k) - nn * k0) <= 3.0) (nn == 1.0 ? fund : harm)[k] = 1;
  }
  double f = 0, h = 0, r = 0;
  for (size_t k = 4; k < p.size(); k++) (fund[k] ? f : harm[k] ? h : r) += p[k];
  Clean c;
  c.fundDb = 10.0 * std::log10(std::max(f, 1e-30));
  c.harmDb = 10.0 * std::log10(std::max(h, 1e-30) / f);
  c.residualDb = 10.0 * std::log10(std::max(r, 1e-30) / f);
  return c;
}

double mean(const Buf& b, size_t from, size_t n) {
  double s = 0;
  for (size_t i = from; i < from + n; i++) s += b[i];
  return s / static_cast<double>(n);
}
double rms(const Buf& b, size_t from, size_t n) {
  double s = 0;
  for (size_t i = from; i < from + n; i++) s += static_cast<double>(b[i]) * b[i];
  return std::sqrt(s / static_cast<double>(n));
}
double peak(const Buf& b, size_t from, size_t n) {
  double m = 0;
  for (size_t i = from; i < from + n; i++) m = std::max(m, static_cast<double>(std::fabs(b[i])));
  return m;
}
double maxStep(const Buf& b, size_t from, size_t to) {
  double m = 0;
  for (size_t i = std::max<size_t>(from, 1); i < to && i < b.size(); i++) m = std::max(m, static_cast<double>(std::fabs(b[i] - b[i - 1])));
  return m;
}
bool finite(const Buf& b) {
  for (float v : b) if (!std::isfinite(v)) return false;
  return true;
}
double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// Band power (dB) over [lo, hi) Hz of a steady stretch.
double bandDb(const Buf& b, size_t from, double lo, double hi) {
  const auto p = powerSpectrum(b, from);
  double s = 0;
  for (size_t k = 1; k < p.size(); k++) {
    const double hz = k * kRate / kFft;
    if (hz >= lo && hz < hi) s += p[k];
  }
  return 10.0 * std::log10(std::max(s, 1e-30));
}

// A steady note locked to a bin: root = binHz(k), held, analysed after the transients have died.
Buf steady(Params p, int k, double seconds = 2.2) {
  p[P::Root] = static_cast<float>(binHz(k));
  return renderNote(p, seconds);
}
constexpr size_t kSettle = 24000;  // analysis starts here: 0.5 s

// ---------------------------------------------------------------------------------------------------------------

void testHarmonicSpectrum() {
  float a[kMaxHarmonics + 1];
  for (int32_t pr = 0; pr < kHarmonicProfileCount; pr++) {
    int32_t top = harmonicSpectrum(pr, 0.0f, 1e9f, a);
    CHECK(top == 1);
    CHECK(a[1] == 1.0f);
    bool zero = true;
    for (int32_t n = 2; n <= kMaxHarmonics; n++) zero = zero && a[n] == 0.0f;
    CHECK(zero);  // shape 0: exactly a sine, in every profile

    // Harmonic count and total brightness grow monotonically with shape.
    int32_t prevTop = 1;
    double prevEnergy = 0.0;
    for (int32_t q = 1; q <= 20; q++) {
      top = harmonicSpectrum(pr, q / 20.0f, 1e9f, a);
      double hi = 0;
      for (int32_t n = 2; n <= kMaxHarmonics; n++) hi += static_cast<double>(a[n]) * a[n];
      CHECK(top >= prevTop);
      CHECK(hi >= prevEnergy - 1e-9);
      double all = static_cast<double>(a[1]) * a[1] + hi;
      CHECK_NEAR(all, 1.0, 1e-5);  // RMS normalised
      prevTop = top;
      prevEnergy = hi;
    }
  }
  // Harmonics arrive in order: at a low shape the 2nd is present before the 4th.
  harmonicSpectrum(0, 0.25f, 1e9f, a);
  CHECK(a[2] > 0.0f && a[2] > a[3] && a[4] == 0.0f);
  // Classic reaches a 1/n saw at 1; Hollow has no even harmonics; Warm stops at 12.
  harmonicSpectrum(4, 1.0f, 1e9f, a);
  CHECK_NEAR(a[2] / a[1], 0.5, 1e-4);
  CHECK_NEAR(a[10] / a[1], 0.1, 1e-4);
  harmonicSpectrum(2, 1.0f, 1e9f, a);
  CHECK(a[2] == 0.0f && a[4] == 0.0f && a[3] > 0.0f);
  CHECK(harmonicSpectrum(1, 1.0f, 1e9f, a) <= 12);
  // The band limit removes harmonics smoothly (a fractional limit gives a fractional top harmonic).
  CHECK(harmonicSpectrum(4, 1.0f, 10.0f, a) == 10);
  harmonicSpectrum(4, 1.0f, 9.5f, a);
  CHECK(a[10] > 0.0f && a[10] < a[9] * 9.0f / 10.0f * 0.6f);
  CHECK(harmonicSpectrum(4, 1.0f, 1.0f, a) == 1);
  // Mirror block: the same values test/subBass.test.ts pins for the client-side waveform (src/runtime/subBass.ts).
  {
    const float want0[] = {0.915728f, 0.323759f, 0.176232f, 0.114466f, 0.081905f, 0.062307f, 0.043264f, 0.0f};
    harmonicSpectrum(0, 0.5f, 1e9f, a);
    for (int n = 1; n <= 8; n++) CHECK_NEAR(a[n], want0[n - 1], 2e-6);
    const float want1[] = {0.957773f, 0.287527f, 0.0f, 0.0f};
    harmonicSpectrum(1, 0.3f, 1e9f, a);
    for (int n = 1; n <= 4; n++) CHECK_NEAR(a[n], want1[n - 1], 2e-6);
    const float want3[] = {0.693326f, 0.415121f, 0.307516f, 0.248550f, 0.210717f, 0.184122f, 0.164272f, 0.148816f};
    harmonicSpectrum(3, 0.8f, 12.5f, a);
    for (int n = 1; n <= 8; n++) CHECK_NEAR(a[n], want3[n - 1], 2e-6);
  }
  // Out-of-range and NaN inputs are safe.
  CHECK(harmonicSpectrum(99, 5.0f, 1e9f, a) >= 1);
  CHECK(harmonicSpectrum(-3, std::nanf(""), 1e9f, a) == 1);
}

void testOscillatorBandLimit() {
  // The primitive on its own, at high pitch and full shape, in a bin-centred setup: everything outside the
  // harmonics is below -100 dB (a naive saw would be at about -25 dB).
  for (int k : {601, 2003, 4001}) {  // ~440 Hz, ~1.47 kHz, ~2.9 kHz
    HarmonicOsc o;
    const double inc = static_cast<double>(k) / static_cast<double>(kFft);
    o.setSpectrum(4, 1.0f, inc);
    Buf b(kFft * 2);
    for (float& v : b) v = o.next(inc);
    const Clean c = cleanliness(b, k, kFft / 2);
    CHECK(c.residualDb < -100.0);
    std::printf("  osc k=%d  harmonics=%d  residual %.1f dB\n", k, o.harmonics(), c.residualDb);
  }
}

void testPureSineAtLowFrequencies() {
  // Shape 0, Drive 0, filter open: a sine with nothing else in it, from 30 Hz to 440 Hz.
  struct Pt { int k; double hz; };
  for (Pt pt : {Pt{41, 30.0}, Pt{55, 40.0}, Pt{75, 55.0}, Pt{109, 80.0}, Pt{150, 110.0}, Pt{301, 220.0}, Pt{602, 440.0}}) {
    const Buf b = steady({}, pt.k);
    CHECK(finite(b));
    const Clean c = cleanliness(b, pt.k, kSettle);
    CHECK_NEAR(binHz(pt.k), pt.hz, 1.0);
    // The fundamental is at the right place, and everything else is below -70 dB.
    CHECK(c.harmDb < -70.0);
    CHECK(c.residualDb < -80.0);
    CHECK_NEAR(mean(b, kSettle, kFft), 0.0, 1e-5);
    // Level 0.85 x 0.6 peak.
    CHECK_NEAR(peak(b, kSettle, kFft), 0.85 * 0.6, 0.005);
    std::printf("  sine %5.1f Hz: harm %.1f dB, residual %.1f dB, peak %.3f\n", pt.hz, c.harmDb, c.residualDb, peak(b, kSettle, kFft));
  }
}

void testShapeAddsHarmonics() {
  const int k = 75;  // ~55 Hz
  double prev = -200.0;
  for (float sh : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const Buf b = steady({{P::Shape, sh}}, k);
    const Clean c = cleanliness(b, k, kSettle);
    CHECK(c.harmDb > prev);
    prev = c.harmDb;
    CHECK(c.residualDb < -85.0);  // harmonic, not noisy: nothing between the harmonics
    CHECK_NEAR(mean(b, kSettle, kFft), 0.0, 1e-5);
    std::printf("  shape %.2f: harmonics %.1f dB re fundamental, residual %.1f dB\n", sh, c.harmDb, c.residualDb);
  }
  // 2nd harmonic arrives before the 4th; at the "strong 2nd/3rd" middle the 2nd and 3rd are large.
  const Buf mid = steady({{P::Shape, 0.5f}}, k);
  const auto p = powerSpectrum(mid, kSettle);
  auto bin = [&](int n) { return 10.0 * std::log10(p[n * k] + 1e-30) - 10.0 * std::log10(p[k]); };
  CHECK(bin(2) > -12.0 && bin(2) < 0.0);
  CHECK(bin(3) > -18.0);
  CHECK(bin(2) > bin(5));
  // Profiles differ: Hollow has no even harmonics, Warm is darker than Saw at the same shape.
  const auto ph = powerSpectrum(steady({{P::Shape, 1.0f}, {P::Profile, 2.0f}}, k), kSettle);
  CHECK(10.0 * std::log10(ph[2 * k] / ph[k]) < -90.0 && 10.0 * std::log10(ph[3 * k] / ph[k]) > -20.0);
  const double warm = bandDb(steady({{P::Shape, 1.0f}, {P::Profile, 1.0f}}, k), kSettle, 1000.0, 20000.0);
  const double saw = bandDb(steady({{P::Shape, 1.0f}, {P::Profile, 4.0f}}, k), kSettle, 1000.0, 20000.0);
  CHECK(warm < saw - 20.0);
}

void testDriveIsNotShape() {
  const int k = 75;
  // Drive 0: bit-exact bypass of the stage; Drive up adds harmonics to the sine and they grow.
  double prev = -200.0;
  for (float d : {0.0f, 0.1f, 0.3f, 0.6f, 1.0f}) {
    const Buf b = steady({{P::Drive, d}}, k);
    const Clean c = cleanliness(b, k, kSettle);
    CHECK(finite(b));
    if (d == 0.0f) CHECK(c.harmDb < -70.0);
    CHECK(c.harmDb > prev - 0.5);
    prev = c.harmDb;
    CHECK_NEAR(mean(b, kSettle, kFft), 0.0, 1e-4);
    std::printf("  drive %.1f: harmonics %.1f dB, residual %.1f dB, rms %.3f\n", d, c.harmDb, c.residualDb, rms(b, kSettle, kFft));
  }
  // Drive on a sine adds only odd harmonics (symmetric curve): 3rd >> 2nd.
  const auto p = powerSpectrum(steady({{P::Drive, 0.6f}}, k), kSettle);
  CHECK(10.0 * std::log10(p[3 * k] / (p[2 * k] + 1e-30)) > 40.0);
}

void testAliasing() {
  // The worst case: full shape and full drive at pitches where the harmonic series runs into Nyquist.
  // Residual = energy that is not at a harmonic of the note (what aliasing becomes).
  // Root tops out at 880 Hz, so the highest pitches are Root + a note offset (exact octaves keep the bin lock).
  struct Pt { int kRoot; float semis; int k; double hz; };
  for (Pt pt : {Pt{75, 0.0f, 75, 55.0}, Pt{301, 0.0f, 301, 220.0}, Pt{602, 0.0f, 602, 440.0}, Pt{602, 12.0f, 1204, 880.0},
                Pt{600, 24.0f, 2400, 1758.0}}) {
    for (float drive : {0.0f, 0.5f, 1.0f}) {
      Params prm = {{P::Shape, 1.0f}, {P::Profile, 4.0f}, {P::Drive, drive}, {P::Cutoff, 16000.0f}};
      prm[P::Root] = static_cast<float>(binHz(pt.kRoot));
      const Buf b = renderNote(prm, 2.2, -1.0, pt.semis);
      CHECK(finite(b));
      const Clean c = cleanliness(b, pt.k, kSettle);
      std::printf("  alias %6.1f Hz drive %.1f: residual %.1f dB re fundamental\n", pt.hz, drive, c.residualDb);
      // Drive 0: the oscillator is -149 dB; what is left is the filter's own soft saturators on a saw.
      CHECK(c.residualDb < (drive > 0.0f ? -52.0 : -75.0));
    }
  }
  // A pitch sweep (pitch envelope) at full shape: no inharmonic junk, i.e. the energy above the band limit
  // stays negligible. Measure the 20-24 kHz band of a +24 st falling sweep from 440 Hz.
  Params sweep = {{P::Shape, 1.0f}, {P::Profile, 4.0f}, {P::PitchAmount, 24.0f}, {P::PitchDecay, 0.2f}, {P::Root, 440.0f}, {P::Cutoff, 16000.0f}};
  const Buf b = renderNote(sweep, 1.6);
  const double top = bandDb(b, 0, 20400.0, 23900.0);
  const double all = bandDb(b, 0, 20.0, 20400.0);
  std::printf("  sweep: 20.4-23.9 kHz at %.1f dB, rest %.1f dB\n", top, all);
  CHECK(top < all - 70.0);
}

void testDcAndClicks() {
  // DC over the whole note (attack to the end of the release) for extremes of shape, drive and cutoff.
  for (float sh : {0.0f, 0.5f, 1.0f}) {
    for (float dr : {0.0f, 1.0f}) {
      for (float root : {30.0f, 55.0f, 440.0f}) {
        Params p = {{P::Shape, sh}, {P::Drive, dr}, {P::Root, root}, {P::Cutoff, 300.0f}, {P::FilterAmount, 3.0f}, {P::Resonance, 0.7f}};
        const Buf b = renderNote(p, 2.0, 1.5);
        CHECK(finite(b));
        // A note is not a whole number of cycles, so even a sine has a mean of up to peak / (pi cycles).
        const double cycles = root * 2.0;
        if (!(std::fabs(mean(b, 0, b.size())) < peak(b, 0, b.size()) * (1.0 / (kPi * cycles) + 0.005))) std::printf("  DC sh %.1f dr %.1f root %.0f: mean %.5f peak %.3f\n", sh, dr, root, mean(b, 0, b.size()), peak(b, 0, b.size()));
        CHECK(std::fabs(mean(b, 0, b.size())) < peak(b, 0, b.size()) * (1.0 / (kPi * cycles) + 0.005));
        CHECK(peak(b, 0, b.size()) <= 1.0);
      }
    }
  }
  // True DC: the mean over a whole number of cycles of a held note, every profile at the extremes of shape and drive.
  for (int pr = 0; pr < kHarmonicProfileCount; pr++) {
    for (float sh : {0.0f, 0.4f, 1.0f}) {
      for (float dr : {0.0f, 1.0f}) {
        for (int k : {41, 301}) {
          const Buf b = steady({{P::Profile, static_cast<float>(pr)}, {P::Shape, sh}, {P::Drive, dr}, {P::Cutoff, 6000.0f}}, k);
          if (!(std::fabs(mean(b, kSettle, kFft)) < 1e-3)) std::printf("  DC pr %d sh %.1f dr %.1f k %d: %.6f\n", pr, sh, dr, k, mean(b, kSettle, kFft));
          CHECK(std::fabs(mean(b, kSettle, kFft)) < 1e-3);  // not exactly 0: the filter's saturators skew a phase-shifted wave
        }
      }
    }
  }
  // Clicks. Compare the largest sample step around note-on, note-off and a retrigger against the largest step
  // of the held sound itself (a waveform's own steepest slope is not a click).
  for (double hz : {30.0, 55.0, 110.0, 440.0}) {
    for (float sh : {0.0f, 1.0f}) {
      Params p = {{P::Shape, sh}, {P::Root, static_cast<float>(hz)}, {P::Drive, sh > 0 ? 0.5f : 0.0f}, {P::Cutoff, 6000.0f}};
      const Buf b = renderNote(p, 1.4, 0.8);
      const double held = maxStep(b, at(0.4), at(0.7));
      const double onset = maxStep(b, 0, at(0.05));
      const double release = maxStep(b, at(0.8), at(1.2));
      std::printf("  clicks %5.1f Hz shape %.0f: held %.4f onset %.4f release %.4f\n", hz, sh, held, onset, release);
      // The attack slope of a 3 ms ramp on a slow sine is the envelope, not a click: bound by 4% of full scale.
      CHECK(onset <= std::max(held * 1.05, 0.02));
      CHECK(release <= std::max(held * 1.05, 0.02));
      // It must actually end: silent after the release.
      CHECK(peak(b, at(1.2), at(0.1)) < 1e-6);
    }
  }
  // Retrigger on a sounding note: a new noteOn mid-sustain is continuous.
  {
    SubBassInstrument inst;
    inst.configure(kRate);
    inst.setParam(id(P::Root), 55.0f);
    inst.setParam(id(P::Shape), 0.7f);
    inst.noteOn(1.0f, 0, 0.0f, 0.0f);
    Buf out;
    float l[kBlock], r[kBlock], s0[kBlock], s1[kBlock];
    float* bus[2] = {l, r};
    float* scratch[2] = {s0, s1};
    for (int blk = 0; blk < 400; blk++) {
      if (blk == 200) inst.noteOn(0.8f, 0, 0.0f, 7.0f);
      std::memset(l, 0, sizeof(l));
      std::memset(r, 0, sizeof(r));
      inst.render(bus, 2, kBlock, scratch);
      out.insert(out.end(), l, l + kBlock);
    }
    const double held = maxStep(out, 128 * 100, 128 * 190);
    const double heldNew = maxStep(out, 128 * 300, 128 * 390);  // the new note is +7 st: a steeper slope of its own
    const double re = maxStep(out, 128 * 200 - 2, 128 * 200 + 600);
    std::printf("  retrigger: held %.4f, new note %.4f, around retrigger %.4f\n", held, heldNew, re);
    CHECK(re <= std::max(held, heldNew) * 1.15);
  }
}

void testControls() {
  const int k = 75;
  // Level scales linearly; Octave halves/doubles the pitch; Tune moves it by cents.
  const double base = peak(steady({}, k), kSettle, kFft);
  CHECK_NEAR(peak(steady({{P::Level, 0.425f}}, k), kSettle, kFft), base * 0.5, 0.003);
  {
    Params p = {{P::Octave, -1.0f}, {P::Root, static_cast<float>(binHz(150))}};
    const auto sp = powerSpectrum(renderNote(p, 2.2), kSettle);
    const size_t pk = static_cast<size_t>(std::max_element(sp.begin(), sp.end()) - sp.begin());
    CHECK(pk >= 74 && pk <= 76);  // octave -1 of bin 150
  }
  {
    Params p = {{P::Tune, 100.0f}, {P::Root, 220.0f}};  // +1 semitone
    const auto sp = powerSpectrum(renderNote(p, 2.2), kSettle);
    const double hz = static_cast<double>(std::max_element(sp.begin(), sp.end()) - sp.begin()) * kRate / kFft;
    CHECK_NEAR(hz, 220.0 * std::exp2(1.0 / 12.0), 1.5);
  }
  {
    Params p = {{P::Root, 110.0f}};
    const auto sp = powerSpectrum(renderNote(p, 2.2, -1.0, 12.0f), kSettle);  // note pitch +12
    const double hz = static_cast<double>(std::max_element(sp.begin(), sp.end()) - sp.begin()) * kRate / kFft;
    CHECK_NEAR(hz, 220.0, 1.5);
  }
  // Filter: lowering the cutoff removes the harmonics of a rich wave and leaves the fundamental.
  {
    Params rich = {{P::Shape, 1.0f}, {P::Profile, 4.0f}};
    Params dark = rich;
    dark[P::Cutoff] = 200.0f;
    const double hi = bandDb(steady(rich, k), kSettle, 1000, 20000), lo = bandDb(steady(dark, k), kSettle, 1000, 20000);
    CHECK(lo < hi - 30.0);
    CHECK(bandDb(steady(dark, k), kSettle, 40, 70) > bandDb(steady(rich, k), kSettle, 40, 70) - 3.0);  // fundamental kept
    // LP 12 is gentler than LP 24.
    dark[P::FilterType] = 1.0f;
    CHECK(bandDb(steady(dark, k), kSettle, 1000, 20000) > lo + 8.0);
  }
  // Resonance adds a peak at the cutoff.
  {
    Params a = {{P::Shape, 1.0f}, {P::Profile, 4.0f}, {P::Cutoff, 800.0f}, {P::Resonance, 0.0f}};
    Params b = a;
    b[P::Resonance] = 0.85f;
    CHECK(bandDb(steady(b, k), kSettle, 600, 1000) > bandDb(steady(a, k), kSettle, 600, 1000) + 6.0);
  }
  // Filter envelope opens the filter at the start and it closes: early brighter than late; negative amount inverts.
  {
    Params p = {{P::Shape, 1.0f}, {P::Profile, 4.0f}, {P::Cutoff, 150.0f}, {P::FilterAmount, 4.0f}, {P::FilterDecay, 0.15f},
                {P::FilterSustain, 0.0f}, {P::Root, 55.0f}};
    auto bright = [](const Buf& x, size_t from) {  // rms of the first difference: weights the highs
      double e = 0;
      for (size_t i = from + 1; i < from + 2400; i++) e += static_cast<double>(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
      return std::sqrt(e / 2400.0);
    };
    const Buf b = renderNote(p, 1.0);
    std::printf("  filter env brightness early %.4f late %.4f\n", bright(b, at(0.02)), bright(b, at(0.8)));
    CHECK(bright(b, at(0.02)) > bright(b, at(0.8)) * 2.0);
    p[P::FilterAmount] = -4.0f;
    p[P::Cutoff] = 4000.0f;
    const Buf c = renderNote(p, 1.0);
    CHECK(bright(c, at(0.02)) < bright(c, at(0.8)) * 0.5);
  }
  // Pitch envelope: starts higher and settles; the start frequency is amount semitones above.
  {
    Params p = {{P::Root, 200.0f}, {P::PitchAmount, 24.0f}, {P::PitchDecay, 0.1f}};
    auto zc = [](const Buf& x, size_t from, size_t n) { int c = 0; for (size_t i = from + 1; i < from + n; i++) c += (x[i - 1] < 0) != (x[i] < 0); return c; };
    const Buf b = renderNote(p, 1.0);
    CHECK(zc(b, at(0.002), 1440) > zc(b, at(0.9), 1440) * 1.5);
    CHECK(zc(b, at(0.9), 1440) > 0);
    // Negative amount starts lower.
    p[P::PitchAmount] = -24.0f;
    const Buf c = renderNote(p, 1.0);
    CHECK(zc(c, at(0.002), 1440) * 1.5 < zc(c, at(0.9), 1440));
  }
  // Amp envelope: attack takes time, sustain holds the level, release ends the note.
  {
    Params p = {{P::AmpAttack, 0.2f}, {P::AmpDecay, 0.2f}, {P::AmpSustain, 0.5f}, {P::AmpRelease, 0.3f}, {P::Root, 110.0f}};
    const Buf b = renderNote(p, 1.5, 1.0);
    CHECK(peak(b, 0, at(0.02)) < 0.05 * peak(b, 0, b.size()));
    const double sus = rms(b, at(0.7), 4800), top = peak(b, 0, at(0.25)) / std::sqrt(2.0);
    CHECK(sus < top * 0.4 && sus > top * 0.1);  // sustain 0.5 -> 0.25 in the squared-envelope gain
    CHECK(peak(b, at(1.35), 4800) < 1e-6);
  }
  // Phase: a fresh note starts at the set phase; free-run does not restart.
  {
    Params p = {{P::Phase, 0.25f}, {P::AmpAttack, 0.001f}, {P::Root, 55.0f}};
    const Buf b = renderNote(p, 0.1);
    CHECK(b[at(0.0015)] > 0.3f);  // starts near the sine's peak, not at zero
    p[P::Phase] = 0.0f;
    const Buf z = renderNote(p, 0.1);
    CHECK(std::fabs(z[at(0.0001)]) < 0.01f);
  }
  // Velocity: scales the level, and 0 is silent.
  CHECK(peak(renderNote({}, 0.5, -1.0, 0.0f, 0.5f), 0, at(0.5)) < peak(renderNote({}, 0.5), 0, at(0.5)) * 0.7);
  CHECK(peak(renderNote({}, 0.5, -1.0, 0.0f, 0.0f), 0, at(0.5)) < 1e-6);
}

void testStability() {
  // Pseudo-random parameter sets at their extremes and in the middle, held, released, with a pitch jump: finite and
  // bounded, no runaway, silent at the end.
  uint32_t s = 12345;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return static_cast<float>(s >> 8) / 16777216.0f; };
  double worst = 0;
  for (int trial = 0; trial < 60; trial++) {
    Params p;
    for (int32_t i = 0; i < kSubBassParamCount; i++) {
      const InstrumentParamInfo& info = kSubBassParams[i];
      const float r = rnd();
      const float v = r < 0.2f ? info.min : r < 0.4f ? info.max : info.min + (info.max - info.min) * rnd();
      p[static_cast<P>(info.id)] = v;
    }
    p[P::AmpRelease] = 0.05f;
    p[P::AmpAttack] = std::min(p[P::AmpAttack], 0.2f);
    p[P::AmpDecay] = std::min(p[P::AmpDecay], 0.5f);
    const Buf b = renderNote(p, 1.5, 0.8, (rnd() - 0.5f) * 48.0f);
    CHECK(finite(b));
    worst = std::max(worst, peak(b, 0, b.size()));
    CHECK(peak(b, 0, b.size()) <= 1.0);
    CHECK(std::fabs(mean(b, 0, b.size())) < 0.05);
  }
  std::printf("  random extremes: worst peak %.3f\n", worst);
  // Resonance and cutoff held at the extremes, a long note.
  for (float cut : {30.0f, 16000.0f}) {
    Params p = {{P::Resonance, 1.0f}, {P::Cutoff, cut}, {P::Shape, 1.0f}, {P::Drive, 1.0f}, {P::FilterAmount, 5.0f}};
    const Buf b = renderNote(p, 2.0);
    CHECK(finite(b) && peak(b, 0, b.size()) <= 1.0);
  }
  // Parameter jumps in a held note are smoothed: the largest step across a jump never exceeds the steepest slope
  // of the settled sound before or after it. Each case changes one thing at a time; the filter stays dark where
  // the jump would otherwise legitimately reveal a bright edge (a saturated sine is a near-square wave).
  struct Jump { const char* name; Params before; Params change; double limit; };
  const Jump jumps[] = {
      {"level", {{P::Cutoff, 12000.0f}}, {{P::Level, 0.2f}}, 1.3},
      {"cutoff", {{P::Cutoff, 12000.0f}}, {{P::Cutoff, 400.0f}}, 1.3},
      {"resonance", {{P::Cutoff, 400.0f}}, {{P::Resonance, 0.8f}}, 3.0},  // a resonant filter rings when its Q changes: gentle, not a click
      {"shape+drive (dark)", {{P::Cutoff, 300.0f}}, {{P::Shape, 1.0f}, {P::Drive, 1.0f}}, 1.3},
      {"shape (dark)", {{P::Cutoff, 300.0f}, {P::Shape, 1.0f}}, {{P::Shape, 0.0f}}, 1.3},
      {"profile", {{P::Cutoff, 300.0f}, {P::Shape, 0.6f}}, {{P::Profile, 2.0f}}, 1.3},
      {"filter type", {{P::Cutoff, 300.0f}, {P::Shape, 0.6f}}, {{P::FilterType, 1.0f}}, 1.3},
  };
  for (const Jump& jp : jumps) {
    SubBassInstrument inst;
    inst.configure(kRate);
    inst.setParam(id(P::Root), 55.0f);
    for (const auto& kv : jp.before) inst.setParam(id(kv.first), kv.second);
    inst.noteOn(1.0f, 0, 0.0f, 0.0f);
    float l[kBlock], r[kBlock], s0[kBlock], s1[kBlock];
    float* bus[2] = {l, r};
    float* scratch[2] = {s0, s1};
    Buf out;
    for (int blk = 0; blk < 600; blk++) {
      if (blk == 300) for (const auto& kv : jp.change) inst.setParam(id(kv.first), kv.second);
      std::memset(l, 0, sizeof(l));
      std::memset(r, 0, sizeof(r));
      inst.render(bus, 2, kBlock, scratch);
      out.insert(out.end(), l, l + kBlock);
    }
    CHECK(finite(out));
    const double settled = std::max(maxStep(out, 128 * 150, 128 * 290), maxStep(out, 128 * 450, 128 * 590));
    const double across = maxStep(out, 128 * 300 - 4, 128 * 450);
    std::printf("  jump %-20s settled %.4f, across %.4f\n", jp.name, settled, across);
    CHECK(across <= settled * jp.limit + 1e-4);
  }
}

void testLoudness() {
  // Shape moves the loudness of a held note by only a few dB; Drive too. (RMS-normalised oscillator, trimmed drive.)
  const int k = 75;
  const double ref = rms(steady({}, k), kSettle, kFft);
  double lo = 99, hi = -99;
  for (float sh : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    for (int pr = 0; pr < kHarmonicProfileCount; pr++) {
      const double d = dbOf(rms(steady({{P::Shape, sh}, {P::Profile, static_cast<float>(pr)}}, k), kSettle, kFft) / ref);
      lo = std::min(lo, d);
      hi = std::max(hi, d);
    }
  }
  std::printf("  shape/profile loudness spread: %.1f .. %.1f dB\n", lo, hi);
  CHECK(lo > -3.5 && hi < 3.0);
  lo = 99; hi = -99;
  for (float dr : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const double d = dbOf(rms(steady({{P::Drive, dr}}, k), kSettle, kFft) / ref);
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
  std::printf("  drive loudness spread: %.1f .. %.1f dB\n", lo, hi);
  CHECK(lo > -3.0 && hi < 4.0);
}

void testSampleRates() {
  for (double rate : {44100.0, 96000.0}) {
    const Buf b = renderNote({{P::Shape, 1.0f}, {P::Drive, 0.5f}, {P::Root, 440.0f}}, 0.9, 0.3, 0.0f, 1.0f, rate);
    CHECK(finite(b));
    CHECK(peak(b, 0, b.size()) <= 1.0 && peak(b, 0, b.size()) > 0.2);
    CHECK(peak(b, at(0.7, rate), 1000) < 1e-6);
  }
}

void testPreDelayAndDeterminism() {
  Params p = {{P::Shape, 0.6f}, {P::Drive, 0.3f}};
  CHECK(renderNote(p, 0.3) == renderNote(p, 0.3));
  // Scheduled note: silent before its frame, sounding right after (sample-accurate pre-note delay).
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kSb, 0);
  e.scheduleInstrumentNote(1000, 1, 1.0f);
  Buf l;
  for (int64_t f = 0; f < 4096; f += kBlock) {
    e.process(f, kBlock);
    l.insert(l.end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
  CHECK(peak(l, 0, 1000) == 0.0);
  CHECK(peak(l, 1000, 3000) > 0.01);
}

void testRegistrationAndTable() {
  const InstrumentKindMeta* m = instrumentKindMeta(kSb);
  CHECK(m && std::string(m->id) == "subbass" && std::string(m->category) == "synth");
  CHECK(kindParamCount(kSb) == kSubBassParamCount && kSubBassParamCount == 23);
  for (int32_t i = 0; i < kSubBassParamCount; i++) {
    const InstrumentParamInfo& p = kSubBassParams[i];
    CHECK(p.id == i);
    CHECK(p.def >= p.min && p.def <= p.max);
    for (int32_t j = i + 1; j < kSubBassParamCount; j++) CHECK(std::strcmp(p.name, kSubBassParams[j].name) != 0);
  }
  CHECK(instrumentParamInfo(kSb, kSubBassParamCount) == &kInstrumentGainInfo);
  CHECK(resolveModDomain(kSubBassParams[id(P::Cutoff)]) == ModDomain::Log);
  CHECK(resolveModDomain(kSubBassParams[id(P::Octave)]) == ModDomain::None);
  CHECK(resolveModDomain(kSubBassParams[id(P::Profile)]) == ModDomain::None);
  CHECK(resolveModDomain(kSubBassParams[id(P::PhaseReset)]) == ModDomain::None);
  // Clamping and non-finite values.
  SubBassInstrument inst;
  inst.configure(kRate);
  inst.setParam(id(P::Shape), 5.0f);
  inst.setParam(id(P::Shape), std::nanf(""));
  inst.setParam(9999, 1.0f);  // unknown id: no-op
  Engine e;
  e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kSb, 0));
  e.instrumentNoteOn(1, 1.0f);
  for (int i = 0; i < 8; i++) e.process(i * kBlock, kBlock);
  CHECK(peak(Buf(e.outputChannel(0), e.outputChannel(0) + kBlock), 0, kBlock) > 0.0);
}

void testNoAllocationInAudioPath() {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kSb, 0);
  e.setInstrumentParam(1, id(P::Shape), 0.8f);
  e.setInstrumentParam(1, id(P::Drive), 0.6f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0;
  for (int i = 0; i < 4; i++, f += kBlock) e.process(f, kBlock);
  const long before = g_allocs;
  for (int i = 0; i < 400; i++, f += kBlock) {
    if (i == 100) e.instrumentNoteOn(1, 0.9f, 0.0f, 5.0f);
    if (i == 200) e.setInstrumentParam(1, id(P::Cutoff), 500.0f);
    if (i == 300) e.instrumentNoteOff(1);
    e.process(f, kBlock);
  }
  CHECK(g_allocs == before);
}

// The five starting sounds (mirrors SUB_BASS_PRESETS in src/runtime/types.ts).
struct Preset { const char* name; Params p; };
std::vector<Preset> presets() {
  return {
      {"pure_sub", {{P::Shape, 0.0f}, {P::Drive, 0.0f}, {P::Cutoff, 16000.0f}, {P::Resonance, 0.0f}, {P::AmpAttack, 0.004f}, {P::AmpRelease, 0.3f}, {P::Root, 41.2f}}},
      {"warm_bass", {{P::Level, 1.0f}, {P::Shape, 0.3f}, {P::Profile, 1.0f}, {P::Drive, 0.2f}, {P::Cutoff, 500.0f}, {P::Resonance, 0.15f}, {P::FilterAmount, 1.2f}, {P::FilterDecay, 0.25f}, {P::FilterSustain, 0.3f}, {P::AmpDecay, 0.4f}, {P::AmpSustain, 0.8f}, {P::AmpRelease, 0.2f}, {P::Root, 55.0f}}},
      {"punchy_bass", {{P::Level, 1.0f}, {P::Shape, 0.5f}, {P::Profile, 0.0f}, {P::Drive, 0.35f}, {P::Cutoff, 350.0f}, {P::Resonance, 0.2f}, {P::FilterAmount, 2.5f}, {P::FilterDecay, 0.12f}, {P::FilterSustain, 0.1f}, {P::PitchAmount, 7.0f}, {P::PitchDecay, 0.04f}, {P::AmpDecay, 0.25f}, {P::AmpSustain, 0.7f}, {P::AmpRelease, 0.12f}, {P::Root, 55.0f}}},
      {"acid_bass", {{P::Level, 1.0f}, {P::Shape, 0.85f}, {P::Profile, 4.0f}, {P::Drive, 0.4f}, {P::Cutoff, 220.0f}, {P::Resonance, 0.75f}, {P::FilterAmount, 3.6f}, {P::FilterDecay, 0.22f}, {P::FilterSustain, 0.0f}, {P::AmpDecay, 0.3f}, {P::AmpSustain, 0.6f}, {P::AmpRelease, 0.08f}, {P::Root, 55.0f}}},
      {"aggressive_bass", {{P::Level, 1.0f}, {P::Shape, 1.0f}, {P::Profile, 3.0f}, {P::Drive, 0.75f}, {P::Cutoff, 400.0f}, {P::Resonance, 0.55f}, {P::FilterAmount, 3.0f}, {P::FilterDecay, 0.1f}, {P::FilterSustain, 0.25f}, {P::PitchAmount, 3.0f}, {P::PitchDecay, 0.03f}, {P::AmpDecay, 0.15f}, {P::AmpSustain, 0.8f}, {P::AmpRelease, 0.06f}, {P::Root, 55.0f}}},
  };
}

void writeWav(const char* name, const Buf& b) {
  const char* dir = std::getenv("SUBBASS_WAV_DIR");
  if (!dir) return;
  const std::string path = std::string(dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(b.size()), bytes = n * 2, rate = 48000, byteRate = rate * 2;
  const uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
  const uint32_t riff = 36 + bytes, fmt = 16;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f);
  std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
  for (float v : b) { const int16_t s = static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); std::fwrite(&s, 2, 1, f); }
  std::fclose(f);
}

void testPresets() {
  for (const Preset& pr : presets()) {
    // Played across the sub range and a few notes up; bounded, finite, no DC, ends silent.
    for (float semis : {0.0f, 12.0f, 24.0f}) {
      const Buf b = renderNote(pr.p, 1.6, 1.0, semis);
      CHECK(finite(b));
      CHECK(peak(b, 0, b.size()) <= 1.0);
      CHECK(peak(b, 0, b.size()) > 0.15);
      CHECK(std::fabs(mean(b, 0, b.size())) < 0.01);
      CHECK(peak(b, at(1.5), 2000) < 1e-6);
    }
    const Buf b = renderNote(pr.p, 1.6, 1.0);
    std::printf("  preset %-16s peak %.3f rms %.3f\n", pr.name, peak(b, 0, b.size()), rms(b, at(0.1), at(0.8)));
    writeWav(pr.name, b);
  }
  // The pure sub really is the sine: nothing but the fundamental.
  Params sub = presets()[0].p;
  const int k = 56;
  sub[P::Root] = static_cast<float>(binHz(k));
  const Clean c = cleanliness(renderNote(sub, 2.2), k, kSettle);
  CHECK(c.harmDb < -70.0 && c.residualDb < -80.0);
}

}  // namespace

int main() {
  testRegistrationAndTable();
  testHarmonicSpectrum();
  testOscillatorBandLimit();
  testPureSineAtLowFrequencies();
  testShapeAddsHarmonics();
  testDriveIsNotShape();
  testAliasing();
  testDcAndClicks();
  testControls();
  testStability();
  testLoudness();
  testSampleRates();
  testPreDelayAndDeterminism();
  testNoAllocationInAudioPath();
  testPresets();
  std::printf("subbass_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
