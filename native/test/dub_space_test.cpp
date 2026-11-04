#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../src/fx_chain.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kKind = static_cast<int32_t>(FxKind::DubSpace);
constexpr int32_t kId = 1;
using Buf = std::vector<std::vector<float>>;

int32_t P(NodeParam p) { return static_cast<int32_t>(p); }
double framesPerBeat(double bpm) { return kRate * 60.0 / bpm; }

struct Settings {
  bool echo = true, space = true;
  int32_t time = 1;  // 1/8: 250 ms at 120 BPM
  float feedback = 0.55f, tone = 16000.0f, size = 0.5f, decay = 0.35f, depth = 0.5f, damping = 0.5f, character = 0.0f, mix = 1.0f;
};

struct Rig {
  FxChain chain;
  explicit Rig(const Settings& s, double bpm = 120.0) {
    chain.configure(kRate);
    chain.setClock(0, 0.0, framesPerBeat(bpm));
    chain.add(kId, kKind, -1);
    apply(s);
  }
  void set(NodeParam p, float v) { chain.setParam(kId, P(p), v); }
  void apply(const Settings& s) {
    set(NodeParam::DubSpaceEchoOn, s.echo ? 1.0f : 0.0f);
    set(NodeParam::DubSpaceSpaceOn, s.space ? 1.0f : 0.0f);
    set(NodeParam::DubSpaceTime, static_cast<float>(s.time));
    set(NodeParam::DubSpaceFeedback, s.feedback);
    set(NodeParam::DubSpaceTone, s.tone);
    set(NodeParam::DubSpaceSize, s.size);
    set(NodeParam::DubSpaceDecay, s.decay);
    set(NodeParam::DubSpaceDepth, s.depth);
    set(NodeParam::DubSpaceDamping, s.damping);
    set(NodeParam::DubSpaceCharacter, s.character);
    set(NodeParam::DubSpaceMix, s.mix);
  }
  // Runs `data` (planar) through the chain in place, in kBlock chunks. Mix is additive (out = dry + wet), so by
  // default the dry input is subtracted again and the tests see the wet signal; raw = true keeps the output.
  void run(Buf& data, bool raw = false) {
    const Buf dry = data;
    runRaw(data);
    if (raw) return;
    for (size_t c = 0; c < data.size(); c++)
      for (size_t i = 0; i < data[c].size(); i++) data[c][i] -= dry[c][i];
  }
  void runRaw(Buf& data) {
    const int32_t nc = static_cast<int32_t>(data.size());
    const int32_t n = static_cast<int32_t>(data[0].size());
    for (int32_t at = 0; at < n; at += kBlock) {
      const int32_t len = std::min(kBlock, n - at);
      float* ch[2] = {data[0].data() + at, nc > 1 ? data[1].data() + at : nullptr};
      chain.process(ch, nc, len);
    }
  }
};

Buf stereo(const std::vector<float>& x) { return {x, x}; }
constexpr int32_t kLead = 2400;  // the mix glides in over 10 ms; impulses land after it
std::vector<float> impulse(int32_t n, float a = 1.0f) {
  std::vector<float> x(n, 0.0f);
  x[kLead] = a;
  return x;
}
std::vector<float> noise(int32_t n, float amp = 0.5f, uint32_t seed = 12345) {
  std::vector<float> x(n);
  for (float& v : x) {
    seed = seed * 1664525u + 1013904223u;
    v = (static_cast<int32_t>(seed >> 8) / static_cast<float>(1 << 23) - 1.0f) * amp;
  }
  return x;
}
std::vector<float> sine(int32_t n, double hz, float amp) {
  std::vector<float> x(n);
  for (int32_t i = 0; i < n; i++) x[i] = amp * static_cast<float>(std::sin(2.0 * M_PI * hz * i / kRate));
  return x;
}
double rms(const std::vector<float>& x, int32_t from, int32_t to) {
  double s = 0;
  for (int32_t i = from; i < to; i++) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / std::max(1, to - from));
}
double peakAbs(const std::vector<float>& x, int32_t from, int32_t to) {
  double p = 0;
  for (int32_t i = from; i < to; i++) p = std::max(p, static_cast<double>(std::fabs(x[i])));
  return p;
}
bool finite(const Buf& b) {
  for (auto& c : b)
    for (float v : c)
      if (!std::isfinite(v)) return false;
  return true;
}
double maxStep(const std::vector<float>& x, int32_t from, int32_t to) {
  double m = 0;
  for (int32_t i = from + 1; i < to; i++) m = std::max(m, static_cast<double>(std::fabs(x[i] - x[i - 1])));
  return m;
}
// Peak / RMS inside a window: high for a defined click, low for a smeared cloud.
double crest(const std::vector<float>& x, int32_t from, int32_t to) {
  const double r = rms(x, from, to);
  return r > 0.0 ? peakAbs(x, from, to) / r : 0.0;
}

