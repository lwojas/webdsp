#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels
#include "dub_delay.h"      // kDubDelaySixteenths: the same musical divisions
#include "reverb.h"         // ReverbModulatedAllpass: the smear

// Dub Space: a tape-style dub echo and a deep synthetic space that sits INSIDE the echo's
// feedback loop, so every repeat is pushed a little further into the space than the one before.
//
//   in --*gE--> [ring] -> tape LP -> saturation -> HP --> x ----------------+-> (1-d)*x --+
//          ^                                              |                  |            |
//          |                                    (gS)      v                  |            |
//          |                          [smear: 3 modulated allpasses / side] -> s          |
//          |                                              |                               |
//          |   loop return r = (1-d)*x + d*s <------------+-- d*s ----------------------+ |
//          +----------- feedback * r                      v                             v v
//                                              [8-line FDN, dark, moving] -> t* -> wet = (1-d)x + d(s + t)
//                                                                                          |
//                                                                  out = dry*(1-mix) + wet*mix
//
// Echo and Space are independent switches (four configurations):
//   both off   -> bit-exact bypass (state cleared once, then nothing runs)
//   echo only  -> r = wet = x: a conventional dub echo (LP + saturation + HP in the loop, tape drift)
//   space only -> the input feeds the space directly; no repeats
//   both       -> every repeat is smeared by the allpass stage on its way round the loop and also feeds
//                 the FDN cloud. Repeat n keeps (1-d)^n of its defined echo; the rest has been smeared,
//                 darkened and decorrelated n times, and the cloud under it grows with each repeat.
// A section that is switched off has its send ramped out in ~30 ms (no click), then goes idle.
//
// Why the loop holds only the smear and not the whole reverb: a feedback delay network has very
// sharp resonant peaks (+40 dB at a mode of a 30 s tail), so a loop that re-injected the reverb
// would howl at the strongest mode whatever the feedback. Allpasses have exactly unity gain at every
// frequency, so loop gain <= feedback <= 0.92 by construction. The FDN still receives every repeat
// and its tail is heard, but it is a branch, not part of the recirculation. Hard bound on top of
// that: the saturator limits the loop signal.
//
// Echo: the time is a musical division of the transport beat (as DubDelay). A new division or
// tempo glides the read position with a rate-limited ease, so the repeats bend in pitch like a
// speed change on tape. Wow and flutter move the read position; Character scales them.
// Space: the smear (size scales the allpass lengths) feeds eight delay lines mixed by a Hadamard
// matrix, a one-pole low-pass per line for the darkness (damping), slowly moving read positions for
// the underwater drift. Decay sets T60 (0.8 s .. ~15 min); Size is the room only.
// Stereo: the echo is dual-mono with decorrelated drift; the space is true stereo (L feeds the even
// lines, R the odd ones, L and R are different orthogonal taps, the smear lengths differ per side).
//
// Character (0..1) is one control for the whole interaction: more tape drift, more saturation and
// bias, a darker and thinner loop, a denser and more mobile smear and more movement in the space.
namespace webdsp {

class DubSpace final : public DSPNode {
 public:
  static constexpr double kMaxSeconds = 4.0;
  static constexpr int32_t kLines = 8;
  static constexpr int32_t kSmear = 3;  // allpasses per side
  static constexpr float kMaxFeedback = 0.92f;

