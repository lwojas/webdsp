#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "matrix_filter.h"  // MatrixFilter::soft, the repo's rational tanh

// Character Filter: a resonant filter built to have a personality, not to be transparent. Design and
// measurements: docs/character-filter.md. It is our own architecture; it does not model any
// particular commercial filter.
//
//   x -> drive -> [ stage1 -> stage2 -> stage3 -> stage4 ] -> pole mix -> DC block -> out
//                   ^  each stage saturates its own input        |
//                   +-- feedback: high-pass -> compress <- y4 ---+
//
// Four trapezoidal (zero-delay) one-pole stages in a feedback loop, the ladder family, so LP 24 dB
// is the natural voice and the other modes are pole mixes of the same stages:
//   mode 0  LP 24 dB    y4
//   mode 1  LP 12 dB    y2
//   mode 2  BP          y2 - 2 y3 + y4 (12 dB per side, unity at the centre)
//   mode 3  HP 24 dB    u - 4 y1 + 6 y2 - 4 y3 + y4
//
// What makes it an instrument rather than a utility:
//  - The nonlinearity is inside the filter. Every stage soft-saturates its input, so how much the
//    filter colours depends on how hard it is fed and where the energy sits relative to the cutoff:
//    lows hit stage 1 hard, highs have already been filtered out by stage 3.
//  - Resonance is compressive. The feedback is soft-limited, so a loud input pulls the resonant
//    peak down and a quiet one lets it ring (touch sensitivity), and at the top of the knob the
//    limit sets the amplitude of a stable self-oscillation, a sine-ish tone tuned to the cutoff
//    that is shaped, not clipped, by the stages.
//  - Asymmetry. Character biases stage 1, which adds even harmonics (warmth) with no DC out.
//  - Bass keeping. Character moves the feedback high-pass: low, the resonance eats the low end the
//    way a classic ladder does; high, the low end stays and only the peak is added.
//
// Controls (all 0..1, smoothed, safe to move at audio rate): resonance, drive, character; mode is
// stepped (crossfaded over a few ms). Cutoff is in Hz and is smoothed in the octave domain, so
// envelopes, LFOs and keyboard tracking act in octaves and fast sweeps stay free of zipper noise.
// The filter is only responsible for filtering: keytracking and envelopes are computed by the
// caller (FilterModulation::cutoffHz, trackedCutoff()) and arrive as the cutoff.
//
// Quality: the whole loop runs at 2x (31-tap half-band up/down, ~0.3 ms latency), which both keeps
// the cutoff accurate up to ~20 kHz and halves the aliasing of the saturators. Everything is
// bounded by construction (saturated stage inputs, convex stage updates, limited feedback, a final
// soft limit), so no valid parameter combination, step or NaN input can blow it up; a non-finite
// result resets the filter. A silent input decays to exact silence unless the filter is
// self-oscillating, in which case it keeps ringing until the resonance is lowered. No allocation.
namespace webdsp {

// Half-band 2x up/down sampler (Kaiser-windowed sinc, 31 taps, ~75 dB stopband). The two directions
// together delay the signal by 15 input samples.
class Oversampler2x {
 public:
  Oversampler2x() {
    design();
    reset();
  }

  void reset() {
    for (float& v : hx_) v = 0.0f;
    for (float& v : hu_) v = 0.0f;
  }

  // One input sample in, two oversampled samples out (a first, then b).
  void up(float x, float& a, float& b) {
    for (int32_t j = kHistory - 1; j > 0; j--) hx_[j] = hx_[j - 1];
    hx_[0] = x;
    float acc = 0.0f;
    for (int32_t j = 0; j < kHistory; j++) acc += upTaps_[j] * hx_[j];
    a = acc;
    b = 2.0f * centre_ * hx_[kK];  // the odd phase of a half-band is a pure delay
  }

  // Two oversampled samples in (older first), one input-rate sample out.
  float down(float older, float newer) {
    for (int32_t j = kTaps - 1; j > 1; j--) hu_[j] = hu_[j - 2];
    hu_[1] = older;
    hu_[0] = newer;
    float acc = centre_ * hu_[kCentreIndex];
    for (int32_t j = 0; j < kTaps; j += 2) acc += taps_[j] * hu_[j];
    return acc;
  }

