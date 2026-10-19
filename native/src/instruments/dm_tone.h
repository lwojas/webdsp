#pragma once
#include "digital_machine.h"

// Digital Machine "Tone": four-operator FM for plucks, bass, keys, bells, leads and chord stabs.
// Docs: docs/digital-machines.md.
//
// Four operators, evaluated 3 -> 0, each phase-modulated by the operators above it through a small
// constant weight table per `structure` (op i is modulated by op j only for j > i, so there is no
// delay in the graph). Carriers are summed with fixed weights.
//
//   0 Stack   3 -> 2 -> 1 -> 0                    bass, brass, glassy leads
//   1 Twin    (1 -> 0) + (3 -> 2)                 electric-piano keys (a slow body and a fast "tine")
//   2 Fan     1, 2, 3 -> 0 at bar-like ratios     bells, clangs
//   3-5       3 -> 0, 1, 2  (carriers at a chord) major / minor / sus stabs; one shared modulator
//
// Each modulator has its own envelope: decay = Decay x the structure's per-operator time scale x
// (0.08 .. 1 by `damp`), so a pluck is bright for a moment and settles while the carriers ring (the
// DX trick). `hold` raises the carrier sustain (and the modulators' floor), so the same patch can be
// an organ instead of a pluck; a note-off then releases. There is deliberately no filter in the voice:
// the spectrum is FM plus Drive. The top modulator has self-feedback proportional to the index, and
// op 0 carries a clean-sine share at low pitch (as Fm's "bass weight") so the fundamental survives
// the Bessel nulls. Each link's index is limited from its modulator's frequency; operators above
// ~0.4 fs fade out.
namespace webdsp {

inline constexpr InstrumentParamInfo kDmToneParams[] = {
    {static_cast<int32_t>(DmToneParam::Tune), "tune", 27.5f, 880.0f, 110.0f, "Hz"},
    {static_cast<int32_t>(DmToneParam::Decay), "decay", 0.03f, 3.0f, 0.5f, "s"},
    {static_cast<int32_t>(DmToneParam::Fm), "fm", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(DmToneParam::Structure), "structure", 0.0f, 5.0f, 0.0f, "step"},
    {static_cast<int32_t>(DmToneParam::Ratio), "ratio", 0.0f, 1.0f, 0.111f, ""},
    {static_cast<int32_t>(DmToneParam::Damp), "damp", 0.0f, 1.0f, 0.4f, ""},
    {static_cast<int32_t>(DmToneParam::Attack), "attack", 0.001f, 0.25f, 0.002f, "s"},
    {static_cast<int32_t>(DmToneParam::Hold), "hold", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(DmToneParam::Drive), "drive", 0.0f, 1.0f, 0.1f, ""},
};
inline constexpr int32_t kDmToneParamCount =
    static_cast<int32_t>(sizeof(kDmToneParams) / sizeof(kDmToneParams[0]));

class DmTone {
 public:
  static constexpr InstrumentKind kKind = InstrumentKind::DmTone;
  static constexpr const InstrumentParamInfo* kParams = kDmToneParams;
  static constexpr int32_t kParamCount = kDmToneParamCount;

  void configure(double sampleRate, const float* table) {
    sr_ = sampleRate;
    table_ = table;
    amp_.configure(sampleRate);
    for (auto& e : idxEnv_) e.configure(sampleRate);
    reset();
  }

  void noteOn(const float* v, float velocity, double pitchRatio, bool fresh) {
    vel_ = velocity;
    pitchRatio_ = pitchRatio;
    if (fresh) clearState();
    const Structure& st = structure(v);
    const float decay = v[idx(DmToneParam::Decay)];
    const float attack = v[idx(DmToneParam::Attack)];
    const float hold = v[idx(DmToneParam::Hold)];
    const float damp = v[idx(DmToneParam::Damp)];
    amp_.trigger(attack, percDecayCoef(decay * kDecayToTau, sr_), hold);
    const float floorLevel = hold * (0.15f + 0.85f * damp);
    const float time = 0.08f + 0.92f * damp;
    for (int32_t j = 1; j < 4; j++) {
      idxEnv_[j].trigger(std::max(attack, 0.0005f),
                         percDecayCoef(decay * st.timeScale[j] * time * kDecayToTau, sr_), floorLevel);
    }
    releaseCoef_ = percDecayCoef((0.012f + 0.25f * hold) * kDecayToTau, sr_);
  }