  explicit DubSpace(double sampleRate) : sampleRate_(sampleRate) {
    capacity_ = static_cast<int32_t>(sampleRate * kMaxSeconds) + 1;
    for (int32_t c = 0; c < kMaxDspChannels; c++) ring_[c].assign(capacity_, 0.0f);

    const double scale = sampleRate / 48000.0;
    // Mutually unrelated lengths (samples at 48 kHz, 30..80 ms) so the lines do not ring together.
    static constexpr double kBase[kLines] = {1427, 1783, 1973, 2097, 2557, 2963, 3421, 3833};
    static constexpr double kRate[kLines] = {0.113, 0.157, 0.191, 0.233, 0.271, 0.317, 0.353, 0.397};
    int32_t maxLen = 0;
    for (int32_t i = 0; i < kLines; i++) {
      baseLen_[i] = kBase[i] * scale;
      lfoInc_[i] = 2.0 * M_PI * kRate[i] / sampleRate;
      lfoPhase_[i] = 2.0 * M_PI * i / kLines;
      maxLen = std::max(maxLen, static_cast<int32_t>(baseLen_[i] * kSizeMax) + 1);
    }
    lineCap_ = maxLen + static_cast<int32_t>(kModMax * scale) + 8;
    for (int32_t i = 0; i < kLines; i++) line_[i].assign(lineCap_, 0.0f);

    // The smear: three allpasses per side, nominal lengths in ms (different per side so L and R
    // decorrelate). Each reads anywhere in [0.4, 2.1] x nominal, which covers size and the drift.
    static constexpr double kSmearMs[2][kSmear] = {{11, 23, 37}, {13, 29, 43}};
    for (int32_t c = 0; c < 2; c++) {
      for (int32_t k = 0; k < kSmear; k++) {
        const double nominal = kSmearMs[c][k] * 0.001 * sampleRate;
        smearNominal_[c][k] = static_cast<float>(nominal);
        smearBase_[c][k] = static_cast<float>(1.25 * nominal);
        smearExc_[c][k] = static_cast<float>(0.85 * nominal);
        smear_[c][k].configure(static_cast<int32_t>(smearBase_[c][k]), static_cast<int32_t>(smearExc_[c][k]) + 1);
        smearPhase_[c][k] = 2.0 * M_PI * (c * kSmear + k) / (2 * kSmear);
        smearInc_[c][k] = 2.0 * M_PI * (0.17 + 0.071 * (c * kSmear + k)) / sampleRate;
      }
    }

    ctlSeconds_ = kCtl / sampleRate;
    kSlow_ = 1.0f - std::exp(-static_cast<float>(ctlSeconds_ / 0.020));
    kSize_ = 1.0f - std::exp(-static_cast<float>(ctlSeconds_ / 0.35));
    sendStep_ = static_cast<float>(1.0 / (0.030 * sampleRate));
    mixStep_ = static_cast<float>(1.0 / (0.010 * sampleRate));
    timeK_ = static_cast<float>(1.0 / (0.08 * sampleRate));
    inHpA_ = std::exp(-6.2831853f * 20.0f / static_cast<float>(sampleRate));
    updateTarget();
    snapControls();
    reset();
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (!dirty_) {  // nothing in the memory yet: the sends start where the switches are
      gE_ = echoOn_ ? 1.0f : 0.0f;
      gS_ = spaceOn_ ? 1.0f : 0.0f;
    }
    // Idle: mix 0 and finished gliding, or both sections off and their sends ramped out. Drop the
    // memory once so the next engage starts clean (same rule as every other FX).
    const bool idle = (mixTarget_ <= 0.0f && mixCur_ <= kSilence) ||
                      (!echoOn_ && !spaceOn_ && gE_ <= kSilence && gS_ <= kSilence);
    if (idle) {
      if (dirty_) {
        reset();
        dirty_ = false;
      }
      return;
    }
    dirty_ = true;
    if (!started_) {
      delayCur_ = targetFrames_;
      started_ = true;
    }
    const bool stereo = numChannels > 1;
    float* left = channels[0];
    float* right = stereo ? channels[1] : channels[0];

    for (int32_t at = 0; at < numFrames; at += kCtl) {
      const int32_t len = std::min(kCtl, numFrames - at);
      tick(len);
      for (int32_t i = at; i < at + len; i++) {
        const float in[2] = {left[i], stereo ? right[i] : left[i]};
        float out[2];
        sample(in, out);
        left[i] = out[0];
        if (stereo) right[i] = out[1];
      }
    }
  }

