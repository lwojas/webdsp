#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../src/dsp/compressor.h"
#include "../src/params.h"
#include "test_main.h"

// The compressor against what a drum bus needs (docs/compressor-node.md, "Validation"): analytic checks on steady
// tones and DC, then a deterministic kick / snare / hat / percussion pattern with low end, normalised to -3 dBFS.

using namespace webdsp;

namespace {

constexpr double kSr = 48000.0;
using NP = NodeParam;

struct Settings {
  float threshold = -18, ratio = 4, attack = 0.01f, release = 0.1f, knee = 0, makeup = 0;
};

void apply(Compressor& c, const Settings& s) {
  c.setParam(static_cast<int32_t>(NP::CompThreshold), s.threshold);
  c.setParam(static_cast<int32_t>(NP::CompRatio), s.ratio);
  c.setParam(static_cast<int32_t>(NP::CompAttack), s.attack);
  c.setParam(static_cast<int32_t>(NP::CompRelease), s.release);
  c.setParam(static_cast<int32_t>(NP::CompKnee), s.knee);
  c.setParam(static_cast<int32_t>(NP::CompMakeup), s.makeup);
}

double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// Steady-state gain in dB (output RMS over input RMS) on a stereo sine of peak `peakDb`.
double toneGainDb(float hz, float peakDb, const Settings& s) {
  Compressor c(kSr);
  apply(c, s);
  const int n = static_cast<int>(kSr * 2);
  std::vector<float> l(n), r(n), in(n);
  const float a = std::pow(10.0f, peakDb / 20.0f);
  for (int i = 0; i < n; i++) in[i] = l[i] = r[i] = a * std::sin(2.0 * M_PI * hz * i / kSr);
  float* ch[2] = {l.data(), r.data()};
  c.process(ch, 2, n);
  double si = 0, so = 0;
  for (int i = n - static_cast<int>(kSr / 2); i < n; i++) {
    si += static_cast<double>(in[i]) * in[i];
    so += static_cast<double>(l[i]) * l[i];
  }
  return 10.0 * std::log10(so / si);
}

// Gain trajectory (dB per sample) for a constant input that steps from silence to `level`.
std::vector<double> dcStepGainDb(float level, const Settings& s, int n) {
  Compressor c(kSr);
  apply(c, s);
  std::vector<float> l(n, level), r(n, level);
  float* ch[2] = {l.data(), r.data()};
  c.process(ch, 2, n);
  std::vector<double> g(n);
  for (int i = 0; i < n; i++) g[i] = db(l[i] / level);
  return g;
}

// --- the drum pattern -------------------------------------------------------------------------------------------

struct Kit {
  std::vector<float> l, r;
};

Kit makeKit(double peakDb = -3.0) {
  const double beat = 0.5;
  const int n = static_cast<int>(kSr * beat * 8);
  Kit k{std::vector<float>(n), std::vector<float>(n)};
  uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
  };
  auto add = [&](double t, int kind, float g) {
    const int st = static_cast<int>(t * kSr);
    const int len = static_cast<int>(kSr * (kind == 0 ? 0.45 : kind == 1 ? 0.25 : kind == 2 ? 0.06 : 0.12));
    double ph = 0;
    float prev = 0;
    for (int i = 0; i < len && st + i < n; i++) {
      const double x = i / kSr;
      float v = 0;
      if (kind == 0) {  // kick: 135 -> 45 Hz sweep, long sub tail, tiny click
        ph += 2 * M_PI * (45 + 90 * std::exp(-x * 40)) / kSr;
        v = static_cast<float>(std::sin(ph) * std::exp(-x * 7) * 0.9 + 0.25 * std::exp(-x * 400) * rnd());
      } else if (kind == 1) {  // snare: 190 Hz body + noise
        ph += 2 * M_PI * 190 / kSr;
        v = static_cast<float>(0.45 * std::sin(ph) * std::exp(-x * 25) + 0.55 * rnd() * std::exp(-x * 18));
      } else if (kind == 2) {  // hat: high-passed noise
        const float nz = rnd();
        v = 0.35f * (nz - prev) * static_cast<float>(std::exp(-x * 90));
        prev = nz;
      } else {  // percussion
        ph += 2 * M_PI * 820 / kSr;
        v = static_cast<float>(0.4 * std::sin(ph) * std::exp(-x * 35));
      }
      k.l[st + i] += v * g;
      k.r[st + i] += v * g * (kind == 3 ? 0.6f : 1.0f);
    }
  };
  for (int b = 0; b < 8; b++) {
    const double t = b * beat;
    if (b % 2 == 0 || b == 5) add(t, 0, 0.9f);
    if (b % 2 == 1) add(t, 1, 0.8f);
    add(t, 2, 0.5f);
    add(t + beat / 2, 2, 0.35f);
    if (b == 3 || b == 6) add(t + beat * 0.75, 3, 0.6f);
    if (b == 7) add(t + beat * 0.5, 0, 0.7f);
  }
  float pk = 0;
  for (float x : k.l) pk = std::max(pk, std::fabs(x));
  const float g = std::pow(10.0f, static_cast<float>(peakDb) / 20.0f) / pk;
  for (auto& x : k.l) x *= g;
  for (auto& x : k.r) x *= g;
  return k;
}