void testRegistration() {
  CHECK(fxParamCount(kKind) == 11);
  CHECK(fxParamInfo(kKind, 0)->id == P(NodeParam::DubSpaceEchoOn));
  CHECK(fxParamInfo(kKind, 10)->id == P(NodeParam::DubSpaceMix));
  CHECK(fxKindForNodeParam(P(NodeParam::DubSpaceTone)) == kKind);
  CHECK(fxKindForNodeParam(P(NodeParam::DubSpaceDecay) + 1) == -1);
  CHECK(fxKindForNodeParam(P(NodeParam::CompSidechain)) == static_cast<int32_t>(FxKind::Compressor));
  for (int32_t i : {0, 1, 2}) CHECK(resolveModDomain(*fxParamInfo(kKind, i)) == ModDomain::None);  // switches and time are steps
  CHECK(resolveModDomain(*fxParamInfo(kKind, 5)) != ModDomain::None);                               // size can be modulated
}

// Mix 0, or both sections off: an exact bypass, from the first sample.
void testBypass() {
  for (int mode = 0; mode < 2; mode++) {
    Settings s;
    if (mode == 0) s.mix = 0.0f;
    else s.echo = s.space = false;
    Rig rig(s);
    const auto in = noise(20000);
    Buf d = stereo(in);
    rig.run(d, true);
    CHECK(d[0] == in);
    CHECK(d[1] == in);
  }
  // Switching both off mid-signal ramps out cleanly and then is exact again.
  Settings s;
  Rig rig(s);
  Buf warm = stereo(noise(24000));
  rig.run(warm);
  s.echo = s.space = false;
  rig.apply(s);
  Buf d = stereo(noise(24000, 0.5f, 99));
  rig.run(d, true);
  const auto in = noise(24000, 0.5f, 99);
  CHECK(std::equal(d[0].end() - 8000, d[0].end(), in.end() - 8000));
}

// Echo only: the repeat lands on the beat division (within the tape drift) and nothing sooner.
void testEchoOnly() {
  Settings s;
  s.space = false;
  s.feedback = 0.0f;
  Rig rig(s);
  Buf d = stereo(impulse(40000));
  rig.run(d);
  const int32_t at = kLead + 12000;  // 1/8 at 120 BPM
  CHECK(peakAbs(d[0], 0, at - 12) < 1e-6);
  double best = 0;
  int32_t bi = 0;
  for (int32_t i = at - 12; i < at + 12; i++)
    if (std::fabs(d[0][i]) > best) best = std::fabs(d[0][i]), bi = i;
  CHECK(std::abs(bi - at) <= 4);
  CHECK(best > 0.15);  // tone 16 kHz, character 0: the echo is plainly there (the loop high-pass costs a little)
  CHECK(peakAbs(d[0], at + 8000, 40000) < 1e-4);  // feedback 0: exactly one (the high-pass rings briefly)

  // With feedback: successive repeats, each quieter.
  s.feedback = 0.6f;
  Rig fb(s);
  Buf e = stereo(impulse(80000));
  fb.run(e);
  const double e1 = peakAbs(e[0], at - 20, at + 20), e2 = peakAbs(e[0], 2 * at - kLead - 30, 2 * at - kLead + 30 + 2 * kLead),
               e3 = peakAbs(e[0], 3 * at - 2 * kLead - 40, 3 * at + 40 + 3 * kLead);
  CHECK(e1 > e2 && e2 > e3 && e3 > 0.001);
}

// Mix is additive: the dry signal is never reduced, whatever the mix and the modes.
void testDryIsKept() {
  for (float mix : {0.3f, 1.0f})
    for (bool echo : {false, true})
      for (bool space : {false, true}) {
        Settings s;
        s.echo = echo;
        s.space = space;
        s.mix = mix;
        Rig rig(s);
        const auto in = sine(48000, 220.0, 0.5f);
        Buf raw = stereo(in), wet = raw;
        rig.run(raw, true);
        Rig rig2(s);
        rig2.run(wet);
        for (int32_t i = 0; i < 48000; i += 997) CHECK_NEAR(raw[0][i] - wet[0][i], in[i], 1e-5);  // out - wet == dry
      }
}

