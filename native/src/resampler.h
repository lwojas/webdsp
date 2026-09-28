#pragma once
#include <cstdint>

// Shared interpolation kernel used for two distinct purposes (see ARCHITECTURE.md,
// "DSP library investigation"):
//   1. Real-time per-voice playback rate / pitch (voice.cpp), where a continuous phase
//      accumulator must be sampled at an arbitrary, modulatable rate every render quantum.
//   2. One-time sample-rate conformance at load time, if a future loader path stops relying
//      on the browser's decodeAudioData for resampling.
//
// Investigated and deliberately not used here: libsamplerate (Secret Rabbit Code). It is
// mature and higher quality for offline batch conversion, but its block-oriented API is
// not a good fit for a continuously-modulated per-sample-frame read, and vendoring a full
// SRC library for a second, non-realtime-critical use (load-time conformance, which the
// browser decoder already handles) would be exactly the kind of dependency the project
// brief warns against ("don't introduce a large dependency merely because it exists").
// Cubic Hermite (Catmull-Rom) interpolation is a standard, well-documented technique used
// by trackers and samplers for this exact job, small enough to own and verify directly
// (see native/test/resampler_test.cpp).
namespace webdsp {

// Reads an interpolated sample at fractional frame position `pos` from a single channel
// buffer of `length` frames. Out-of-range neighbours are clamped to the buffer edges
// rather than wrapping, so looping is the caller's responsibility (the voice renderer wraps
// `pos` itself before calling this).
inline float cubicHermite(const float* data, int32_t length, double pos) {
  int32_t i1 = static_cast<int32_t>(pos);
  double frac = pos - static_cast<double>(i1);

  auto sample = [&](int32_t idx) -> float {
    if (idx < 0) idx = 0;
    if (idx >= length) idx = length - 1;
    return data[idx];
  };

  const float y0 = sample(i1 - 1);
  const float y1 = sample(i1);
  const float y2 = sample(i1 + 1);
  const float y3 = sample(i1 + 2);

  const float c0 = y1;
  const float c1 = 0.5f * (y2 - y0);
  const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

  const float t = static_cast<float>(frac);
  return ((c3 * t + c2) * t + c1) * t + c0;
}

inline float linear(const float* data, int32_t length, double pos) {
  int32_t i0 = static_cast<int32_t>(pos);
  int32_t i1 = i0 + 1;
  double frac = pos - static_cast<double>(i0);
  if (i0 < 0) i0 = 0;
  if (i0 >= length) i0 = length - 1;
  if (i1 < 0) i1 = 0;
  if (i1 >= length) i1 = length - 1;
  return static_cast<float>(data[i0] * (1.0 - frac) + data[i1] * frac);
}

}  // namespace webdsp
