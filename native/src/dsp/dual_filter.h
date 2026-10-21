#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "biquad_filter.h"
#include "percussion.h"

// Two filter stages with a routing switch, for one mono signal. Design and reasoning:
// docs/dual-filter-proposal.md (our own architecture; the Access Virus's configurable dual filter
// is inspiration only, nothing here claims to reproduce it).
//
//   single    x -> A -> out
//   series    x -> A -> B -> out
//   parallel  x -> A -+-> sum -> out
//             x -> B -+
//
// Each stage has a mode: 12 dB low-pass, 24 dB low-pass, 12 dB high-pass or band-pass. LP/HP are
// the existing BiquadFilter (RBJ, 12 or 24 dB Butterworth split); band-pass is PercBandpass (RBJ
// constant-peak-gain, then given a level make-up that grows with its Q so it sits near the low-pass
// in loudness on a saw-like input). Cutoffs are set per stage; one `resonance` (0..1) feeds both.
//
// Resonance handling (the reason this is not just "two filters in a row"):
//  - Series resonance multiplies (two peaks of Q at the same cutoff give Q*Q), so B follows A only
//    partway: Q_B = flat + kSeriesTie * (Q_A - flat). B colours, A resonates.
//  - Parallel resonance adds, so both stages use the full Q.
//  - gainComp() is the level correction for the resonant peaks of the LP/HP stages in use (the
//    band-pass has constant peak gain and needs none). The instrument multiplies it into its VCA.
//
// Block-based and in place; blocks of at most kMaxBlock frames (the instrument's control step).
// Real-time safe: fixed members only, nothing allocates after construction.
namespace webdsp {

class DualFilter {
 public:
  static constexpr int32_t kMaxBlock = 32;
  static constexpr int32_t kModeCount = 4;      // 0 LP12, 1 LP24, 2 HP12, 3 BP
  static constexpr int32_t kRoutingCount = 3;   // 0 single, 1 series, 2 parallel

  void configure(double sampleRate) {
    sampleRate_ = sampleRate;
    a_.configure(sampleRate);
    b_.configure(sampleRate);
    a_.setMode(modeA_);
    b_.setMode(modeB_);
    a_.cutoff(hzA_, true);
    b_.cutoff(hzB_, true);
    updateResonance();
  }

  void reset() {
    a_.reset();
    b_.reset();
  }

  void setRouting(int32_t routing) {
    const int32_t r = std::clamp(routing, 0, kRoutingCount - 1);
    if (r == routing_) return;
    routing_ = r;
    updateResonance();
  }
  void setModes(int32_t a, int32_t b) {
    a = std::clamp(a, 0, kModeCount - 1);
    b = std::clamp(b, 0, kModeCount - 1);
    if (a != modeA_) { modeA_ = a; a_.setMode(a); updateResonance(); }
    if (b != modeB_) { modeB_ = b; b_.setMode(b); updateResonance(); }
  }
  void setResonance(float r) {
    r = std::clamp(r, 0.0f, 1.0f);
    if (r == resonance_) return;
    resonance_ = r;
    updateResonance();
  }

  // Both cutoffs in Hz. Coefficients are only recomputed when a cutoff moves by > 0.2 %.
  void setCutoffs(float hzA, float hzB) {
    hzA_ = hzA;
    hzB_ = hzB;
    a_.cutoff(hzA, false);
    b_.cutoff(hzB, false);
  }

  // Level correction for the resonant peaks currently in effect (<= 1).
  float gainComp() const { return comp_; }
  int32_t routing() const { return routing_; }

  void process(float* buf, int32_t n) {
    n = std::min(n, kMaxBlock);
    switch (routing_) {
      case 0: a_.process(buf, n); break;
      case 1: a_.process(buf, n); b_.process(buf, n); break;
      default: {
        for (int32_t i = 0; i < n; i++) tmp_[i] = buf[i];
        a_.process(buf, n);
        b_.process(tmp_, n);
        for (int32_t i = 0; i < n; i++) buf[i] = (buf[i] + tmp_[i]) * kParallelScale;
        break;
      }
    }
  }

 private:
  static constexpr float kQFlat = 0.7071f;
  static constexpr float kQSpan = 13.0f;        // resonance 1 -> Q ~13.7 (as Bass / Percussion Synth)
  static constexpr float kQComp = 0.12f;
  static constexpr float kSeriesTie = 0.3f;     // share of A's resonance that reaches B in series
  static constexpr float kBpQMin = 0.6f;        // wide band-pass at resonance 0
  static constexpr float kBpQSpan = 12.0f;
  static constexpr float kBpMakeup = 1.5f;      // band-pass gain at its widest (see Stage::setQ)
  static constexpr float kParallelScale = 0.6f; // two paths add where they overlap
  static constexpr float kMaxStepOctaves = 0.5f;  // per control step (measured: 1.5 diverges at high Q)
  static constexpr float kMaxStep = 1.4142136f;   // 2^kMaxStepOctaves
  static constexpr float kMinStep = 0.7071068f;   // 2^-kMaxStepOctaves

