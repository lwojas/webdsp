#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Dattorro plate reverb — Jon Dattorro, "Effect Design, Part 1: Reverberator and Other
// Filters" (JAES 45(9), 1997), Fig. 1 / Table 1 / Table 2. Re-derived directly from the
// paper (not ported from any third-party implementation), per
// docs/effects-algorithm-survey.md's recommendation and this project's "own a small
// algorithm" bias (see ARCHITECTURE.md, "DSP library investigation"). All delay lengths,
// coefficients, and the stereo output tap structure are the paper's own published values,
// linearly scaled from its reference sample rate (29761 Hz) to this engine's actual rate —
// the standard way this specific algorithm is ported to 44.1/48 kHz.
//
// One deliberate simplification vs. a literal reading of Fig. 1: the paper's block diagram
// shows the "decay" gain applied at two points per tank loop (an artifact of the fixed-point
// hardware the paper targets, where gain staging for headroom matters); this implementation
// applies it once, at the point where each tank's recirculating output is summed into the
// *other* tank's input (see `advanceTanks` below) — the single coefficient that actually
// determines the per-loop feedback gain (and therefore tail length), functionally equivalent
// for a float32 implementation with no fixed-point headroom concern.
namespace webdsp {

// Delay-line lengths and output-tap offsets in Table 1/Table 2 are specified at this rate.
constexpr double kReverbReferenceSampleRate = 29761.0;
// Table 1: "rate of update" for the tank's delay-modulation LFO is "on the order of 1 Hz".
constexpr double kReverbModRateHz = 1.0;

// A fixed-length circular buffer implementing an exact N-sample delay: reading and writing
// happen at the same index each sample (the standard trick — buf[w] holds, before being
// overwritten, exactly the value written N samples ago). Also supports `tap()`, an
// additional read at a fixed offset behind the write head, into the same physical memory —
// this is how Dattorro's Table 2 derives multiple “taps” from a handful of delay lines
// rather than adding dedicated buffers per output tap.
class ReverbDelayLine {
 public:
  void resize(int32_t length) {
    buf_.assign(static_cast<size_t>(std::max(1, length)), 0.0f);
    w_ = 0;
  }
  void clear() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    w_ = 0;
  }
  float peek() const { return buf_[w_]; }
  void push(float x) {
    buf_[w_] = x;
    w_ = (w_ + 1) % static_cast<int32_t>(buf_.size());
  }
  float advance(float x) {
    const float out = peek();
    push(x);
    return out;
  }
  // Value written `offset` samples ago (0 == the most recent write).
  float tap(int32_t offset) const {
    const int32_t n = static_cast<int32_t>(buf_.size());
    int32_t idx = (w_ - 1 - offset) % n;
    if (idx < 0) idx += n;
    return buf_[idx];
  }

 private:
  std::vector<float> buf_;
  int32_t w_ = 0;
};

// Two-multiplier lattice all-pass filter (Schroeder form), the topology every diffuser in
// Fig. 1 uses: H(z) = (-g + z^-N) / (1 - g*z^-N). Also exposes `tap()` for the same reason
// as ReverbDelayLine — two of Table 2's named segments (node31_33 / node55_59) are taps
// into a decay-diffuser's own internal delay memory.
class ReverbAllpass {
 public:
  void resize(int32_t length) {
    buf_.assign(static_cast<size_t>(std::max(1, length)), 0.0f);
    w_ = 0;
  }
  void clear() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    w_ = 0;
  }
  float process(float x, float coeff) {
    const int32_t n = static_cast<int32_t>(buf_.size());
    const float bufOut = buf_[w_];
    const float y = bufOut - coeff * x;
    buf_[w_] = x + coeff * y;
    w_ = (w_ + 1) % n;
    return y;
  }
  float tap(int32_t offset) const {
    const int32_t n = static_cast<int32_t>(buf_.size());
    int32_t idx = (w_ - 1 - offset) % n;
    if (idx < 0) idx += n;
    return buf_[idx];
  }

 private:
  std::vector<float> buf_;
  int32_t w_ = 0;
};

