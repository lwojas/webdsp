#pragma once
#include <algorithm>
#include <cmath>

// Anti-aliased tanh saturator for per-sample use inside an instrument (the bus Saturation node is block-based and
// not anti-aliased). First-order antiderivative anti-aliasing (ADAA): instead of sampling the nonlinearity, it
// outputs the average of f over the segment the input travelled this sample, (F(x1) - F(x0)) / (x1 - x0) with
// F = ln cosh. That removes most of the folded-back harmonics of a hard clip without oversampling, at the cost of
// a half-sample delay and a very slight high-frequency roll-off.
//
// Normalised so the control is musical at both ends: y = tanh(g x) / tanh(g). For small g this is the
// identity (y -> x), so a drive that is almost off is almost transparent; for large g a full-scale input stays at
// +-1 and anything below it is pushed toward it. g <= kOff is an exact bit-for-bit bypass.
// Odd-symmetric, so a zero-mean input gives a zero-mean output (no DC from the stage).
namespace webdsp {

class AdaaTanh {
 public:
  static constexpr float kOff = 1e-3f;

  void reset() { x1_ = 0.0; valid_ = false; }

  void setGain(float g) {
    g_ = std::isfinite(g) ? std::max(g, 0.0f) : 0.0f;
    norm_ = g_ > kOff ? 1.0 / (static_cast<double>(g_) * std::tanh(static_cast<double>(g_))) : 0.0;
    gn_ = g_ > kOff ? 1.0 / std::tanh(static_cast<double>(g_)) : 1.0;
  }
  bool active() const { return g_ > kOff; }

  float process(float x) {
    if (g_ <= kOff) {
      valid_ = false;
      return x;
    }
    const double xd = static_cast<double>(x);
    const double f1 = lnCosh(g_ * xd);
    double y;
    if (!valid_) {
      y = std::tanh(g_ * xd) * gn_;
    } else {
      const double dx = xd - x1_;
      if (std::fabs(dx) < 1e-7) {
        y = std::tanh(g_ * 0.5 * (xd + x1_)) * gn_;
      } else {
        y = (f1 - f0_) * norm_ / dx;
      }
    }
    x1_ = xd;
    f0_ = f1;
    valid_ = true;
    return static_cast<float>(y);
  }

 private:
  // Stable ln(cosh(z)) for any magnitude.
  static double lnCosh(double z) {
    const double a = std::fabs(z);
    return a + std::log1p(std::exp(-2.0 * a)) - 0.6931471805599453;
  }
  float g_ = 0.0f;
  double norm_ = 0.0, gn_ = 1.0;
  double x1_ = 0.0, f0_ = 0.0;
  bool valid_ = false;
};

}  // namespace webdsp