  void noteOff(const float*) {
    amp_.release(releaseCoef_);
    for (int32_t j = 1; j < 4; j++) idxEnv_[j].release(releaseCoef_);
  }

  void reset() {
    amp_.reset();
    for (auto& e : idxEnv_) e.reset();
    clearState();
  }

  bool render(const float* v, float* out, int32_t n) {
    const Structure& st = structure(v);
    const float tune = v[idx(DmToneParam::Tune)];
    const float fm = v[idx(DmToneParam::Fm)];
    const float ratio = fmk::magneticRatio(kRatios, v[idx(DmToneParam::Ratio)]);
    const float vf = fmk::velocityBrightness(vel_);
    drive_.set(v[idx(DmToneParam::Drive)]);

    // --- control rate: operator frequencies, per-link index limits, operator gates, bass weight.
    const double f0 = std::clamp(static_cast<double>(tune) * pitchRatio_, 20.0, std::min(4000.0, sr_ * 0.2));
    double inc[4], hzOp[4];
    float gate[4];
    for (int32_t i = 0; i < 4; i++) {
      hzOp[i] = f0 * st.mul[i] * (st.useRatio[i] ? static_cast<double>(ratio) : 1.0);
      inc[i] = hzOp[i] / sr_;
      gate[i] = fmk::nyquistGain(hzOp[i], sr_);
    }
    // Index limits, top-down. A modulator's own bandwidth is its frequency x (1 + its feedback +
    // the indexes of the operators modulating it), so each link is limited from that, using the
    // largest index each upper link can actually reach.
    const int32_t fb = st.feedbackOp;
    const float beta = std::min(kFeedbackMax * fm, fmk::maxIndex(hzOp[fb], sr_));
    float cap[4] = {0, 0, 0, 0}, reach[4] = {0, 0, 0, 0};
    for (int32_t i = 3; i >= 1; i--) {
      double spread = 1.0 + (i == fb ? static_cast<double>(beta) : 0.0);
      for (int32_t k = i + 1; k < 4; k++) spread += static_cast<double>(st.weight[i][k]) * reach[k];
      cap[i] = fmk::maxIndex(hzOp[i] * spread, sr_);
      reach[i] = std::min(cap[i], kIndexScale * fm * st.index[i]);
    }
    const float weight = kBassWeight * static_cast<float>(std::clamp(1.0 - std::log2(f0 / 80.0) * 0.5, 0.0, 1.0));

    for (int32_t k = 0; k < n; k++) {
      const float e = amp_.next();
      float link[4] = {0, 0, 0, 0};  // index x envelope of each modulator's outgoing link
      for (int32_t j = 1; j < 4; j++) {
        const float ie = idxEnv_[j].next();
        link[j] = std::min(vf * fm * kIndexScale * st.index[j] * ie, cap[j]);
      }

      float x[4] = {0, 0, 0, 0};
      for (int32_t i = 3; i >= 0; i--) {
        op_[i].advance(inc[i]);
        float pm = 0.0f;
        for (int32_t j = i + 1; j < 4; j++) pm += st.weight[i][j] * link[j] * x[j];
        if (i == st.feedbackOp) pm += beta * 0.5f * (fb1_ + fb2_);
        x[i] = op_[i].at(table_, pm) * gate[i];
        if (i == st.feedbackOp) { fb2_ = fb1_; fb1_ = x[i]; }
      }
      if (weight > 0.0f) {
        x[0] = (x[0] + weight * op_[0].at(table_, 0.0f) * gate[0]) * (1.0f / (1.0f + weight));
      }
      const float y = x[0] * st.out[0] + x[1] * st.out[1] + x[2] * st.out[2] + x[3] * st.out[3];
      out[k] = drive_.process(y * e) * vel_ * kLevel;
    }
    return !amp_.idle();
  }

