#pragma once
#include <algorithm>
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// EQ: four serial parametric bands, each a standard RBJ ("Audio EQ Cookbook") biquad.
//
//   in -> band 1 -> band 2 -> band 3 -> band 4 -> out
//
// Band types (EqBandType): bell, low shelf, high shelf, low-pass, high-pass, notch. Which
// parameters a type uses: bell gain+Q; shelves gain+shelfSlope; low/high-pass slope (12 or 24
// dB/oct, built like BiquadFilter: one Butterworth biquad, or two with the 4th-order Q split);
// notch Q. Frequency is used by all. Unused parameters are stored but ignored, so switching a
// band's type back and forth keeps its other settings.
//
// Everything is explicit scalar state (4 x 7 NodeParams, see params.h): a client can save, restore
// and inspect the whole EQ from those values, and draw the response from them alone (the curve is
// a client concern; src/runtime/eq.ts mirrors this file's coefficient maths for that).
//
// A band is skipped while disabled; all bands disabled (the default) is a bit-exact pass-through.
// Coefficients are recomputed once per block for bands whose parameters changed (setParam only
// marks the band dirty), not per sample. Like BiquadFilter, coefficients change in one step and
// the transposed direct-form II state carries over; there is no coefficient smoothing.
namespace webdsp {

enum class EqBandType : int32_t { Bell = 0, LowShelf = 1, HighShelf = 2, LowPass = 3, HighPass = 4, Notch = 5 };

inline constexpr int32_t kEqBandCount = 4;
inline constexpr int32_t kEqParamsPerBand = 7;
inline constexpr float kEqMinHz = 20.0f;
inline constexpr float kEqMaxHz = 20000.0f;
inline constexpr float kEqMaxGainDb = 18.0f;

class Eq final : public DSPNode {
 public:
  explicit Eq(double sampleRate) : sampleRate_(sampleRate) {
    // Defaults mirror kEqFxParams (fx_params.h): a low shelf, two bells and a high shelf, all off.
    bands_[0].type = EqBandType::LowShelf;
    bands_[0].freq = 100.0f;
    bands_[1].freq = 500.0f;
    bands_[2].freq = 2000.0f;
    bands_[3].type = EqBandType::HighShelf;
    bands_[3].freq = 8000.0f;
    for (Band& b : bands_) design(b);
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    const int32_t nc = std::min(numChannels, kMaxDspChannels);
    for (Band& b : bands_) {
      if (!b.enabled) continue;
      if (b.dirty) design(b);
      for (int32_t c = 0; c < nc; c++) {
        float* buf = channels[c];
        for (int32_t s = 0; s < b.sections; s++) {
          const Coeffs& k = b.k[s];
          double& z1 = b.z[c][s][0];
          double& z2 = b.z[c][s][1];
          for (int32_t i = 0; i < numFrames; i++) {
            const double in = buf[i];
            const double out = k.b0 * in + z1;
            z1 = k.b1 * in + z2 - k.a1 * out;
            z2 = k.b2 * in - k.a2 * out;
            buf[i] = static_cast<float>(out);
          }
          // A decayed tail must not linger in denormal range (slow arithmetic on some CPUs).
          if (std::fabs(z1) < 1e-20) z1 = 0.0;
          if (std::fabs(z2) < 1e-20) z2 = 0.0;
        }
      }
    }
  }

  void setParam(int32_t param, float value) override {
    const int32_t rel = param - static_cast<int32_t>(NodeParam::EqBand1On);
    if (rel < 0 || rel >= kEqBandCount * kEqParamsPerBand || !std::isfinite(value)) return;
    Band& b = bands_[rel / kEqParamsPerBand];
    switch (rel % kEqParamsPerBand) {
      case 0: {
        const bool on = value >= 0.5f;
        if (on && !b.enabled) clearState(b);  // re-enabling starts clean, like FxChain::setEnabled
        b.enabled = on;
        break;
      }
      case 1:
        b.type = static_cast<EqBandType>(std::clamp(static_cast<int32_t>(std::lround(value)), 0, 5));
        break;
      case 2: b.freq = std::clamp(value, kEqMinHz, kEqMaxHz); break;
      case 3: b.gainDb = std::clamp(value, -kEqMaxGainDb, kEqMaxGainDb); break;
      case 4: b.q = std::clamp(value, 0.1f, 18.0f); break;
      case 5: b.shelfSlope = std::clamp(value, 0.1f, 1.0f); break;
      case 6: b.slope24 = value >= 18.0f; break;
    }
    b.dirty = true;
  }