  void setParam(int32_t param, float value) override {
    if (!std::isfinite(value)) return;
    switch (static_cast<NodeParam>(param)) {
      case NodeParam::DubSpaceEchoOn: echoOn_ = value >= 0.5f; break;
      case NodeParam::DubSpaceSpaceOn: spaceOn_ = value >= 0.5f; break;
      case NodeParam::DubSpaceTime:
        division_ = std::clamp(static_cast<int32_t>(std::lround(value)), 0, kDubDelayDivisionCount - 1);
        updateTarget();
        break;
      case NodeParam::DubSpaceFeedback: fbT_ = std::clamp(value, 0.0f, kMaxFeedback); break;
      case NodeParam::DubSpaceTone: toneT_ = std::clamp(value, 200.0f, 16000.0f); break;
      case NodeParam::DubSpaceDecay: decayT_ = std::clamp(value, 0.0f, 1.0f); break;
      case NodeParam::DubSpaceSize: sizeT_ = std::clamp(value, 0.0f, 1.0f); break;
      case NodeParam::DubSpaceDepth: depthT_ = std::clamp(value, 0.0f, 1.0f); break;
      case NodeParam::DubSpaceDamping: dampT_ = std::clamp(value, 0.0f, 1.0f); break;
      case NodeParam::DubSpaceCharacter: charT_ = std::clamp(value, 0.0f, 1.0f); break;
      case NodeParam::DubSpaceMix: mixTarget_ = std::clamp(value, 0.0f, 1.0f); break;
      default: break;
    }
  }

  void setTempo(double framesPerBeat) override {
    if (!(framesPerBeat > 0.0) || !std::isfinite(framesPerBeat)) return;
    framesPerBeat_ = framesPerBeat;
    updateTarget();
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      std::fill(ring_[c].begin(), ring_[c].end(), 0.0f);
      writePos_ = 0;
      tapeLp_[c] = hpX_[c] = hpY_[c] = inHpX_[c] = inHpY_[c] = 0.0f;
      for (auto& a : smear_[c]) a.clear();
    }
    for (int32_t i = 0; i < kLines; i++) {
      std::fill(line_[i].begin(), line_[i].end(), 0.0f);
      lineLp_[i] = 0.0f;
    }
    lineW_ = 0;
    gE_ = echoOn_ ? 1.0f : 0.0f;  // the memory is empty, so there is nothing to click against
    gS_ = spaceOn_ ? 1.0f : 0.0f;
    mixCur_ = 0.0f;  // the dry/wet balance glides in from the dry signal
    started_ = false;
    snapControls();
  }

  // Exposed for tests: the settled delay in frames for the current division and tempo.
  double targetFrames() const { return targetFrames_; }

 private:
  static constexpr int32_t kCtl = 16;         // control-rate block, frames
  static constexpr double kSizeMin = 0.6, kSizeMax = 2.8;  // line length multiplier at size 0 / 1
  static constexpr double kModMax = 130.0;    // largest read-position excursion, samples at 48 kHz
  static constexpr double kMaxSlew = 0.3;     // frames of read-position change per sample (pitch +-30%)
  static constexpr double kMinSlew = 0.002;   // the glide always arrives: no endless exponential tail
  static constexpr float kSilence = 1e-4f;
  // Level calibration of the space: output RMS ~= input RMS for noise at any size (see dub_space_test).
  static constexpr float kSpaceCal = 1.0f;

  // Cheap monotone tanh (Pade), exact enough for a soft clipper and free of libm in the inner loop.
  static float softClip(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
  }
  static float flush(float x) { return std::fabs(x) < 1e-20f ? 0.0f : x; }

  void updateTarget() {
    double frames = framesPerBeat_ * 0.25 * kDubDelaySixteenths[division_];
    const double maxFrames = static_cast<double>(capacity_ - 3 - 512);  // headroom for the tape drift
    while (frames > maxFrames) frames *= 0.5;
    targetFrames_ = std::max(2.0, std::round(frames));
  }

