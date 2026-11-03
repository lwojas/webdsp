#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// The filter of the Matrix synth (instruments/matrix.h): one resonant filter that can be retuned on
// every sample, because the patch matrix can drive its cutoff from audio (the oscillator, noise,
// or the filter's own output). Both topologies are zero-delay-feedback (trapezoidal) designs, which
// stay well behaved when the coefficient moves at audio rate.
//
//   mode 0  LP 24 dB  four one-pole stages in a feedback loop (Moog-style ladder). Resonance
//                     0..1 maps to loop gain 0..4; at 1 the ladder rings on its own.
//   mode 1  LP 12 dB  state-variable filter, low-pass output
//   mode 2  BP        state-variable filter, band-pass output
//   mode 3  HP 12 dB  state-variable filter, high-pass output
//
// Everything is bounded: the signal entering the stages is soft-limited to +-1, so each stage output
// (a convex mix of that input and its own state) cannot leave +-1, and the state-variable
// integrators are clamped. A silent input decays to silence. No allocation.
namespace webdsp {

class MatrixFilter {
 public:
  static constexpr int32_t kModes = 4;

  void configure(double sampleRate) {
    sampleRate_ = sampleRate;
    reset();
  }

  void reset() {
    s1_ = s2_ = s3_ = s4_ = 0.0f;
    ic1_ = ic2_ = 0.0f;
  }

  // Bounded smooth saturation: ~x below 0.5, exactly +-1 from |x| >= 3.
  static float soft(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
  }

  // One sample. `cutoffHz` is clamped to 20 Hz .. 0.45 x sampleRate, `resonance` and `drive` to
  // 0..1. Returns the filtered sample, soft-limited to +-1.
  float process(float x, float cutoffHz, float resonance, float drive, int32_t mode) {
    if (!std::isfinite(x)) x = 0.0f;
    const double fc = std::clamp(static_cast<double>(std::isfinite(cutoffHz) ? cutoffHz : 1000.0f),
                                 20.0, 0.45 * sampleRate_);
    const float res = std::clamp(resonance, 0.0f, 1.0f);
    const float gain = 1.0f + 7.0f * drive * drive;  // drive 0 is transparent
    const float in = drive > 0.0f ? soft(x * gain) : x;
    const float g = static_cast<float>(std::tan(3.14159265358979323846 * fc / sampleRate_));
    float y;
    if (mode == 0) {
      y = ladder(in, g, res);
    } else {
      y = svf(in, g, res, mode);
    }
    return soft(y);
  }

 private:
  float ladder(float x, float g, float res) {
    const float G = g / (1.0f + g);
    const float k = 4.0f * res;
    const float G2 = G * G;
    const float G4 = G2 * G2;
    // y4 = G^4 u + S, with S gathered from the four integrator states; solve u = x - k y4.
    const float S = (1.0f - G) * (G2 * G * s1_ + G2 * s2_ + G * s3_ + s4_);
    float u = (x - k * S) / (1.0f + k * G4);
    u = soft(u);
    const float y1 = G * u + (1.0f - G) * s1_;
    const float y2 = G * y1 + (1.0f - G) * s2_;
    const float y3 = G * y2 + (1.0f - G) * s3_;
    const float y4 = G * y3 + (1.0f - G) * s4_;
    s1_ = flush(2.0f * y1 - s1_);
    s2_ = flush(2.0f * y2 - s2_);
    s3_ = flush(2.0f * y3 - s3_);
    s4_ = flush(2.0f * y4 - s4_);
    // The loop takes away passband level as resonance rises; give most of it back.
    return y4 * (1.0f + 0.5f * k);
  }

  float svf(float x, float g, float res, int32_t mode) {
    const float k = 2.0f - 1.95f * res;  // damping: 2 (no peak) .. 0.05 (Q 20)
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = x - ic2_;
    const float v1 = a1 * ic1_ + a2 * v3;
    const float v2 = ic2_ + a2 * ic1_ + a3 * v3;
    ic1_ = std::clamp(flush(2.0f * v1 - ic1_), -4.0f, 4.0f);
    ic2_ = std::clamp(flush(2.0f * v2 - ic2_), -4.0f, 4.0f);
    if (mode == 2) return v1 * k;  // band-pass, unity gain at the centre
    if (mode == 3) return x - k * v1 - v2;
    return v2;
  }

  static float flush(float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; }

  double sampleRate_ = 48000.0;
  float s1_ = 0.0f, s2_ = 0.0f, s3_ = 0.0f, s4_ = 0.0f;
  float ic1_ = 0.0f, ic2_ = 0.0f;
};

}  // namespace webdsp
