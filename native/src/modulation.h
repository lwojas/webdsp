#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "dsp/lfo.h"
#include "instrument.h"
#include "instrument_registry.h"

// Parameter modulation: the layer between LFO sources (dsp/lfo.h) and the parameters a module
// exposes. The same bank serves synths (InstrumentModulation, below) and FX instances (fx_chain.h),
// so a stack of LFOs behaves identically wherever it is attached. Design and rationale:
// docs/lfo-modulation.md and docs/modulation.md.
//
//   synth parameter (BASE, set by the app)  --+
//                                             +--> effective = map(domain, base, sum of LFOs)
//   LFO x up to kMaxLfos (value x amount)   --+            |
//                                                          v
//                                            Instrument::setParam(effective)   (control rate)
//
// - The LFO is parameter-agnostic. The synth owns the parameter and declares its ModDomain; this
//   layer applies normalized modulation to the base in that domain and clamps to the range.
// - The base value is stored here, separate from what the instrument is told. While a parameter is
//   targeted, app writes update the base only; the next control tick pushes base + modulation.
//   When the last LFO leaves a parameter, the base is restored to the instrument.
// - Nothing here (or in the LFO) is known to Bass, Pulse or any other synth. A synth takes part
//   only by marking params with a ModDomain; it implements no modulation code.
// - Fixed capacity, no allocation, no graph: each LFO has exactly one target; several LFOs may
//   share a target (their contributions add before mapping).
namespace webdsp {

inline constexpr int32_t kMaxLfos = 4;
inline constexpr int32_t kMaxModParams = 32;       // per instrument kind (asserted in tests)
inline constexpr int32_t kModChunkFrames = 32;     // control rate while modulating (~1.5 kHz)
inline constexpr float kModSmoothSeconds = 0.0015f;  // one-pole on the LFO output (square/saw edges)
inline constexpr float kModMaxOctavesPerChunk = 1.0f;  // slew limit on Log params (filter safety)

// Pure mapping: `mod` is the summed, amount-scaled LFO signal (nominally -1..+1).
inline float modulatedValue(ModDomain d, float base, float mod, float lo, float hi) {
  if (!std::isfinite(mod) || d == ModDomain::None) return std::clamp(base, lo, hi);
  float v;
  if (d == ModDomain::Log && lo > 0.0f) {
    v = base * std::exp2(mod * 0.5f * std::log2(hi / lo));
  } else {
    v = base + mod * 0.5f * (hi - lo);
  }
  return std::isfinite(v) ? std::clamp(v, lo, hi) : std::clamp(base, lo, hi);
}

// One modulatable-or-not parameter of whatever the bank is attached to (a synth control, an FX
// NodeParam). `dom` is the resolved domain (never Auto).
struct ModParamSpec {
  int32_t id;
  float min;
  float max;
  float def;
  ModDomain dom;
};

// Sink contract used by the templates below: `set(paramId, value)` pushes a value to the owner.
class ParamModulation {
 public:
  void configure(const ModParamSpec* specs, int32_t count, double sampleRate) {
    sampleRate_ = sampleRate;
    n_ = std::min(count, kMaxModParams);
    for (int32_t i = 0; i < n_; i++) {
      id_[i] = specs[i].id;
      lo_[i] = specs[i].min;
      hi_[i] = specs[i].max;
      dom_[i] = specs[i].dom;
      base_[i] = specs[i].def;
      last_[i] = specs[i].def;
    }
    for (auto& s : slots_) s = Slot{};
    mask_ = 0;
    alpha_ = 1.0f - std::exp(-static_cast<float>(kModChunkFrames) /
                             static_cast<float>(sampleRate * kModSmoothSeconds));
  }

  bool active() const { return mask_ != 0; }

  // The app wrote a parameter. Records the (range-clamped) base. Returns true if the value should
  // go straight to the instrument, false if an LFO currently owns the live value of this param.
  bool setBase(int32_t paramId, float value) {
    const int32_t i = indexOf(paramId);
    if (i < 0) return true;
    if (std::isfinite(value)) base_[i] = std::clamp(value, lo_[i], hi_[i]);
    return (mask_ & (1u << i)) == 0;
  }