  void snapControls() {
    fbCur_ = fbT_;
    toneCur_ = toneT_;
    sizeCur_ = sizeT_;
    decayCur_ = decayT_;
    depthCur_ = depthT_;
    dampCur_ = dampT_;
    charCur_ = charT_;
    fCur_ = static_cast<float>(kSizeMin + (kSizeMax - kSizeMin) * sizeCur_);
    for (int32_t i = 0; i < kLines; i++) offs_[i] = 0.0f, offsInc_[i] = 0.0f;
    modOff_[0] = modOff_[1] = 0.0f;
    for (int32_t c = 0; c < 2; c++)
      for (int32_t k = 0; k < kSmear; k++) smearLfo_[c][k] = smearLfoInc_[c][k] = 0.0f;
    modInc_[0] = modInc_[1] = 0.0f;
    fInc_ = gEInc_ = gSInc_ = mixInc_ = 0.0f;
    derive();
  }

  // Everything that follows from the smoothed controls and is constant within a control block.
  void derive() {
    const float c = charCur_;
    const float sr = static_cast<float>(sampleRate_);
    // Echo loop: the tape low-pass darkens with Character, the high-pass thins the loop.
    const float lpHz = std::max(150.0f, toneCur_ * (1.0f - 0.5f * c));
    lpA_ = std::exp(-6.2831853f * lpHz / sr);
    const float hpHz = 70.0f + 130.0f * c;
    hpA_ = std::exp(-6.2831853f * hpHz / sr);
    drive_ = 1.0f + 2.5f * c;
    bias_ = 0.12f * c;
    const float tb = softClip(bias_);
    satNorm_ = 1.0f / (drive_ * (1.0f - tb * tb));
    satBias_ = tb;
    fbEff_ = fbCur_;
    // Space: darkness from damping (6 kHz .. 480 Hz), a little more with Character.
    const float dampHz = 6000.0f * std::pow(0.08f, dampCur_) * (1.0f - 0.35f * c);
    dampA_ = std::exp(-6.2831853f * dampHz / sr);
    apG_ = 0.45f + 0.27f * c;
    smearDrift_ = static_cast<float>((3.0 + 30.0 * c) * sampleRate_ / 48000.0);
    modDepth_ = static_cast<float>((10.0 + 110.0 * c) * sampleRate_ / 48000.0);
    tapeWow_ = static_cast<float>((1.5 + 20.0 * c) * sampleRate_ / 48000.0);
    tapeFlut_ = static_cast<float>((0.15 + 1.6 * c) * sampleRate_ / 48000.0);
  }

