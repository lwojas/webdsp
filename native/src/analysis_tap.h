#pragma once
#include <array>
#include <cstdint>

// A realtime-safe mono sample tap for client-side spectrum display. The engine only COPIES
// samples here (one mono sum per block, no allocation, no FFT); the analysis itself happens on
// the UI side (src/runtime/spectrum.ts), so the audio thread never pays for it. One tap exists
// slot (Engine::setAnalysisTap), attached to the input or the output of one effect instance.
namespace webdsp {

constexpr int32_t kAnalysisWindow = 2048;  // samples handed to the client per read
constexpr int32_t kAnalysisRing = 4096;    // power of two, >= kAnalysisWindow
constexpr int32_t kMaxAnalysisTaps = 4;    // simultaneous taps (e.g. the input and output of an EQ)

class AnalysisTap {
 public:
  void write(float* const* channels, int32_t numChannels, int32_t numFrames) {
    if (numChannels <= 0) return;
    const float scale = 1.0f / static_cast<float>(numChannels);
    for (int32_t i = 0; i < numFrames; i++) {
      float s = 0.0f;
      for (int32_t c = 0; c < numChannels; c++) s += channels[c][i];
      ring_[pos_ & (kAnalysisRing - 1)] = s * scale;
      pos_++;
    }
  }

  // Total frames written since the last clear(); a consumer compares it to see if anything new
  // arrived (an idle or dormant bus writes nothing). Wraps harmlessly.
  uint32_t written() const { return pos_; }

  // Copies the most recent kAnalysisWindow samples, oldest first, into the snapshot and returns it.
  const float* snapshot() {
    for (int32_t i = 0; i < kAnalysisWindow; i++) {
      snap_[i] = ring_[(pos_ - kAnalysisWindow + static_cast<uint32_t>(i)) & (kAnalysisRing - 1)];
    }
    return snap_.data();
  }

  void clear() {
    ring_.fill(0.0f);
    pos_ = 0;
  }

 private:
  std::array<float, kAnalysisRing> ring_{};
  std::array<float, kAnalysisWindow> snap_{};
  uint32_t pos_ = 0;
};

}  // namespace webdsp