  float base(int32_t paramId) const {
    const int32_t i = indexOf(paramId);
    return i < 0 ? 0.0f : base_[i];
  }
  // What the instrument was last told: base + modulation if targeted, else the base.
  float effective(int32_t paramId) const {
    const int32_t i = indexOf(paramId);
    if (i < 0) return 0.0f;
    return (mask_ & (1u << i)) ? last_[i] : base_[i];
  }
  const LfoConfig& config(int32_t slot) const { return slots_[slot].cfg; }
  // Smoothed, amount-scaled signal of an LFO as of the last tick.
  float lastSignal(int32_t slot) const { return slots_[slot].smooth * slots_[slot].cfg.amount; }

  // Configures (or disables) LFO `slot`. Returns false, leaving the slot disabled, if the slot is
  // out of range or an enabled config targets a parameter that does not exist or is not
  // modulatable. Phase is preserved across edits of an enabled LFO (rate, wave, amount, target...);
  // it restarts only when the LFO goes from disabled to enabled.
  template <class Set>
  bool setLfo(int32_t slot, LfoConfig cfg, Set&& set, double frame) {
    if (slot < 0 || slot >= kMaxLfos) return false;
    Slot& s = slots_[slot];
    if (s.cfg.enabled && !s.cfg.sync) s.lfo.catchUp(frame, sampleRate_, s.cfg.rateHz);  // old rate
    const uint32_t oldMask = mask_;

    cfg.waveform = static_cast<LfoWaveform>(lfoWaveformIndex(static_cast<int32_t>(cfg.waveform)));
    cfg.division = lfoDivisionIndex(cfg.division);
    cfg.rateHz = std::isfinite(cfg.rateHz) ? std::clamp(cfg.rateHz, kLfoMinRateHz, kLfoMaxRateHz)
                                           : kLfoDefaultRateHz;
    cfg.amount = std::isfinite(cfg.amount) ? std::clamp(cfg.amount, -1.0f, 1.0f) : 0.0f;
    cfg.phase = std::isfinite(cfg.phase) ? cfg.phase - std::floor(cfg.phase) : 0.0f;
    bool ok = true;
    if (cfg.enabled) {
      const int32_t ti = indexOf(cfg.target);
      if (ti < 0 || dom_[ti] == ModDomain::None) {
        cfg.enabled = false;
        ok = false;
      }
    }
    if (cfg.enabled && !s.cfg.enabled) {
      s.lfo.reset(frame);
      s.smoothValid = false;
    }
    s.cfg = cfg;
    rebuildMask();

    // Params that just lost their last LFO get their base back; newly targeted ones start their
    // slew from the base.
    for (int32_t i = 0; i < n_; i++) {
      const uint32_t bit = 1u << i;
      if ((oldMask & bit) && !(mask_ & bit)) set(id_[i], base_[i]);
      if (!(oldMask & bit) && (mask_ & bit)) last_[i] = base_[i];
    }
    return ok;
  }

  // Brings the modulated params to their exact value at `frame`. The engine calls this before a
  // note starts on an idle instrument, because an idle instrument gets no control ticks and
  // per-note params (a drum's decay, read at note-on) would otherwise use a stale value.
  template <class Set>
  void prepare(Set&& set, double frame, double anchorFrame, double framesPerBeat) {
    update(set, frame, 1, anchorFrame, framesPerBeat, true);
  }

  // A note starts at `frame`: LFOs that asked for it restart their free-running cycle.
  void noteOn(double frame) {
    for (auto& s : slots_) {
      if (s.cfg.enabled && s.cfg.retrigger && !s.cfg.sync) s.lfo.reset(frame);
    }
  }

