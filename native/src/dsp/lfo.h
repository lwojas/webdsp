#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// A low-frequency oscillator: a small, parameter-agnostic modulation *source*. It produces a
// normalized signal in [-1, +1] and knows nothing about what it will be applied to (filters,
// pitch, pans, ...) — that is the modulation layer's job (modulation.h). Design and rationale:
// docs/lfo-modulation.md.
//
//   output = shape(waveform, frac(cycles + phase))        // cycles: free-running or beat-locked
//
// Two ways to derive `cycles`:
//   - Free:   an accumulator advanced by rate (Hz) x elapsed seconds. It keeps its own time, so
//             it runs through silence and is independent of the transport.
//   - Synced: a pure function of the engine's beat grid, (frame - anchor) / period, where the
//             period is a musical division of a beat. No state: it stays aligned through tempo
//             changes (re-derived from the grid) and silence, exactly as Pulse's pump does.
// `phase` (0..1) is a constant offset added to either, i.e. the LFO's starting phase.
//
// Waveforms are closed-form and cheap. Square is a hard +-1 here; softening it is the modulation
// layer's smoothing, not the oscillator's concern.
namespace webdsp {

enum class LfoWaveform : int32_t { Sine = 0, Triangle = 1, Square = 2, Saw = 3 };
inline constexpr int32_t kLfoWaveformCount = 4;

inline constexpr float kLfoMinRateHz = 0.01f;
inline constexpr float kLfoMaxRateHz = 40.0f;
inline constexpr float kLfoDefaultRateHz = 1.0f;

// Sync divisions, slowest to fastest. Values are the cycle length in beats (a beat is a quarter
// note, as in the engine's beat grid). "1/4" is one cycle per beat; dotted = x1.5, triplet = x2/3.
inline constexpr double kLfoDivisionBeats[] = {
    16.0,        // 4 bars
    8.0,         // 2 bars
    4.0,         // 1/1  (1 bar)
    3.0,         // 1/2.
    2.0,         // 1/2
    1.5,         // 1/4.
    4.0 / 3.0,   // 1/2T
    1.0,         // 1/4
    0.75,        // 1/8.
    2.0 / 3.0,   // 1/4T
    0.5,         // 1/8
    0.375,       // 1/16.
    1.0 / 3.0,   // 1/8T
    0.25,        // 1/16
    1.0 / 6.0,   // 1/16T
    0.125,       // 1/32
};
inline constexpr int32_t kLfoDivisionCount =
    static_cast<int32_t>(sizeof(kLfoDivisionBeats) / sizeof(kLfoDivisionBeats[0]));
inline constexpr int32_t kLfoDefaultDivision = 7;  // 1/4
inline constexpr const char* kLfoDivisionNames[] = {
    "4 bars", "2 bars", "1/1",  "1/2.", "1/2",   "1/4.", "1/2T", "1/4",
    "1/8.",   "1/4T",   "1/8",  "1/16.", "1/8T", "1/16", "1/16T", "1/32",
};
inline constexpr const char* kLfoWaveformNames[] = {"sine", "triangle", "square", "saw"};

inline int32_t lfoDivisionIndex(int32_t v) { return std::clamp(v, 0, kLfoDivisionCount - 1); }
inline int32_t lfoWaveformIndex(int32_t v) { return std::clamp(v, 0, kLfoWaveformCount - 1); }

// Waveform value for a phase in [0, 1). Sine and triangle start at 0 and rise; saw ramps -1 -> +1
// over the cycle; square is +1 for the first half, -1 for the second.
inline float lfoShape(LfoWaveform w, double phase) {
  const double p = phase - std::floor(phase);
  switch (w) {
    case LfoWaveform::Sine: return static_cast<float>(std::sin(6.283185307179586 * p));
    case LfoWaveform::Triangle: {
      const double t = p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0);
      return static_cast<float>(t);
    }
    case LfoWaveform::Square: return p < 0.5 ? 1.0f : -1.0f;
    case LfoWaveform::Saw: return static_cast<float>(2.0 * p - 1.0);
  }
  return 0.0f;
}

struct LfoConfig {
  bool enabled = false;
  LfoWaveform waveform = LfoWaveform::Sine;
  bool sync = false;                  // false = free-running at rateHz; true = beat-locked
  float rateHz = kLfoDefaultRateHz;   // free mode
  int32_t division = kLfoDefaultDivision;  // sync mode (index into kLfoDivisionBeats)
  float phase = 0.0f;                 // 0..1 starting phase / offset
  float amount = 0.0f;                // -1..+1, how far the target moves (see modulation.h)
  int32_t target = -1;                // instrument param id the modulation layer routes this to
  bool retrigger = false;             // free mode only: restart the cycle at each note-on
};

// Runtime state of one LFO. `advance` is called with monotonically increasing absolute frames; the
// gap between calls may be large (an idle instrument is not rendered), which is exact for the free
// accumulator because the rate does not change between calls (rate changes catch up first).
class Lfo {
 public:
  void reset(double frame = 0.0) {
    acc_ = 0.0;
    lastFrame_ = frame;
  }

  // Moves the free-running accumulator to `frame` at the current rate. Call before changing the
  // rate so the old rate is honoured up to now (the phase never jumps on a rate change).
  void catchUp(double frame, double sampleRate, float rateHz) {
    if (frame > lastFrame_) acc_ += static_cast<double>(rateHz) * (frame - lastFrame_) / sampleRate;
    lastFrame_ = std::max(lastFrame_, frame);
    acc_ -= std::floor(acc_);  // keep the accumulator small and precise
  }

  // Raw LFO value in [-1, 1] at `frame` (not smoothed, not scaled by amount).
  float value(const LfoConfig& c, double frame, double sampleRate, double anchorFrame,
              double framesPerBeat) {
    double cycles;
    if (c.sync) {
      const double period = framesPerBeat * kLfoDivisionBeats[lfoDivisionIndex(c.division)];
      cycles = (frame - anchorFrame) / std::max(period, 1.0);
    } else {
      catchUp(frame, sampleRate, c.rateHz);
      cycles = acc_;
    }
    return lfoShape(c.waveform, cycles + static_cast<double>(c.phase));
  }

  double accumulator() const { return acc_; }

 private:
  double acc_ = 0.0;
  double lastFrame_ = 0.0;
};

}  // namespace webdsp