  void reset() override {
    for (Band& b : bands_) clearState(b);
  }

 private:
  struct Coeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  };

  struct Band {
    bool enabled = false;
    EqBandType type = EqBandType::Bell;
    float freq = 1000.0f, gainDb = 0.0f, q = 0.707f, shelfSlope = 1.0f;
    bool slope24 = false;
    bool dirty = true;
    int32_t sections = 1;  // biquads in use: 2 only for a 24 dB/oct low/high-pass
    Coeffs k[2];
    double z[kMaxDspChannels][2][2] = {};  // [channel][section][z1, z2], transposed DF2
  };

  static void clearState(Band& b) {
    for (auto& ch : b.z) {
      for (auto& sec : ch) sec[0] = sec[1] = 0.0;
    }
  }

  // RBJ cookbook, normalised by a0. `s` is the shelf slope S, `A` = 10^(dB/40).
  static Coeffs rbj(EqBandType type, double omega, double q, double gainDb, double s) {
    const double sinO = std::sin(omega), cosO = std::cos(omega);
    const double A = std::pow(10.0, gainDb / 40.0);
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
      case EqBandType::Bell: {
        const double alpha = sinO / (2.0 * q);
        b0 = 1.0 + alpha * A; b1 = -2.0 * cosO; b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A; a1 = -2.0 * cosO; a2 = 1.0 - alpha / A;
        break;
      }
      case EqBandType::LowShelf:
      case EqBandType::HighShelf: {
        const double t = 2.0 * std::sqrt(A) * (sinO / 2.0) * std::sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0);
        if (type == EqBandType::LowShelf) {
          b0 = A * ((A + 1.0) - (A - 1.0) * cosO + t);
          b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosO);
          b2 = A * ((A + 1.0) - (A - 1.0) * cosO - t);
          a0 = (A + 1.0) + (A - 1.0) * cosO + t;
          a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosO);
          a2 = (A + 1.0) + (A - 1.0) * cosO - t;
        } else {
          b0 = A * ((A + 1.0) + (A - 1.0) * cosO + t);
          b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosO);
          b2 = A * ((A + 1.0) + (A - 1.0) * cosO - t);
          a0 = (A + 1.0) - (A - 1.0) * cosO + t;
          a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosO);
          a2 = (A + 1.0) - (A - 1.0) * cosO - t;
        }
        break;
      }
      case EqBandType::LowPass: {
        const double alpha = sinO / (2.0 * q);
        b0 = (1.0 - cosO) / 2.0; b1 = 1.0 - cosO; b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosO; a2 = 1.0 - alpha;
        break;
      }
      case EqBandType::HighPass: {
        const double alpha = sinO / (2.0 * q);
        b0 = (1.0 + cosO) / 2.0; b1 = -(1.0 + cosO); b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosO; a2 = 1.0 - alpha;
        break;
      }
      case EqBandType::Notch:
      default: {
        const double alpha = sinO / (2.0 * q);
        b0 = 1.0; b1 = -2.0 * cosO; b2 = 1.0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosO; a2 = 1.0 - alpha;
        break;
      }
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
  }

  void design(Band& b) const {
    const double freq = std::min<double>(b.freq, sampleRate_ * 0.49);
    const double omega = 2.0 * M_PI * freq / sampleRate_;
    const bool pass = b.type == EqBandType::LowPass || b.type == EqBandType::HighPass;
    if (pass && b.slope24) {
      // 4th-order Butterworth: two sections with Q 0.5412 and 1.3066 (as BiquadFilter).
      b.sections = 2;
      b.k[0] = rbj(b.type, omega, 0.54119610, 0.0, 1.0);
      b.k[1] = rbj(b.type, omega, 1.30656296, 0.0, 1.0);
    } else {
      b.sections = 1;
      b.k[0] = rbj(b.type, omega, pass ? 0.70710678 : b.q, b.gainDb, b.shelfSlope);
    }
    b.dirty = false;
  }

  double sampleRate_;
  Band bands_[kEqBandCount];
};

}  // namespace webdsp