struct Run {
  Kit out;
  double maxGrDb = 0;   // deepest per-block reduction, as the UI's meter would show it
  double rmsDeltaDb = 0;
  double lowDeltaDb = 0;  // level change below ~120 Hz
  double hitPeakDb = 0;   // peak of the first snare hit, out relative to in
};

double rmsOf(const std::vector<float>& v) {
  double s = 0;
  for (float x : v) s += static_cast<double>(x) * x;
  return std::sqrt(s / v.size());
}

std::vector<float> lowpass(const std::vector<float>& v, float hz) {
  std::vector<float> o(v.size());
  const float a = 1.0f - std::exp(-2.0f * static_cast<float>(M_PI) * hz / static_cast<float>(kSr));
  float y = 0;
  for (size_t i = 0; i < v.size(); i++) o[i] = y += a * (v[i] - y);
  return o;
}

Run runKit(const Kit& in, const Settings& s) {
  Run r;
  r.out = in;
  Compressor c(kSr);
  apply(c, s);
  const int n = static_cast<int>(in.l.size());
  for (int i = 0; i + 128 <= n; i += 128) {
    float* ch[2] = {r.out.l.data() + i, r.out.r.data() + i};
    c.process(ch, 2, 128);
    float a, gr, b;
    CHECK(c.takeMeter(&a, &gr, &b));
    r.maxGrDb = std::min(r.maxGrDb, static_cast<double>(gr));
  }
  r.rmsDeltaDb = db(rmsOf(r.out.l) / rmsOf(in.l));
  r.lowDeltaDb = db(rmsOf(lowpass(r.out.l, 120)) / rmsOf(lowpass(in.l, 120)));
  const int st = static_cast<int>(kSr * 0.5), len = static_cast<int>(kSr * 0.03);
  float pi = 0, po = 0;
  for (int i = 0; i < len; i++) {
    pi = std::max(pi, std::fabs(in.l[st + i]));
    po = std::max(po, std::fabs(r.out.l[st + i]));
  }
  r.hitPeakDb = db(po / pi);
  return r;
}

}  // namespace

// Ratio sets the reduction above the threshold: a 1 kHz tone 18 dB over it settles at 18 * (1 - 1/R), and 10:1 takes far more
// than 2:1. This is the check the old envelope-in-dB detector failed (it read -7.9 dB at 2:1 where the maths says -9).
static void testRatioSetsTheReduction() {
  Settings s;
  s.threshold = -24;
  double last = 0;
  for (float ratio : {1.0f, 2.0f, 4.0f, 10.0f, 20.0f}) {
    s.ratio = ratio;
    const double g = toneGainDb(1000, -6, s);
    CHECK_NEAR(g, -18.0 * (1.0 - 1.0 / ratio), 0.4);
    CHECK(g <= last + 1e-6);
    last = g;
  }
  s.ratio = 2;
  const double g2 = toneGainDb(1000, -6, s);
  s.ratio = 10;
  CHECK(toneGainDb(1000, -6, s) < g2 - 6.0);
}

// Threshold decides where compression starts: lower means more reduction, and a tone under it is untouched.
static void testThresholdDecidesWhereItStarts() {
  Settings s;
  s.ratio = 4;
  double last = 1.0;
  for (float t : {0.0f, -12.0f, -24.0f, -36.0f, -48.0f}) {
    s.threshold = t;
    const double g = toneGainDb(1000, -6, s);
    CHECK(g <= last + 1e-6);
    last = g;
  }
  s.threshold = -3;  // -6 dB peak is 3 dB under it
  CHECK_NEAR(toneGainDb(1000, -6, s), 0.0, 0.02);
}