  void tick(int32_t len) {
    const float n = static_cast<float>(len);
    fbCur_ += kSlow_ * (fbT_ - fbCur_);
    toneCur_ += kSlow_ * (toneT_ - toneCur_);
    depthCur_ += kSlow_ * (depthT_ - depthCur_);
    dampCur_ += kSlow_ * (dampT_ - dampCur_);
    charCur_ += kSlow_ * (charT_ - charCur_);
    sizeCur_ += kSize_ * (sizeT_ - sizeCur_);
    decayCur_ += kSize_ * (decayT_ - decayCur_);
    derive();

    // Linear ramps across the block for everything that would zipper.
    auto ramp = [n](float cur, float target, float step) {
      return std::clamp(target - cur, -step * n, step * n) / n;
    };
    gEInc_ = ramp(gE_, echoOn_ ? 1.0f : 0.0f, sendStep_);
    gSInc_ = ramp(gS_, spaceOn_ ? 1.0f : 0.0f, sendStep_);
    mixInc_ = ramp(mixCur_, mixTarget_, mixStep_);
    const float fNext = static_cast<float>(kSizeMin + (kSizeMax - kSizeMin) * sizeCur_);
    fInc_ = (fNext - fCur_) / n;

    // Space: per-line lowpass-included gain for the T60, and the moving read positions.
    const float t60 = 0.8f * std::exp(7.0f * decayCur_);  // 0.8 s .. ~875 s, exponential
    const float sr = static_cast<float>(sampleRate_);
    for (int32_t i = 0; i < kLines; i++) {
      gLine_[i] = std::exp(-6.9078f * static_cast<float>(baseLen_[i]) * fNext / (t60 * sr));
      lfoPhase_[i] += lfoInc_[i] * kCtl;
      if (lfoPhase_[i] > 2.0 * M_PI) lfoPhase_[i] -= 2.0 * M_PI;
      const float next = modDepth_ * static_cast<float>(std::sin(lfoPhase_[i]));
      offsInc_[i] = (next - offs_[i]) / n;
    }
    // Level: constant onset for the tail would be 1/sqrt(T60) in energy terms but that makes long tails inaudible; T60^-1/4 is the
    // compromise (a sustained pad swells by 6 dB per x16 in decay, the tail onset falls by 6 dB).
    comp_ = kSpaceCal * std::sqrt(fNext) * std::pow(t60 * (1.0f / 6.0f), -0.25f) * 0.41f;

    // Smear: lengths follow size (x0.5 .. x2) plus a slow drift; expressed as a position in each
    // allpass' modulation range.
    const float sizeScale = 0.5f + 1.5f * sizeCur_;
    for (int32_t c = 0; c < 2; c++) {
      for (int32_t k = 0; k < kSmear; k++) {
        smearPhase_[c][k] += smearInc_[c][k] * kCtl;
        if (smearPhase_[c][k] > 2.0 * M_PI) smearPhase_[c][k] -= 2.0 * M_PI;
        const float target = smearNominal_[c][k] * sizeScale + smearDrift_ * static_cast<float>(std::sin(smearPhase_[c][k]));
        const float pos = std::clamp((target - smearBase_[c][k]) / smearExc_[c][k], -1.0f, 1.0f);
        smearLfoInc_[c][k] = (pos - smearLfo_[c][k]) / n;
      }
    }

    // Tape drift: wow (slow), a second incommensurate wander, and flutter; right is out of phase.
    wowPhase_ += 2.0 * M_PI * 0.61 * kCtl / sampleRate_;
    driftPhase_ += 2.0 * M_PI * 0.23 * kCtl / sampleRate_;
    flutPhase_ += 2.0 * M_PI * 6.3 * kCtl / sampleRate_;
    if (wowPhase_ > 2.0 * M_PI) wowPhase_ -= 2.0 * M_PI;
    if (driftPhase_ > 2.0 * M_PI) driftPhase_ -= 2.0 * M_PI;
    if (flutPhase_ > 2.0 * M_PI) flutPhase_ -= 2.0 * M_PI;
    for (int32_t c = 0; c < 2; c++) {
      const double ph = c == 0 ? 0.0 : 1.9;
      const float next = tapeWow_ * static_cast<float>(0.7 * std::sin(wowPhase_ + ph) + 0.3 * std::sin(driftPhase_ + 2.0 * ph)) +
                         tapeFlut_ * static_cast<float>(std::sin(flutPhase_ + 3.0 * ph));
      modInc_[c] = (next - modOff_[c]) / n;
    }
  }

  float readLine(int32_t i, float length) const {
    float r = static_cast<float>(lineW_) - length;
    if (r < 0.0f) r += static_cast<float>(lineCap_);
    const int32_t r0 = std::min(static_cast<int32_t>(r), lineCap_ - 1);
    const float frac = r - static_cast<float>(r0);
    const int32_t r1 = r0 + 1 == lineCap_ ? 0 : r0 + 1;
    return line_[i][r0] + frac * (line_[i][r1] - line_[i][r0]);
  }

  // The smear: the in-loop part of the space. Allpasses only, so unity gain at every frequency.
  void smear(const float* in, float* out) {
    for (int32_t c = 0; c < 2; c++) {
      float v = in[c];
      for (int32_t k = 0; k < kSmear; k++) {
        smearLfo_[c][k] += smearLfoInc_[c][k];
        v = smear_[c][k].process(v, apG_, smearLfo_[c][k]);
      }
      out[c] = flush(v);
    }
  }