  struct Stage {
    BiquadFilter bq{48000.0};
    PercBandpass bp;
    double sampleRate = 48000.0;
    int32_t mode = 0;
    float q = kQFlat;     // LP / HP quality
    float bpQ = kBpQMin;  // band-pass quality
    float bpGain = 1.0f;  // band-pass make-up (see setQ)
    float hz = 1000.0f, applied = -1.0f;

    void configure(double sr) {
      sampleRate = sr;
      bq = BiquadFilter(sr);
      bp.reset();
      applied = -1.0f;
    }
    void reset() {
      bq.reset();
      bp.reset();
      applied = -1.0f;  // the next cutoff jumps straight to its target (a fresh note has nothing to protect)
    }
    void setMode(int32_t m) {
      mode = m;
      bq.reset();
      bp.reset();
      bq.setParam(static_cast<int32_t>(VoiceParam::FilterMode), m == 2 ? 1.0f : 0.0f);
      bq.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), m == 1 ? 24.0f : 12.0f);
      applied = -1.0f;
      cutoff(hz, true);
      setQ(q, bpQ);
    }
    void setQ(float lpq, float bandQ) {
      q = lpq;
      bpQ = bandQ;
      // A band-pass passes a sliver of a broadband (saw-like) input and a narrower one passes less, so
      // its output would sit 10-25 dB under a low-pass at the same cutoff. Energy scales with the
      // bandwidth (1/Q), so the make-up grows as sqrt(Q): about +3.5 dB wide, +16 dB at full resonance.
      bpGain = kBpMakeup * std::sqrt(bandQ / kBpQMin);
      bq.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), q);
      if (mode == 3 && applied > 0.0f) bp.set(applied, bpQ, sampleRate);
    }
    double clampHz(double f) const { return std::clamp(f, 30.0, std::min(18000.0, sampleRate * 0.45)); }
    // Moves toward `f`, at most kMaxStepOctaves (0.5) per call: a biquad whose coefficients jump across
    // many octaves between blocks at high Q can diverge (measured; see filter_modulation.h). The
    // instrument's envelopes are smooth, so this only acts on abrupt moves (a retrigger, a live
    // knob). `force` jumps straight there (fresh filter, nothing to protect).
    void cutoff(float f, bool force) {
      hz = static_cast<float>(clampHz(f));
      float next = hz;
      if (!force && applied > 0.0f) {
        next = std::clamp(hz, applied * kMinStep, applied * kMaxStep);
      }
      if (!force && std::fabs(next - applied) <= 0.002f * next) return;
      applied = next;
      if (mode == 3) bp.set(next, bpQ, sampleRate);
      else bq.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), next);
    }
    void process(float* buf, int32_t n) {
      if (mode == 3) {
        for (int32_t i = 0; i < n; i++) buf[i] = bp.process(buf[i]) * bpGain;
      } else {
        float* ch[1] = {buf};
        bq.process(ch, 1, n);
      }
    }
  };

  void updateResonance() {
    const float qA = kQFlat + resonance_ * resonance_ * kQSpan;
    const float bandA = kBpQMin + resonance_ * resonance_ * kBpQSpan;
    const bool series = routing_ == 1;
    const float qB = series ? kQFlat + kSeriesTie * (qA - kQFlat) : qA;
    const float bandB = series ? kBpQMin + kSeriesTie * (bandA - kBpQMin) : bandA;
    a_.setQ(qA, bandA);
    b_.setQ(qB, bandB);
    // Peak-gain compensation. A resonant LP/HP stage peaks at about its Q at the cutoff; a band-pass
    // has constant peak gain and contributes nothing. Series peaks multiply, so the effective Q is
    // the product (as a single-stage Q: qA * qB / flat); parallel peaks add, so the larger one rules.
    const float peakA = modeA_ == 3 ? kQFlat : qA;
    const float peakB = routing_ == 0 || modeB_ == 3 ? kQFlat : qB;
    float qEff = peakA;
    if (routing_ == 1) qEff = peakA * peakB / kQFlat;
    else if (routing_ == 2) qEff = std::max(peakA, peakB);
    float comp = 1.0f / (1.0f + kQComp * (qEff - kQFlat));
    comp_ = comp;
  }

  double sampleRate_ = 48000.0;
  int32_t routing_ = 1;
  int32_t modeA_ = 0, modeB_ = 2;
  float resonance_ = 0.0f;
  float hzA_ = 2500.0f, hzB_ = 400.0f;
  float comp_ = 1.0f;
  float tmp_[kMaxBlock] = {};
  Stage a_, b_;
};

}  // namespace webdsp