// A bass note keeps its level through the gain computer: the gain on 50 Hz matches 1 kHz and barely ripples within a cycle.
static void testLowFrequencyIsNotOverRead() {
  Settings s;
  s.threshold = -24;
  s.attack = 0.003f;
  s.release = 0.05f;
  CHECK_NEAR(toneGainDb(50, -6, s), toneGainDb(1000, -6, s), 0.4);
  CHECK_NEAR(toneGainDb(40, -6, s), toneGainDb(1000, -6, s), 0.4);
}

// Attack and release are the time the gain takes to cover 63% of its move, in the dB domain of the reduction.
static void testAttackAndReleaseAreTheStatedTimes() {
  Settings s;
  s.threshold = -30;
  s.ratio = 8;
  s.attack = 0.02f;
  s.release = 0.2f;
  const int n = static_cast<int>(kSr * 2);
  const auto g = dcStepGainDb(0.5f, s, n);  // -6 dB DC, 24 dB over: settles at -21 dB
  const double final = g[n - 1];
  CHECK_NEAR(final, -21.0, 0.3);
  int t63 = 0;
  while (t63 < n && g[t63] > final * 0.632) t63++;
  CHECK_NEAR(t63 / kSr, 0.02, 0.006);

  // Release: input drops to silence-level (-60 dB, below threshold) after being compressed.
  Compressor c(kSr);
  apply(c, s);
  std::vector<float> l(n, 0.5f), r(n, 0.5f);
  float* ch[2] = {l.data(), r.data()};
  c.process(ch, 2, n);
  std::vector<float> l2(n, 0.001f), r2(n, 0.001f);
  float* ch2[2] = {l2.data(), r2.data()};
  c.process(ch2, 2, n);
  // The 20 ms detector decay precedes the release, so allow for it.
  int r63 = 0;
  while (r63 < n && db(l2[r63] / 0.001f) < final * 0.368) r63++;
  CHECK_NEAR(r63 / kSr, 0.2 + 0.02, 0.05);
}

// Knee: a hard knee does nothing at the threshold, a soft one starts early and is gentler; both agree well above it.
static void testKneeShapesTheTransition() {
  Settings s;
  s.ratio = 4;
  s.threshold = -20;
  const float atThreshold = std::pow(10.0f, -20.0f / 20.0f);
  s.knee = 0;
  const double hard = dcStepGainDb(atThreshold, s, 48000)[47999];
  s.knee = 12;
  const double soft = dcStepGainDb(atThreshold, s, 48000)[47999];
  CHECK_NEAR(hard, 0.0, 0.01);
  CHECK_NEAR(soft, -(1.0 - 1.0 / 4.0) * 12.0 / 8.0, 0.05);  // -(1 - 1/R) * knee / 8 at the threshold
  const float below = std::pow(10.0f, -23.0f / 20.0f);  // inside the soft knee's lower half
  CHECK(dcStepGainDb(below, s, 48000)[47999] < -0.05);
  s.knee = 0;
  CHECK_NEAR(dcStepGainDb(below, s, 48000)[47999], 0.0, 0.01);
  const float far = std::pow(10.0f, -2.0f / 20.0f);
  s.knee = 0;
  const double h = dcStepGainDb(far, s, 48000)[47999];
  s.knee = 12;
  CHECK_NEAR(dcStepGainDb(far, s, 48000)[47999], h, 0.01);
}

// Makeup is added after the reduction is computed: the meter reads the same, the output is simply that much louder.
static void testMakeupIsIndependent() {
  const Kit kit = makeKit();
  Settings s;
  s.threshold = -15;
  const Run a = runKit(kit, s);
  s.makeup = 6;
  const Run b = runKit(kit, s);
  CHECK_NEAR(a.maxGrDb, b.maxGrDb, 1e-4);
  CHECK_NEAR(b.rmsDeltaDb - a.rmsDeltaDb, 6.0, 0.01);
}