  // The cloud: the smeared repeats into the FDN. Stereo out, level-compensated, NOT recirculated
  // through the echo loop.
  void cloud(const float* in, float* out) {
    float a[2];
    for (int32_t c = 0; c < 2; c++) {
      // Subsonic guard: the long tail must not collect DC.
      const float x = in[c] * comp_;
      inHpY_[c] = inHpA_ * (inHpY_[c] + x - inHpX_[c]);
      inHpX_[c] = x;
      a[c] = flush(inHpY_[c]);
    }
    float v[kLines];
    for (int32_t i = 0; i < kLines; i++) {
      const float rd = readLine(i, static_cast<float>(baseLen_[i]) * fCur_ + offs_[i]);
      lineLp_[i] = flush(dampA_ * lineLp_[i] + (1.0f - dampA_) * rd * gLine_[i]);
      v[i] = lineLp_[i];
    }
    // Two different orthogonal (Sylvester Hadamard) rows give decorrelated left and right.
    out[0] = 0.5f * (v[0] + v[1] - v[2] - v[3] + v[4] + v[5] - v[6] - v[7]);
    out[1] = 0.5f * (v[0] - v[1] - v[2] + v[3] - v[4] + v[5] + v[6] - v[7]);
    for (int32_t len = 1; len < kLines; len <<= 1) {
      for (int32_t i = 0; i < kLines; i += len << 1) {
        for (int32_t j = i; j < i + len; j++) {
          const float x = v[j], y = v[j + len];
          v[j] = x + y;
          v[j + len] = x - y;
        }
      }
    }
    constexpr float kNorm = 0.35355339f;  // 1/sqrt(8): the matrix is orthogonal, so lossless
    for (int32_t i = 0; i < kLines; i++) line_[i][lineW_] = v[i] * kNorm + a[i & 1];
    if (++lineW_ == lineCap_) lineW_ = 0;
  }

  void sample(const float* in, float* out) {
    gE_ += gEInc_;
    gS_ += gSInc_;
    mixCur_ += mixInc_;
    fCur_ += fInc_;
    delayCur_ = glide(delayCur_, targetFrames_);
    for (int32_t i = 0; i < kLines; i++) offs_[i] += offsInc_[i];

    // Echo read: the tape LP, the saturator, then the loop high-pass.
    float x[2];
    for (int32_t c = 0; c < 2; c++) {
      modOff_[c] += modInc_[c];
      double r = static_cast<double>(writePos_) - (delayCur_ + modOff_[c]);
      if (r < 0.0) r += capacity_;
      const int32_t r0 = std::min(static_cast<int32_t>(r), capacity_ - 1);
      const float frac = static_cast<float>(r - r0);
      const int32_t r1 = r0 + 1 == capacity_ ? 0 : r0 + 1;
      const float e = ring_[c][r0] + frac * (ring_[c][r1] - ring_[c][r0]);

      tapeLp_[c] = flush(lpA_ * tapeLp_[c] + (1.0f - lpA_) * e);
      const float s = (softClip(drive_ * tapeLp_[c] + bias_) - satBias_) * satNorm_;
      hpY_[c] = flush(hpA_ * (hpY_[c] + s - hpX_[c]));
      hpX_[c] = s;
      x[c] = hpY_[c] * gE_;
    }

    // Space send: the repeats when the echo is on, the input itself when it is off.
    float sIn[2], sm[2], t[2];
    for (int32_t c = 0; c < 2; c++) sIn[c] = gS_ * (x[c] + in[c] * (1.0f - gE_));
    smear(sIn, sm);
    cloud(sm, t);

    // Depth: the share of each pass that is smeared (echo on). With the echo off there is no defined
    // signal to keep, so the space's wet level has a floor.
    const float smearHeard = 0.15f + 0.35f * gE_;  // the cloud is the voice of the space; the smear is its onset
    const float mixEff = mixCur_ * std::max(gE_, gS_);  // both sends ramped out = the dry signal, with no step
    const float dLoop = depthCur_ * gS_;
    const float dHeard = (depthCur_ + (1.0f - gE_) * 0.4f * (1.0f - depthCur_)) * gS_;
    for (int32_t c = 0; c < 2; c++) {
      const float r = x[c] * (1.0f - dLoop) + sm[c] * dLoop;               // what goes round the loop
      const float wet = x[c] * (1.0f - dHeard) + (smearHeard * sm[c] + t[c]) * dHeard;  // what is heard
      ring_[c][writePos_] = gE_ * (in[c] + fbEff_ * r);
      out[c] = in[c] * (1.0f - mixEff) + wet * mixEff;
    }
    if (++writePos_ == capacity_) writePos_ = 0;
  }