// Space only: no repeats, a long dark tail that dies away.
void testSpaceOnly() {
  Settings s;
  s.echo = false;
  s.depth = 1.0f;
  Rig rig(s);
  Buf d = stereo(impulse(48000 * 12));
  rig.run(d);
  CHECK(finite(d));
  const double early = rms(d[0], 4800, 48000), mid = rms(d[0], 48000 * 3, 48000 * 4), late = rms(d[0], 48000 * 9, 48000 * 10);
  CHECK(early > 0.0 && mid > 0.0);
  CHECK(mid < early);
  CHECK(late < mid);
  // Decay 0.35 is a T60 of about 9 s: 60 dB down between the first second and a second ~6 s later, give or take.
  const double db = 20.0 * std::log10(rms(d[0], 48000 * 7, 48000 * 8) / rms(d[0], 48000, 48000 * 2));
  CHECK(db < -20.0 && db > -90.0);

  // Left and right are different networks taps: a mono impulse comes out decorrelated.
  double lr = 0, ll = 0, rr = 0;
  for (int32_t i = 24000; i < 48000 * 3; i++) lr += d[0][i] * d[1][i], ll += d[0][i] * d[0][i], rr += d[1][i] * d[1][i];
  CHECK(std::fabs(lr) / std::sqrt(ll * rr) < 0.35);
}

// Decay sets the tail length; damping darkens it.
void testDecayAndDamping() {
  auto tailDb = [](float decay) {
    Settings s;
    s.echo = false;
    s.depth = 1.0f;
    s.decay = decay;
    Rig rig(s);
    Buf d = stereo(impulse(48000 * 8));
    rig.run(d);
    return 20.0 * std::log10(rms(d[0], 48000 * 5, 48000 * 6) / rms(d[0], 24000, 48000));
  };
  CHECK(tailDb(0.9f) > tailDb(0.5f) + 6.0);
  CHECK(tailDb(0.5f) > tailDb(0.1f) + 6.0);
  // Extremely long: at decay 1 (T60 ~ 15 min) the tail has barely moved after 5 s.
  CHECK(tailDb(1.0f) > -8.0);  // (damping 0.5 still darkens it, which is level in this broadband measure)

  auto brightness = [](float damping) {  // ratio of high-passed (first difference) to raw energy
    Settings s;
    s.echo = false;
    s.depth = 1.0f;
    s.damping = damping;
    Rig rig(s);
    Buf d = stereo(impulse(48000 * 3));
    rig.run(d);
    double a = 0, b = 0;  // the cloud, well after the smear's onset
    for (int32_t i = 48000; i < 96000; i++) a += d[0][i] * d[0][i], b += (d[0][i] - d[0][i - 1]) * (d[0][i] - d[0][i - 1]);
    return b / a;
  };
  std::printf("  brightness %.4f %.4f\n", brightness(0.0f), brightness(1.0f));
  CHECK(brightness(0.0f) > 2.0 * brightness(1.0f));
}

// Level calibration: noise in, space-only wet RMS is about the noise RMS at any size.
void testSpaceLevel() {
  for (float size : {0.0f, 0.5f, 1.0f}) {
    Settings s;
    s.echo = false;
    s.depth = 1.0f;
    s.size = size;
    s.decay = size;  // 0.8 s .. ~15 min
    s.damping = 0.0f;
    Rig rig(s);
    auto in = noise(48000 * 24);
    Buf d = stereo(in);
    rig.run(d);
    const double ratio = rms(d[0], 48000 * 14, 48000 * 24) / rms(in, 48000 * 14, 48000 * 24);
    std::printf("  space level ratio at size %.1f: %.3f\n", size, ratio);
    CHECK(ratio > 0.3 && ratio < 1.5);
  }
}

// The point of the effect: with Space in the loop each repeat is less defined than the one before,
// and more so than the same echo without the space.
void testRepeatsDissolve() {
  const int32_t at = 12000, n = 48000 * 3;
  auto crests = [&](bool space, double* out) {
    Settings s;
    s.space = space;
    s.feedback = 0.8f;
    s.depth = 0.7f;
    s.damping = 0.4f;
    s.character = 0.4f;
    Rig rig(s);
    Buf d = stereo(impulse(n, 0.9f));
    rig.run(d);
    for (int k = 1; k <= 4; k++) out[k] = crest(d[0], kLead + k * at - 600, kLead + k * at + 600);
  };
  double withSpace[5], without[5];
  crests(true, withSpace);
  crests(false, without);
  std::printf("  crest (peak/rms) of repeats 1..4: with space %.1f %.1f %.1f %.1f, echo only %.1f %.1f %.1f %.1f\n", withSpace[1], withSpace[2],
              withSpace[3], withSpace[4], without[1], without[2], without[3], without[4]);
  CHECK(withSpace[1] > withSpace[4] * 1.5);  // the repeats smear progressively
  CHECK(withSpace[4] < without[4]);          // ... and more than the plain echo's
  // The first repeat is still identifiable (a clear peak), not a wash.
  CHECK(withSpace[1] > 4.0);
}