 private:
  static constexpr int32_t kK = 7;
  static constexpr int32_t kTaps = 4 * kK + 3;          // 31
  static constexpr int32_t kCentreIndex = 2 * kK + 1;   // 15
  static constexpr int32_t kHistory = kTaps / 2 + 1;    // 16 input samples for the even phase

  static double bessel0(double x) {
    double sum = 1.0, term = 1.0;
    for (int32_t k = 1; k < 40; k++) {
      term *= (x / (2.0 * k)) * (x / (2.0 * k));
      sum += term;
    }
    return sum;
  }

  void design() {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kBeta = 8.0;
    double h[kTaps] = {};
    double sum = 0.0;
    for (int32_t i = 0; i < kTaps; i++) {
      const int32_t m = i - kCentreIndex;
      double v = 0.0;
      if (m == 0) {
        v = 0.5;
      } else if ((m & 1) != 0) {
        const double r = static_cast<double>(m) / kCentreIndex;
        const double w = bessel0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(kBeta);
        v = std::sin(kPi * m / 2.0) / (kPi * m) * w;
      }
      h[i] = v;
      sum += v;
    }
    for (int32_t i = 0; i < kTaps; i++) taps_[i] = static_cast<float>(h[i] / sum);
    // Even-phase up-sampler taps: 2 * h[2j] applied to x[m - j].
    for (int32_t j = 0; j < kHistory; j++) upTaps_[j] = 2.0f * taps_[2 * j];
    centre_ = taps_[kCentreIndex];
    taps_[kCentreIndex] = 0.0f;  // the centre is added separately in down()
  }

  float centre_ = 0.5f;
  float taps_[kTaps] = {};
  float upTaps_[kHistory] = {};
  float hx_[kHistory] = {};
  float hu_[kTaps] = {};
};

class CharacterFilter {
 public:
  static constexpr int32_t kModes = 4;
  static constexpr int32_t kControlBlock = 8;  // input samples per control step (~0.17 ms at 48 kHz)

  void configure(double sampleRate) {
    sampleRate_ = sampleRate;
    maxHz_ = static_cast<float>(std::min(20000.0, 0.45 * sampleRate));
    reset();
  }

  void reset() {
    s1_ = s2_ = s3_ = s4_ = 0.0f;
    fbLp_ = 0.0f;
    dcX_ = dcY_ = 0.0f;
    os_.reset();
    snap_ = true;  // the next block jumps to its targets: a fresh note has nothing to protect
  }

  void setMode(int32_t mode) { mode_ = std::clamp(mode, 0, kModes - 1); }
  void setResonance(float r) { resonance_ = clean01(r, resonance_); }
  void setDrive(float d) { drive_ = clean01(d, drive_); }
  void setCharacter(float c) { character_ = clean01(c, character_); }

  // Keyboard tracking: scales a base cutoff by the note's distance from middle C, `amount` 0..1
  // (1 = the cutoff follows the note one octave per octave). A pure helper; the filter holds no
  // note state.
  static float trackedCutoff(float baseHz, float noteHz, float amount) {
    if (!(noteHz > 0.0f) || !std::isfinite(noteHz) || !std::isfinite(amount)) return baseHz;
    return baseHz * std::pow(noteHz / 261.6256f, std::clamp(amount, 0.0f, 1.0f));
  }

  // Filters `n` samples in place. `cutoffHz` is the cutoff for this call (clamped 20 Hz .. ~20 kHz,
  // NaN-safe); call with short blocks to modulate it faster, the filter glides between values.
  void process(float* buf, int32_t n, float cutoffHz) {
    const float target = std::isfinite(cutoffHz) ? cutoffHz : 1000.0f;
    cutTarget_ = std::log2(std::clamp(target, kMinHz, maxHz_));
    int32_t pos = 0;
    while (pos < n) {
      const int32_t m = std::min(kControlBlock, n - pos);
      step(buf + pos, m);
      pos += m;
    }
  }

