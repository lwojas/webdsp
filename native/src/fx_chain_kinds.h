#pragma once
#include <cstdint>

// The effect kinds, split out of fx_chain.h so parameter tables (fx_params.h) can name them
// without pulling in every DSP class.
namespace webdsp {

// Mirrors src/runtime/types.ts EffectKind.
enum class FxKind : int32_t {
  Filter = 0,
  ChorusFlanger = 1,
  Delay = 2,
  Reverb = 3,
  Compressor = 4,
  Saturation = 5,
  Phaser = 6,
  Bitcrusher = 7,
  Utility = 8,
  DubDelay = 9,
  Eq = 10,
  Glitch = 11,
};
constexpr int32_t kFxKindCount = 12;

}  // namespace webdsp