// Character is monotone in what it does to the loop: the second repeat gets darker.
void testCharacterDarkensLoop() {
  auto hf = [](float character) {
    Settings s;
    s.space = false;
    s.feedback = 0.7f;
    s.character = character;
    Rig rig(s);
    Buf d = stereo(noise(48000));  // a noise burst, then silence: the repeats are what follows
    for (int32_t i = 12000; i < 48000; i++) d[0][i] = d[1][i] = 0.0f;
    d[0].resize(48000 * 3, 0.0f);
    d[1].resize(48000 * 3, 0.0f);
    rig.run(d);
    double a = 0, b = 0;
    for (int32_t i = 36000; i < 48000 * 2; i++) a += d[0][i] * d[0][i], b += (d[0][i] - d[0][i - 1]) * (d[0][i] - d[0][i - 1]);
    return b / std::max(a, 1e-12);
  };
  CHECK(hf(1.0f) < hf(0.0f));
}

// No setting runs away: loud sustained input, then silence, must stay bounded and die away.
void testStability() {
  struct Case { float fb, size, depth, damping, character, tone; int32_t time; float decay = 0.6f; };
  const Case cases[] = {
      {0.92f, 1.0f, 1.0f, 0.0f, 0.0f, 16000.0f, 0}, {0.92f, 1.0f, 1.0f, 0.0f, 1.0f, 16000.0f, 1}, {0.92f, 0.0f, 1.0f, 0.0f, 0.0f, 16000.0f, 0},
      {0.92f, 0.5f, 0.5f, 0.3f, 0.5f, 8000.0f, 6},      {0.92f, 1.0f, 0.2f, 1.0f, 1.0f, 200.0f, 5},   {0.92f, 0.7f, 0.9f, 0.1f, 0.0f, 16000.0f, 9},
      {0.92f, 1.0f, 1.0f, 0.0f, 1.0f, 16000.0f, 0, 1.0f},  // decay 1: effectively endless, must stay bounded
  };
  const double freqs[] = {55.0, 220.0, 1800.0};
  for (const Case& c : cases) {
    for (double f : freqs) {
      Settings s;
      s.feedback = c.fb;
      s.size = c.size;
      s.decay = c.decay;
      s.depth = c.depth;
      s.damping = c.damping;
      s.character = c.character;
      s.tone = c.tone;
      s.time = c.time;
      Rig rig(s);
      const int32_t sus = 48000 * 4, tail = 48000 * 24;
      auto in = sine(sus + tail, f, 0.9f);
      for (int32_t i = sus; i < sus + tail; i++) in[i] = 0.0f;
      for (int32_t i = 0; i < sus; i += 7) in[i] += 0.3f * (static_cast<float>((i * 2654435761u) >> 16 & 0xff) / 128.0f - 1.0f);
      Buf d = stereo(in);
      rig.run(d);
      CHECK(finite(d));
      CHECK(peakAbs(d[0], 0, sus + tail) < 4.0);
      CHECK(peakAbs(d[1], 0, sus + tail) < 4.0);
      const double a = rms(d[0], sus + 48000 * 2, sus + 48000 * 6), b = rms(d[0], sus + tail - 48000 * 4, sus + tail);
      if (c.decay <= 0.9f && !(b < a * 0.7 || b < 1e-4)) std::printf("  not decaying: fb %.2f size %.1f f %.0f: %.5f -> %.5f\n", c.fb, c.size, f, a, b);
      CHECK(c.decay > 0.9f || b < a * 0.7 || b < 1e-4);
    }
  }
}