 private:
  static constexpr float kLevel = 0.5f;
  static constexpr float kIndexScale = 7.0f;   // radians at fm 1, before a structure's per-link scale
  static constexpr float kFeedbackMax = 1.0f;  // top-modulator self-feedback at fm 1
  static constexpr float kBassWeight = 0.8f;   // clean-carrier share at/below 80 Hz
  // Modulator ratios: sub-octave, 1, sqrt 2, 2, bar partial, 3, 3.5, 4, 5, 7.
  static constexpr float kRatios[] = {0.5f, 1.0f, 1.41421356f, 2.0f, 2.76f, 3.0f, 3.5f, 4.0f, 5.0f, 7.0f};

  struct Structure {
    float mul[4];        // operator frequency = f0 x mul (x ratio where useRatio)
    bool useRatio[4];
    float weight[4][4];  // weight[i][j]: how much op j (j > i) modulates op i
    float index[4];      // outgoing-link index scale of modulator j
    float timeScale[4];  // modulator j's decay relative to Decay
    float out[4];        // carrier mix
    int32_t feedbackOp;  // top modulator that feeds itself back
  };
  static constexpr Structure kStructures[6] = {
      // Stack: 3 -> 2 -> 1 -> 0
      {{1, 1, 1, 2}, {false, true, false, true},
       {{0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}, {0, 0, 0, 0}},
       {0, 1.0f, 0.7f, 0.5f}, {1, 0.7f, 0.5f, 0.35f}, {1, 0, 0, 0}, 3},
      // Twin: (1 -> 0) + (3 -> 2); a fast high "tine"
      {{1, 1, 1.003f, 7}, {false, true, false, false},
       {{0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 0}},
       {0, 1.0f, 0, 0.6f}, {1, 0.8f, 1, 0.12f}, {0.6f, 0, 0.45f, 0}, 1},
      // Fan: 1, 2, 3 -> 0 at bar-like ratios
      {{1, 1, 2.76f, 5.4f}, {false, true, true, true},
       {{0, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {0, 0.8f, 0.55f, 0.4f}, {1, 0.8f, 0.4f, 0.2f}, {1, 0, 0, 0}, 1},
      // Major / minor / sus: modulator 3 -> carriers 0, 1, 2 at the chord's intervals
      {{1, 1.25992105f, 1.49830708f, 1}, {false, false, false, true},
       {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
       {0, 0, 0, 1.0f}, {1, 1, 1, 0.6f}, {0.42f, 0.36f, 0.32f, 0}, 3},
      {{1, 1.18920712f, 1.49830708f, 1}, {false, false, false, true},
       {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
       {0, 0, 0, 1.0f}, {1, 1, 1, 0.6f}, {0.42f, 0.36f, 0.32f, 0}, 3},
      {{1, 1.33483985f, 1.49830708f, 1}, {false, false, false, true},
       {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
       {0, 0, 0, 1.0f}, {1, 1, 1, 0.6f}, {0.42f, 0.36f, 0.32f, 0}, 3},
  };

  static int32_t idx(DmToneParam p) { return static_cast<int32_t>(p); }
  static const Structure& structure(const float* v) {
    const int32_t s = std::clamp(static_cast<int32_t>(std::lround(v[idx(DmToneParam::Structure)])), 0, 5);
    return kStructures[s];
  }

  void clearState() {
    for (int32_t i = 0; i < 4; i++) op_[i].phase = 0.0;
    op_[0].phase = 0.25;  // carrier peak: punch from the first cycle (attack >= 1 ms keeps it click-free)
    fb1_ = fb2_ = 0.0f;
  }

  double sr_ = 48000.0;
  const float* table_ = nullptr;
  float vel_ = 1.0f;
  double pitchRatio_ = 1.0;
  fmk::ExpEnv amp_, idxEnv_[4];
  fmk::FmOp op_[4];
  float fb1_ = 0.0f, fb2_ = 0.0f;
  float releaseCoef_ = 0.99f;
  fmk::Drive drive_;
};

using DmToneInstrument = DigitalMachine<DmTone>;

}  // namespace webdsp