// The modulated all-pass at the head of each tank (Fig. 1's "672 + EXCURSION" /
// "908 + EXCURSION" boxes). Same lattice math as ReverbAllpass, but the read tap is a
// slowly LFO-modulated *fractional* position instead of a fixed integer one — Section 1.3.7
// of the paper: "linear interpolation ... efficiently employed to modulate slowly the
// nominal tap point". `resampler.h`'s `linear()` isn't reused verbatim here because it
// clamps at buffer edges (correct for a single non-wrapping sample buffer) rather than
// wrapping (required for this circular delay); the interpolation itself — a plain two-point
// linear blend — is the same technique.
class ReverbModulatedAllpass {
 public:
  void configure(int32_t baseDelay, int32_t excursion) {
    baseDelay_ = static_cast<float>(baseDelay);
    excursion_ = static_cast<float>(excursion);
    buf_.assign(static_cast<size_t>(baseDelay + excursion + 4), 0.0f);
    w_ = 0;
  }
  void clear() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    w_ = 0;
  }
  // `lfo` is expected in [-1, 1].
  float process(float x, float coeff, float lfo) {
    const int32_t n = static_cast<int32_t>(buf_.size());
    const double readPos = static_cast<double>(w_) - (baseDelay_ + excursion_ * lfo);
    const float bufOut = readWrapped(readPos);
    const float y = bufOut - coeff * x;
    buf_[w_] = x + coeff * y;
    w_ = (w_ + 1) % n;
    return y;
  }

 private:
  float readWrapped(double pos) const {
    const int32_t n = static_cast<int32_t>(buf_.size());
    int32_t i0 = static_cast<int32_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(i0);
    int32_t idx0 = i0 % n;
    if (idx0 < 0) idx0 += n;
    const int32_t idx1 = (idx0 + 1) % n;
    return static_cast<float>(buf_[static_cast<size_t>(idx0)] * (1.0 - frac) +
                               buf_[static_cast<size_t>(idx1)] * frac);
  }

  std::vector<float> buf_;
  int32_t w_ = 0;
  float baseDelay_ = 0.0f;
  float excursion_ = 0.0f;
};

class Reverb final : public DSPNode {
 public:
  explicit Reverb(double sampleRate) {
    const double scale = sampleRate / kReverbReferenceSampleRate;
    auto scaled = [scale](int32_t n) {
      return std::max(1, static_cast<int32_t>(std::lround(n * scale)));
    };

    inputDiffuse_[0].resize(scaled(142));
    inputDiffuse_[1].resize(scaled(107));
    inputDiffuse_[2].resize(scaled(379));
    inputDiffuse_[3].resize(scaled(277));

    const int32_t excursion = scaled(16);
    tankA_.decayDiffuse1.configure(scaled(672), excursion);
    tankA_.dampDelay.resize(scaled(4453));
    tankA_.decayDiffuse2.resize(scaled(1800));
    tankA_.longDelay.resize(scaled(3720));

    tankB_.decayDiffuse1.configure(scaled(908), excursion);
    tankB_.dampDelay.resize(scaled(4217));
    tankB_.decayDiffuse2.resize(scaled(2656));
    tankB_.longDelay.resize(scaled(3163));

    // Table 2's output-tap offsets, scaled the same way as the delay lengths they read
    // into (ReverbDelayLine::tap / ReverbAllpass::tap wrap safely regardless, so exact
    // rounding here isn't precision-critical).
    tapADamp_[0] = scaled(1990);
    tapADamp_[1] = scaled(353);
    tapADamp_[2] = scaled(3627);
    tapADiffuse2_[0] = scaled(187);
    tapADiffuse2_[1] = scaled(1228);
    tapALong_[0] = scaled(1066);
    tapALong_[1] = scaled(2673);

    tapBDamp_[0] = scaled(266);
    tapBDamp_[1] = scaled(2974);
    tapBDamp_[2] = scaled(2111);
    tapBDiffuse2_[0] = scaled(1913);
    tapBDiffuse2_[1] = scaled(335);
    tapBLong_[0] = scaled(1996);
    tapBLong_[1] = scaled(121);

    lfoIncrement_ = 2.0 * M_PI * kReverbModRateHz / sampleRate;
    recomputeDecayDiffusion2();
    reset();
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (mix_ <= 0.0f) return;
    float* left = channels[0];
    float* right = numChannels > 1 ? channels[1] : channels[0];

    for (int32_t i = 0; i < numFrames; i++) {
      const float dryL = left[i];
      const float dryR = numChannels > 1 ? right[i] : dryL;
      float wetL, wetR;
      processSample(dryL, dryR, wetL, wetR);
      left[i] = dryL * (1.0f - mix_) + wetL * mix_;
      if (numChannels > 1) right[i] = dryR * (1.0f - mix_) + wetR * mix_;
    }
  }

  void setParam(int32_t param, float value) override {
    if (param == static_cast<int32_t>(NodeParam::ReverbDecay)) {
      decay_ = std::clamp(value, 0.0f, 0.9999f);
      recomputeDecayDiffusion2();
    } else if (param == static_cast<int32_t>(NodeParam::ReverbDamping)) {
      damping_ = std::clamp(value, 0.0f, 0.9999f);
    } else if (param == static_cast<int32_t>(NodeParam::ReverbMix)) {
      mix_ = std::clamp(value, 0.0f, 1.0f);
    }
  }

  void reset() override {
    bandwidthState_ = 0.0f;
    lfoPhase_ = 0.0;
    for (auto& d : inputDiffuse_) d.clear();
    tankA_.clear();
    tankB_.clear();
  }

 private:
  struct Tank {
    ReverbModulatedAllpass decayDiffuse1;
    ReverbDelayLine dampDelay;
    float dampState = 0.0f;
    ReverbAllpass decayDiffuse2;
    ReverbDelayLine longDelay;

    void clear() {
      decayDiffuse1.clear();
      dampDelay.clear();
      dampState = 0.0f;
      decayDiffuse2.clear();
      longDelay.clear();
    }