// On the drum pattern: lowering the threshold deepens the reduction, a higher ratio deepens it, and a slower attack lets
// more of the snare's first transient through.
static void testBehavesOnADrumPattern() {
  const Kit kit = makeKit();
  Settings s;
  double last = 1.0;
  for (float t : {-6.0f, -12.0f, -18.0f, -24.0f}) {
    s.threshold = t;
    const double gr = runKit(kit, s).maxGrDb;
    CHECK(gr < last);
    last = gr;
  }
  s.threshold = -18;
  s.ratio = 2;
  const double gr2 = runKit(kit, s).maxGrDb;
  s.ratio = 10;
  const double gr10 = runKit(kit, s).maxGrDb;
  CHECK(gr10 < gr2 - 3.0);

  s.ratio = 4;
  double lastPeak = -100;
  for (float a : {0.0005f, 0.003f, 0.01f, 0.03f, 0.1f}) {
    s.attack = a;
    const double p = runKit(kit, s).hitPeakDb;
    CHECK(p >= lastPeak - 1e-6);  // slower attack: peak closer to unity
    lastPeak = p;
  }
  s.attack = 0.0005f;
  const double fast = runKit(kit, s).hitPeakDb;
  s.attack = 0.1f;
  CHECK(runKit(kit, s).hitPeakDb > fast + 3.0);

  // Release: a longer one holds the gain down between hits, so the mean reduction is deeper.
  s.attack = 0.01f;
  s.release = 0.03f;
  const double quick = runKit(kit, s).rmsDeltaDb;
  s.release = 0.8f;
  CHECK(runKit(kit, s).rmsDeltaDb < quick - 0.3);
}

// Heavy compression is not a low-cut: with level matched, the kick's band keeps within 3 dB of the whole kit.
static void testHeavyCompressionKeepsTheLowEnd() {
  const Kit kit = makeKit();
  Settings s;
  s.threshold = -19;
  s.ratio = 10;
  s.attack = 0.004f;
  s.release = 0.05f;
  s.knee = 2;
  const Run heavy = runKit(kit, s);
  CHECK(heavy.maxGrDb < -9.0);
  s.makeup = static_cast<float>(-heavy.rmsDeltaDb);
  const Run matched = runKit(kit, s);
  CHECK_NEAR(matched.rmsDeltaDb, 0.0, 0.01);
  CHECK(std::fabs(matched.lowDeltaDb) < 3.0);
}

// The presets (src/runtime/compressor.ts): each lands on its target reduction at its threshold below the kit's peak, and
// its explicit makeup keeps the loudness. If this fails the table in compressor.ts has drifted from the DSP.
static void testDrumBusPresets() {
  struct P { const char* name; Settings s; float below; float target; };
  const P presets[] = {
      {"Drum Glue", {0, 2, 0.025f, 0.1f, 8, 1.3f}, 11.8f, 3.0f},
      {"Punch", {0, 3, 0.028f, 0.09f, 6, 1.7f}, 12.2f, 4.0f},
      {"Tight", {0, 4, 0.009f, 0.07f, 6, 2.3f}, 10.8f, 5.5f},
      {"Smash", {0, 10, 0.004f, 0.05f, 2, 5.5f}, 16.0f, 12.0f},
  };
  const Kit kit = makeKit(-3.0);
  for (const P& p : presets) {
    Settings s = p.s;
    s.threshold = -3.0f - p.below;
    const Run r = runKit(kit, s);
    CHECK_NEAR(-r.maxGrDb, p.target, 0.6);
    CHECK_NEAR(r.rmsDeltaDb, 0.0, 0.5);  // not dramatically louder or quieter
    CHECK(std::fabs(r.lowDeltaDb) < 3.0);
  }
}

// The meter feed: input and output peaks, and the reduction, read-and-cleared; a bypassed node reports nothing.
static void testMeterFeed() {
  Compressor idle(kSr);
  float a, g, b;
  CHECK(!idle.takeMeter(&a, &g, &b));
  Compressor c(kSr);
  Settings s;
  s.threshold = -20;
  s.makeup = 3;
  apply(c, s);
  std::vector<float> l(2048, 0.5f), r(2048, 0.25f);
  float* ch[2] = {l.data(), r.data()};
  c.process(ch, 2, 2048);
  CHECK(c.takeMeter(&a, &g, &b));
  CHECK_NEAR(a, 0.5, 1e-6);
  CHECK(g < -10.0f);
  CHECK(b > 0.0f && b < a * std::pow(10.0f, 3.0f / 20.0f));
  c.reset();
  CHECK(c.takeMeter(&a, &g, &b));
  CHECK(a == 0.0f && b == 0.0f && g == 0.0f);
}

int main() {
  testRatioSetsTheReduction();
  testThresholdDecidesWhereItStarts();
  testLowFrequencyIsNotOverRead();
  testAttackAndReleaseAreTheStatedTimes();
  testKneeShapesTheTransition();
  testMakeupIsIndependent();
  testBehavesOnADrumPattern();
  testHeavyCompressionKeepsTheLowEnd();
  testDrumBusPresets();
  testMeterFeed();
  std::printf("compressor_drumbus_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures, webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
