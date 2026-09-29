#pragma once
#include <cstdint>

// Mirrors src/runtime/types.ts VoiceParam / NodeParam. Manually kept in sync (see the
// comment on those TS enums); test/paramIds.test.ts pins the TS-side literal values.
namespace webdsp {

enum class VoiceParam : int32_t {
  Gain = 0,
  Rate = 1,
  FilterCutoff = 2,
  FilterResonance = 3,
  FilterMode = 4,
};

enum class NodeParam : int32_t {
  DelayTime = 0,
  DelayFeedback = 1,
  DelayMix = 2,
  BusGain = 3,
  FilterCutoff = 4,
  FilterResonance = 5,
  FilterMode = 6,
  ReverbDecay = 7,
  ReverbDamping = 8,
  ReverbMix = 9,
  CompThreshold = 10,
  CompRatio = 11,
  CompAttack = 12,
  CompRelease = 13,
  CompKnee = 14,
  CompMakeup = 15,
};

// Shared 0/1 values for VoiceParam::FilterMode / NodeParam::FilterMode. Not itself part of
// either enum's numeric contract (it's a value, not a param id), but kept alongside them
// since both filter instances (per-voice, per-bus) interpret it identically.
enum class FilterMode : int32_t {
  LowPass = 0,
  HighPass = 1,
};

}  // namespace webdsp