  // One control tick for the `n` frames starting at absolute `frame`. Cheap when nothing is
  // targeted (the caller normally skips it via active()).
  //
  // `snap` (used by prepare) skips smoothing and slew: the values are applied exactly.
  template <class Set>
  void update(Set&& set, double frame, int32_t n, double anchorFrame, double framesPerBeat,
              bool snap = false) {
    if (mask_ == 0) return;
    float sum[kMaxModParams] = {};
    for (auto& s : slots_) {
      if (!s.cfg.enabled) continue;
      const float raw = s.lfo.value(s.cfg, frame, sampleRate_, anchorFrame, framesPerBeat);
      if (snap || !s.smoothValid || frame - s.lastFrame > 4.0 * kModChunkFrames) {
        s.smooth = raw;  // fresh LFO or a long idle gap: no stale ramp
        s.smoothValid = true;
      } else {
        const float a = n >= kModChunkFrames
                            ? alpha_
                            : 1.0f - std::exp(-static_cast<float>(n) /
                                              static_cast<float>(sampleRate_ * kModSmoothSeconds));
        s.smooth += a * (raw - s.smooth);
      }
      s.lastFrame = frame;
      sum[indexOf(s.cfg.target)] += s.smooth * s.cfg.amount;
    }
    const float maxOct = snap ? 1e9f : kModMaxOctavesPerChunk * static_cast<float>(n) / kModChunkFrames;
    for (int32_t i = 0; i < n_; i++) {
      if (!(mask_ & (1u << i))) continue;
      float eff = modulatedValue(dom_[i], base_[i], sum[i], lo_[i], hi_[i]);
      if (dom_[i] == ModDomain::Log && last_[i] > 0.0f && eff > 0.0f) {
        const float step = std::clamp(std::log2(eff / last_[i]), -maxOct, maxOct);
        eff = std::clamp(last_[i] * std::exp2(step), lo_[i], hi_[i]);
      }
      if (eff != last_[i]) {
        last_[i] = eff;
        set(id_[i], eff);
      }
    }
  }

 private:
  struct Slot {
    LfoConfig cfg;
    Lfo lfo;
    float smooth = 0.0f;
    bool smoothValid = false;
    double lastFrame = 0.0;
  };

  int32_t indexOf(int32_t paramId) const {
    for (int32_t i = 0; i < n_; i++) {
      if (id_[i] == paramId) return i;
    }
    return -1;
  }
  void rebuildMask() {
    mask_ = 0;
    for (const auto& s : slots_) {
      if (s.cfg.enabled) mask_ |= 1u << indexOf(s.cfg.target);
    }
  }

  double sampleRate_ = 48000.0;
  float alpha_ = 0.3f;
  int32_t n_ = 0;
  int32_t id_[kMaxModParams] = {};
  float lo_[kMaxModParams] = {}, hi_[kMaxModParams] = {};
  ModDomain dom_[kMaxModParams] = {};
  float base_[kMaxModParams] = {};
  float last_[kMaxModParams] = {};
  uint32_t mask_ = 0;  // bit i = param index i has at least one enabled LFO
  Slot slots_[kMaxLfos];
};

// A synth's modulation bank: ParamModulation configured from the kind's own parameter table, with
// the instrument as the sink.
class InstrumentModulation : public ParamModulation {
 public:
  void configure(int32_t kind, double sampleRate) {
    ModParamSpec specs[kMaxModParams];
    const int32_t n = std::min(instrumentParamCount(kind), kMaxModParams);
    for (int32_t i = 0; i < n; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kind, i);
      specs[i] = {p->id, p->min, p->max, p->def, resolveModDomain(*p)};
    }
    ParamModulation::configure(specs, n, sampleRate);
  }
  bool setLfo(int32_t slot, LfoConfig cfg, Instrument& inst, double frame) {
    return ParamModulation::setLfo(slot, cfg, [&](int32_t id, float v) { inst.setParam(id, v); }, frame);
  }
  void prepare(Instrument& inst, double frame, double anchorFrame, double framesPerBeat) {
    ParamModulation::prepare([&](int32_t id, float v) { inst.setParam(id, v); }, frame, anchorFrame, framesPerBeat);
  }
  void update(Instrument& inst, double frame, int32_t n, double anchorFrame, double framesPerBeat,
              bool snap = false) {
    ParamModulation::update([&](int32_t id, float v) { inst.setParam(id, v); }, frame, n, anchorFrame,
                            framesPerBeat, snap);
  }
};

}  // namespace webdsp