// Moving Time while audio runs bends the pitch smoothly: no step bigger than the signal's own slope.
void testTimeGlide() {
  Settings s;
  s.space = false;
  s.feedback = 0.0f;
  s.mix = 1.0f;
  s.time = 0;
  Rig rig(s);
  const int32_t n = 48000 * 4;
  Buf d = stereo(sine(n, 220.0, 0.5f));
  Buf a = {std::vector<float>(d[0].begin(), d[0].begin() + 48000), std::vector<float>(d[0].begin(), d[0].begin() + 48000)};
  rig.run(a);
  rig.set(NodeParam::DubSpaceTime, 5.0f);  // 1/16 -> 1/2: a 1.5 s longer delay
  Buf b = {std::vector<float>(d[0].begin() + 48000, d[0].end()), std::vector<float>(d[0].begin() + 48000, d[0].end())};
  rig.run(b);
  const double slope = 0.5 * 2.0 * M_PI * 220.0 / kRate;  // the sine's own steepest step
  CHECK(maxStep(b[0], 0, static_cast<int32_t>(b[0].size())) < slope * 1.8);
  // The delay really did move, by gliding: a pitch shift of ~-30% is visible in the zero-crossing rate early on.
  auto crossings = [](const std::vector<float>& x, int32_t from, int32_t to) {
    int32_t c = 0;
    for (int32_t i = from + 1; i < to; i++) c += (x[i - 1] < 0) != (x[i] < 0);
    return c;
  };
  const int32_t base = crossings(a[0], 12000, 36000);
  const int32_t gliding = crossings(b[0], 1000, 12000) * 24000 / 11000;
  CHECK(gliding < base * 0.9 || gliding > base * 1.1);
}

// The switches ramp: toggling either section on a loud signal never steps.
void testSwitchesAreClickFree() {
  Settings s;
  s.feedback = 0.5f;
  Rig rig(s);
  const double slope = 0.5 * 2.0 * M_PI * 220.0 / kRate;
  Buf warm = stereo(sine(48000, 220.0, 0.5f));
  rig.run(warm);
  for (int k = 0; k < 6; k++) {
    s.echo = (k & 1) == 0 ? !s.echo : s.echo;
    s.space = (k & 1) == 1 ? !s.space : s.space;
    rig.apply(s);
    Buf d = stereo(sine(24000, 220.0, 0.5f));
    rig.run(d);
    CHECK(finite(d));
    if (maxStep(d[0], 0, 24000) >= slope * 6.0) std::printf("  toggle %d echo %d space %d: step %.4f vs slope %.4f\n", k, s.echo, s.space, maxStep(d[0], 0, 24000), slope);
    CHECK(maxStep(d[0], 0, 24000) < slope * 6.0);
  }
}

// The four configurations differ audibly and mono input / output works.
void testConfigurations() {
  const int32_t n = 48000 * 3;
  double energyAfter[4];
  int idx = 0;
  for (bool echo : {false, true})
    for (bool space : {false, true}) {
      Settings s;
      s.echo = echo;
      s.space = space;
      s.depth = 0.8f;
      Rig rig(s);
      Buf d = stereo(impulse(n));
      rig.run(d);
      CHECK(finite(d));
      energyAfter[idx++] = rms(d[0], 48000 * 2, n);
    }
  // [0]=off,off  [1]=off,space  [2]=echo,off  [3]=echo,space
  CHECK(energyAfter[0] == 0.0);  // both off: no wet at all
  CHECK(energyAfter[1] > 0.0);
  CHECK(energyAfter[3] > energyAfter[2]);  // the space adds a tail the plain echo does not have

  Settings s;
  Rig mono(s);
  Buf m = {noise(24000)};
  mono.run(m);
  CHECK(finite(m));
  CHECK(rms(m[0], 12000, 24000) > 0.0);
}

// Sample rates other than 48 kHz scale the tempo and the space.
void testOtherRates() {
  FxChain chain;
  chain.configure(44100.0);
  chain.setClock(0, 0.0, 44100.0 * 60.0 / 120.0);
  chain.add(1, kKind, -1);
  chain.setParam(1, P(NodeParam::DubSpaceMix), 1.0f);
  chain.setParam(1, P(NodeParam::DubSpaceFeedback), 0.9f);
  std::vector<float> x = noise(44100 * 2);
  for (int32_t at = 0; at < static_cast<int32_t>(x.size()); at += kBlock) {
    float* ch[1] = {x.data() + at};
    chain.process(ch, 1, std::min<int32_t>(kBlock, static_cast<int32_t>(x.size()) - at));
  }
  CHECK(std::isfinite(rms(x, 0, static_cast<int32_t>(x.size()))));
  CHECK(peakAbs(x, 0, static_cast<int32_t>(x.size())) < 4.0);
}

}  // namespace

int main() {
  testRegistration();
  testBypass();
  testDryIsKept();
  testEchoOnly();
  testSpaceOnly();
  testDecayAndDamping();
  testSpaceLevel();
  testRepeatsDissolve();
  testCharacterDarkensLoop();
  testStability();
  testTimeGlide();
  testSwitchesAreClickFree();
  testConfigurations();
  testOtherRates();
  std::printf("dub_space_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