    // Runs this tank forward by one sample given its already-summed input (diffused signal
    // + decay * other tank's recirculating output, see processSample) and this tank's LFO
    // value; the fresh recirculating output is left in longDelay for next sample's read.
    void advance(float x, float lfo, float diffuse1Coeff, float dampingCoeff,
                 float decayDiffuse2Coeff) {
      const float afterDiffuse1 = decayDiffuse1.process(x, diffuse1Coeff, lfo);
      const float delayed = dampDelay.advance(afterDiffuse1);
      dampState = (1.0f - dampingCoeff) * delayed + dampingCoeff * dampState;
      const float afterDiffuse2 = decayDiffuse2.process(dampState, decayDiffuse2Coeff);
      longDelay.push(afterDiffuse2);
    }
  };

  void recomputeDecayDiffusion2() { decayDiffuse2Coeff_ = std::clamp(decay_ + 0.15f, 0.25f, 0.50f); }

  void processSample(float xL, float xR, float& outL, float& outR) {
    const float mono = (xL + xR) * 0.5f;
    bandwidthState_ = mono * kBandwidth + bandwidthState_ * (1.0f - kBandwidth);

    float diffused = bandwidthState_;
    diffused = inputDiffuse_[0].process(diffused, kInputDiffusion1);
    diffused = inputDiffuse_[1].process(diffused, kInputDiffusion1);
    diffused = inputDiffuse_[2].process(diffused, kInputDiffusion2);
    diffused = inputDiffuse_[3].process(diffused, kInputDiffusion2);

    // Both tanks read each other's *pre-this-sample* recirculating output (i.e. what's
    // already sitting in each longDelay before either tank advances) — order-independent,
    // avoids a same-sample feedback cycle between the two tanks.
    const float feedbackFromA = tankA_.longDelay.peek();
    const float feedbackFromB = tankB_.longDelay.peek();

    const float lfoA = static_cast<float>(std::sin(lfoPhase_));
    const float lfoB = static_cast<float>(std::cos(lfoPhase_));  // quadrature, per §1.3.7
    lfoPhase_ += lfoIncrement_;
    if (lfoPhase_ > 2.0 * M_PI) lfoPhase_ -= 2.0 * M_PI;

    tankA_.advance(diffused + decay_ * feedbackFromB, lfoA, kDecayDiffusion1, damping_,
                    decayDiffuse2Coeff_);
    tankB_.advance(diffused + decay_ * feedbackFromA, lfoB, kDecayDiffusion1, damping_,
                    decayDiffuse2Coeff_);

    // Table 2's stereo output tap structure — an all-wet (y_L, y_R) synthesized purely from
    // fixed reads into the tank's own delay memory, no extra filtering.
    float accL = 0.6f * tankB_.dampDelay.tap(tapBDamp_[0]);
    accL += 0.6f * tankB_.dampDelay.tap(tapBDamp_[1]);
    accL -= 0.6f * tankB_.decayDiffuse2.tap(tapBDiffuse2_[0]);
    accL += 0.6f * tankB_.longDelay.tap(tapBLong_[0]);
    accL -= 0.6f * tankA_.dampDelay.tap(tapADamp_[0]);
    accL -= 0.6f * tankA_.decayDiffuse2.tap(tapADiffuse2_[0]);
    outL = accL - 0.6f * tankA_.longDelay.tap(tapALong_[0]);

    float accR = 0.6f * tankA_.dampDelay.tap(tapADamp_[1]);
    accR += 0.6f * tankA_.dampDelay.tap(tapADamp_[2]);
    accR -= 0.6f * tankA_.decayDiffuse2.tap(tapADiffuse2_[1]);
    accR += 0.6f * tankA_.longDelay.tap(tapALong_[1]);
    accR -= 0.6f * tankB_.dampDelay.tap(tapBDamp_[2]);
    accR -= 0.6f * tankB_.decayDiffuse2.tap(tapBDiffuse2_[1]);
    outR = accR - 0.6f * tankB_.longDelay.tap(tapBLong_[1]);
  }

  static constexpr float kBandwidth = 0.9995f;
  static constexpr float kInputDiffusion1 = 0.750f;
  static constexpr float kInputDiffusion2 = 0.625f;
  static constexpr float kDecayDiffusion1 = 0.700f;

  float decay_ = 0.5f;       // NodeParam::ReverbDecay — tank per-loop feedback gain (tail length)
  float damping_ = 0.0005f;  // NodeParam::ReverbDamping — tank high-frequency damping
  float mix_ = 0.0f;         // NodeParam::ReverbMix — 0 => bypass, matches Filter/Delay's convention
  float decayDiffuse2Coeff_ = 0.5f;

  float bandwidthState_ = 0.0f;
  double lfoPhase_ = 0.0;
  double lfoIncrement_ = 0.0;

  ReverbAllpass inputDiffuse_[4];
  Tank tankA_;
  Tank tankB_;

  int32_t tapADamp_[3] = {};
  int32_t tapADiffuse2_[2] = {};
  int32_t tapALong_[2] = {};
  int32_t tapBDamp_[3] = {};
  int32_t tapBDiffuse2_[2] = {};
  int32_t tapBLong_[2] = {};
};

}  // namespace webdsp