  // An eased, rate-limited approach to the target delay: pitch bends by at most kMaxSlew.
  double glide(double d, double target) const {
    const double err = target - d;
    const double mag = std::fabs(err);
    if (mag <= kMinSlew) return target;
    double v = std::clamp(mag * timeK_, kMinSlew, kMaxSlew);
    v = std::min(v, mag);
    return err > 0.0 ? d + v : d - v;
  }

  double sampleRate_;
  int32_t capacity_ = 0;
  int32_t lineCap_ = 0;
  double ctlSeconds_ = 0.0;
  float kSlow_ = 0.0f, kSize_ = 0.0f, sendStep_ = 0.0f, mixStep_ = 0.0f, timeK_ = 0.0f;
  float inHpA_ = 0.0f;

  // Controls (targets, written by setParam) and their smoothed values.
  bool echoOn_ = true, spaceOn_ = true;
  int32_t division_ = 2;  // 1/8 dotted
  double framesPerBeat_ = 24000.0;
  double targetFrames_ = 0.0;
  float fbT_ = 0.55f, toneT_ = 4000.0f, sizeT_ = 0.5f, decayT_ = 0.35f, depthT_ = 0.5f, dampT_ = 0.5f, charT_ = 0.3f;
  float fbCur_ = 0.0f, toneCur_ = 0.0f, sizeCur_ = 0.0f, decayCur_ = 0.0f, depthCur_ = 0.0f, dampCur_ = 0.0f, charCur_ = 0.0f;
  float mixTarget_ = 0.0f, mixCur_ = 0.0f;  // 0 => bypass, like every other FX
  float gE_ = 1.0f, gS_ = 1.0f;             // the sends: ramped on and off by the switches

  // Derived per control block.
  float lpA_ = 0.0f, hpA_ = 0.0f, drive_ = 1.0f, bias_ = 0.0f, satNorm_ = 1.0f, satBias_ = 0.0f, fbEff_ = 0.0f;
  float dampA_ = 0.0f, apG_ = 0.5f, modDepth_ = 0.0f, tapeWow_ = 0.0f, tapeFlut_ = 0.0f, comp_ = 0.0f;
  float gEInc_ = 0.0f, gSInc_ = 0.0f, mixInc_ = 0.0f, fInc_ = 0.0f;

  // Echo state.
  std::vector<float> ring_[kMaxDspChannels];
  int32_t writePos_ = 0;
  double delayCur_ = 0.0;
  bool started_ = false;
  float tapeLp_[kMaxDspChannels] = {}, hpX_[kMaxDspChannels] = {}, hpY_[kMaxDspChannels] = {};
  double wowPhase_ = 0.0, driftPhase_ = 1.3, flutPhase_ = 0.4;
  float modOff_[2] = {}, modInc_[2] = {};

  // Space state.
  std::vector<float> line_[kLines];
  int32_t lineW_ = 0;
  double baseLen_[kLines] = {}, lfoInc_[kLines] = {}, lfoPhase_[kLines] = {};
  float lineLp_[kLines] = {}, gLine_[kLines] = {}, offs_[kLines] = {}, offsInc_[kLines] = {};
  float fCur_ = 1.0f;
  float inHpX_[2] = {}, inHpY_[2] = {};
  ReverbModulatedAllpass smear_[2][kSmear];
  float smearNominal_[2][kSmear] = {}, smearBase_[2][kSmear] = {}, smearExc_[2][kSmear] = {};
  float smearLfo_[2][kSmear] = {}, smearLfoInc_[2][kSmear] = {}, smearDrift_ = 0.0f;
  double smearPhase_[2][kSmear] = {}, smearInc_[2][kSmear] = {};

  bool dirty_ = false;  // processed since the last reset(): the buffers may hold audio
};

}  // namespace webdsp