 private:
  static constexpr float kMinHz = 20.0f;
  static constexpr float kLimitKnee = 0.7f;
  static constexpr float kScale = 2.0f;        // input 0.5 reaches the saturators' knee
  static constexpr float kMaxLoopGain = 4.8f;  // 4 = self-oscillation in a linear ladder
  static constexpr float kMaxDriveDb = 24.0f;
    static constexpr int32_t kStopsPerMode = 5;
  static constexpr float kRetune = 0.88f;      // measured: the analytic retune overshoots slightly
  static constexpr float kFbCorner = 0.4f;     // feedback high-pass corner, as a fraction of the cutoff

  // Character knots: Clean, Warm, Punchy, Aggressive at 0, 1/3, 2/3, 1.
  struct Voice {
    float hardness;  // stage saturator gain: how early the stages bend
    float bias;      // stage-1 asymmetry (even harmonics)
    float fbLimit;   // feedback soft-limit: low = more compression, buzzier self-oscillation
    float bassKeep;  // feedback high-pass amount: how much low end the resonance leaves alone
    float driveComp; // share of the drive gain taken back at the output (hard stages have saturated already)
    float trim;      // level trim so the voices sit at similar loudness at drive 0
    float oscLevel;  // output gain at the top of the resonance range, so a self-oscillation has a similar level
  };
  static constexpr Voice kVoices[4] = {
      {0.22f, 0.00f, 6.0f, 0.00f, 0.65f, 1.00f, 0.32f},  // Clean: nearly linear, a very good ladder
      {0.75f, 0.42f, 3.0f, 0.15f, 0.40f, 1.05f, 1.00f},  // Warm: soft stages, even harmonics
      {1.15f, 0.28f, 1.7f, 0.75f, 0.25f, 1.20f, 1.90f},  // Punchy: tighter loop, the low end stays
      {1.90f, 0.06f, 0.9f, 0.45f, 0.12f, 1.40f, 2.30f},  // Aggressive: hard stages, compressed resonance
  };
  static constexpr float kModeWeights[kModes][kStopsPerMode] = {
      {0, 0, 0, 0, 1},       // LP 24: y4
      {0, 0, 1, 0, 0},       // LP 12: y2
      {0, 0, 4, -8, 4},      // BP:    y2 - 2 y3 + y4, unity at the centre
      {1, -4, 6, -4, 1},     // HP 24: (1 - H)^4 expanded over u, y1..y4
  };

  static float clean01(float v, float fallback) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : fallback;
  }
  // The output safety limit: untouched up to kLimitKnee, then a smooth knee that reaches exactly +-1.
  // (The colour comes from the stages; the limiter must not add any below the knee.)
  static float outputLimit(float x) {
    const float a = std::fabs(x);
    if (a <= kLimitKnee) return x;
    const float y = kLimitKnee + (1.0f - kLimitKnee) * MatrixFilter::soft((a - kLimitKnee) / (1.0f - kLimitKnee));
    return x < 0.0f ? -y : y;
  }
  static float flush(float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; }
  // 0 below resonance 0.8, 1 from 0.95: where the loop is at or near self-oscillation.
  static float oscZone(float r) {
    const float t = std::clamp((r - 0.8f) / 0.15f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }
  static float lerp(float a, float b, float t) { return a + (b - a) * t; }

  static Voice voiceAt(float c) {
    const float x = std::clamp(c, 0.0f, 1.0f) * 3.0f;
    const int32_t i = std::min(static_cast<int32_t>(x), 2);
    const float t = x - static_cast<float>(i);
    const Voice& a = kVoices[i];
    const Voice& b = kVoices[i + 1];
    return {lerp(a.hardness, b.hardness, t), lerp(a.bias, b.bias, t), lerp(a.fbLimit, b.fbLimit, t),
            lerp(a.bassKeep, b.bassKeep, t), lerp(a.driveComp, b.driveComp, t), lerp(a.trim, b.trim, t),
            lerp(a.oscLevel, b.oscLevel, t)};
  }

  // Stage-1 saturator: biased (asymmetric) and re-centred so zero in is zero out, normalised so the
  // small-signal gain is exactly 1 (the filter's linear response does not move with the character).
  struct Shaper {
    float h = 1.0f, bias = 0.0f, offset = 0.0f, norm = 1.0f;
    void set(float hardness, float b) {
      h = hardness;
      bias = b;
      offset = MatrixFilter::soft(b);
      const float e = 1e-3f;
      const float slope = (MatrixFilter::soft(b + e) - MatrixFilter::soft(b - e)) / (2.0f * e);
      norm = 1.0f / (h * slope);
    }
    float operator()(float x) const { return (MatrixFilter::soft(h * x + bias) - offset) * norm; }
  };

  void step(float* buf, int32_t m) {
    // --- control rate: smooth everything toward its target, derive loop constants ---
    const float srF = static_cast<float>(sampleRate_);
    if (snap_) {
      cutLog_ = cutTarget_;
      resS_ = resonance_;
      driveS_ = drive_;
      charS_ = character_;
      for (int32_t j = 0; j < kStopsPerMode; j++) wS_[j] = kModeWeights[mode_][j];
      G_ = gFor(cutLog_);
      snap_ = false;
    } else {
      const float aCut = 1.0f - std::exp(-static_cast<float>(m) / (srF * 0.0004f));
      const float aPar = 1.0f - std::exp(-static_cast<float>(m) / (srF * 0.004f));
      const float aMode = 1.0f - std::exp(-static_cast<float>(m) / (srF * 0.006f));
      cutLog_ += aCut * (cutTarget_ - cutLog_);
      resS_ += aPar * (resonance_ - resS_);
      driveS_ += aPar * (drive_ - driveS_);
      charS_ += aPar * (character_ - charS_);
      for (int32_t j = 0; j < kStopsPerMode; j++) wS_[j] += aMode * (kModeWeights[mode_][j] - wS_[j]);
    }
    const Voice v = voiceAt(charS_);
    // Resonance: gentle at the bottom, reaches self-oscillation (k > 4) in the top ~10 %.
    // The feedback high-pass at the cutoff has gain < 1 and a phase lead; undo both so the loop still
    // closes at the cutoff (the self-oscillation stays on pitch and keeps its margin).
    constexpr float kX = 1.0f / kFbCorner;
    const float hpRe = 1.0f - v.bassKeep / (1.0f + kX * kX);
    const float hpIm = v.bassKeep * kX / (1.0f + kX * kX);
    const float lead = std::atan2(hpIm, hpRe);
    const float theta = 0.78539816f + 0.25f * lead;  // phase each stage must lag at the new crossing
    const float retune = kRetune * std::log2(std::tan(theta));
    const float cosT = std::cos(theta);
    const float loopComp = 0.25f / (cosT * cosT * cosT * cosT) / std::sqrt(hpRe * hpRe + hpIm * hpIm);
    const float k = kMaxLoopGain * resS_ * (0.35f + 0.65f * resS_) * loopComp;
    shaper1_.set(v.hardness, v.bias);
    const float h2 = v.hardness * 0.7f;
    const float h2inv = 1.0f / h2;
    const float driveDb = kMaxDriveDb * std::pow(driveS_, 1.4f);
    const float driveGain = std::exp2(driveDb * 0.16609640f);  // dB -> 2^(dB/6.02)
    const float inGain = kScale * driveGain;
    const float zone = oscZone(resS_);
    const float outGain = (1.0f / kScale) * v.trim * (1.0f + zone * (v.oscLevel - 1.0f)) /
                          std::pow(driveGain, v.driveComp);
    // The loop gives back less passband level than it takes; return most of it, less where the
    // feedback high-pass already keeps the lows.
    const float resComp = 1.0f + 0.55f * k * (1.0f - 0.8f * v.bassKeep);
    const float gEnd = gFor(cutLog_ - retune);
    const float gStep = (gEnd - G_) / static_cast<float>(m);
    const float bassKeep = v.bassKeep;
    const float limit = v.fbLimit;
    const float limitInv = 1.0f / limit;
    const float dcR = 1.0f - 6.2831853f * 8.0f / srF;

    for (int32_t i = 0; i < m; i++) {
      float x = buf[i];
      if (!std::isfinite(x)) x = 0.0f;
      float a, b;
      os_.up(x * inGain, a, b);
      G_ += gStep;
      const float ya = tick(a, G_, k, bassKeep, limit, limitInv, h2, h2inv);
      const float yb = tick(b, G_, k, bassKeep, limit, limitInv, h2, h2inv);
      float y = os_.down(ya, yb) * outGain * resComp;

      // DC block (stage-1 asymmetry and any offset), then a final soft limit.
      const float dc = y - dcX_ + dcR * dcY_;
      dcX_ = y;
      dcY_ = std::clamp(flush(dc), -4.0f, 4.0f);
      y = outputLimit(dcY_);
      if (!std::isfinite(y)) {
        reset();
        y = 0.0f;
      }
      buf[i] = y;
    }
  }

  float gFor(float cutLog2) const {
    const double hz = std::exp2(static_cast<double>(cutLog2));
    const double g = std::tan(3.14159265358979323846 * hz / (2.0 * sampleRate_));  // at the 2x rate
    return static_cast<float>(g / (1.0 + g));
  }

  // One oversampled step of the loop; returns the pole-mixed output.
  float tick(float xin, float G, float k, float bassKeep, float limit, float limitInv, float h2,
             float h2inv) {
    const float G2 = G * G;
    const float G4 = G2 * G2;
    const float S = (1.0f - G) * (G2 * G * s1_ + G2 * s2_ + G * s3_ + s4_);
    // Linear zero-delay solve for the loop input, used to predict the fed-back y4.
    const float uLin = (xin + bassKeep * fbLp_ - k * S) / (1.0f + k * G4);
    const float y4p = G4 * uLin + S;
    // Feedback: high-passed (keeps the lows), then compressed. Small signals pass at unity.
    const float z = k * y4p - bassKeep * fbLp_;
    fbLp_ += fbA(G) * (k * y4p - fbLp_);
    const float fb = limit * MatrixFilter::soft(z * limitInv);
    const float u = xin - fb;

    const float y1 = G * shaper1_(u) + (1.0f - G) * s1_;
    const float y2 = G * (MatrixFilter::soft(h2 * y1) * h2inv) + (1.0f - G) * s2_;
    const float y3 = G * (MatrixFilter::soft(h2 * y2) * h2inv) + (1.0f - G) * s3_;
    const float y4 = G * (MatrixFilter::soft(h2 * y3) * h2inv) + (1.0f - G) * s4_;
    s1_ = flush(2.0f * y1 - s1_);
    s2_ = flush(2.0f * y2 - s2_);
    s3_ = flush(2.0f * y3 - s3_);
    s4_ = flush(2.0f * y4 - s4_);
    return wS_[0] * u + wS_[1] * y1 + wS_[2] * y2 + wS_[3] * y3 + wS_[4] * y4;
  }

  // One-pole coefficient of the feedback high-pass's low-pass: a fraction of the stage rate, so the
  // high-pass corner follows the cutoff.
  static float fbA(float G) { return 2.0f * kFbCorner * G; }

  double sampleRate_ = 48000.0;
  float maxHz_ = 20000.0f;
  int32_t mode_ = 0;
  float resonance_ = 0.0f, drive_ = 0.0f, character_ = 0.4f;
  float cutTarget_ = 10.0f, cutLog_ = 10.0f;
  float resS_ = 0.0f, driveS_ = 0.0f, charS_ = 0.4f;
  float wS_[kStopsPerMode] = {0, 0, 0, 0, 1};
  float G_ = 0.1f;
  bool snap_ = true;
  Shaper shaper1_;
  float s1_ = 0.0f, s2_ = 0.0f, s3_ = 0.0f, s4_ = 0.0f, fbLp_ = 0.0f;
  float dcX_ = 0.0f, dcY_ = 0.0f;
  Oversampler2x os_;
};

}  // namespace webdsp
