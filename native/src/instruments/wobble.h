#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/lfo.h"
#include "../dsp/matrix_filter.h"
#include "../dsp/percussion.h"

// Wobble: an opinionated bass-music bass (DnB, dubstep, grime). Shipped behaviour and parameter mapping:
// docs/wobble-instrument.md.
//
//   saw + pulse (PolyBLEP) <- phase-modulated by a sine at ~2 f0     (growl: FM index, enveloped by the wobble)
//        |  src
//        +--> ZDF ladder LP 24 dB  <- cutoff = f(cutoff, wobble, bite zap), Q = f(resonance, wobble)
//        +--> presence band (bite) -> fold -----------------+--> mid
//        +--> two moving formant band-passes (growl) -------+
//                                  mid --> tanh drive, gain-compensated, DC-blocked
//   sine at f0 (+ a little 3rd) = sub, never filtered, never wobbled ---------------> + (weight)
//        --> x amp env^2 x velocity --> soft clip (bounded +-1) --> Panner --> bus
//
// What makes it a wobble bass rather than "VA + filter LFO": ONE wobble signal moves the filter, the FM
// index, the resonance and the formant vowel together (depth set by one knob); the low end is a separate
// sub path that the wobble cannot touch, so the bass stays heavy while the mids move; and drive, weight and
// growl are gain-compensated against measured loudness so a macro changes colour, not volume.
//
// Reused: AdsrEnvelope (amp), MatrixFilter (ZDF ladder, stable under per-sample retuning), PercBandpass,
// PercCore (lifecycle/pan/pre-note delay), and the sample-and-hold hash of dsp/lfo.h. The wobble is tempo-locked (Instrument::syncTransport), like Pulse's pump: its phase is a pure
// function of the engine beat grid, so a note on the beat always starts the same point of the cycle (a
// sequencer sees consistent wubs), and tempo changes never jump. `freerate` > 0 makes it free-running and
// restarts the cycle at each note.
//
// Mono per instance (bass is mono). A fresh note resets all state, so identical inputs render identical samples.
// A retrigger of a sounding note is continuous (phase, filter and envelopes carry). A note tied with setTie
// glides into the held note without re-attacking.
namespace webdsp {

// Cycle length in beats (a beat is a quarter note), slow to fast. Curated from the LFO table: the divisions
// that matter in bass music. Index = `rate`.
inline constexpr double kWobbleRateBeats[] = {
    4.0,         // 1/1
    2.0,         // 1/2
    1.5,         // 1/4.
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
inline constexpr int32_t kWobbleRateCount =
    static_cast<int32_t>(sizeof(kWobbleRateBeats) / sizeof(kWobbleRateBeats[0]));
inline constexpr const char* kWobbleRateNames[] = {"1/1", "1/2", "1/4.", "1/4", "1/8.", "1/4T",
                                                   "1/8", "1/16.", "1/8T", "1/16", "1/16T", "1/32"};
inline constexpr const char* kWobbleShapeNames[] = {"smooth", "square", "ramp", "random"};

inline constexpr InstrumentParamInfo kWobbleParams[] = {
    {static_cast<int32_t>(WobbleParam::Wobble), "wobble", 0.0f, 1.0f, 0.6f, "", ModDomain::Linear},
    {static_cast<int32_t>(WobbleParam::Rate), "rate", 0.0f, 11.0f, 6.0f, "step"},
    {static_cast<int32_t>(WobbleParam::Shape), "shape", 0.0f, 3.0f, 0.0f, "step"},
    {static_cast<int32_t>(WobbleParam::Growl), "growl", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(WobbleParam::Bite), "bite", 0.0f, 1.0f, 0.45f, "", ModDomain::Linear},
    {static_cast<int32_t>(WobbleParam::Weight), "weight", 0.0f, 1.0f, 0.6f, "", ModDomain::Linear},
    {static_cast<int32_t>(WobbleParam::Drive), "drive", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(WobbleParam::Cutoff), "cutoff", 80.0f, 8000.0f, 650.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(WobbleParam::Resonance), "resonance", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(WobbleParam::Tune), "tune", 27.5f, 110.0f, 55.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(WobbleParam::Release), "release", 0.01f, 1.5f, 0.12f, "s"},
    {static_cast<int32_t>(WobbleParam::FreeRate), "freerate", 0.0f, 20.0f, 0.0f, "Hz", ModDomain::None},
    {static_cast<int32_t>(WobbleParam::Glide), "glide", 0.0f, 0.5f, 0.06f, "s"},
};
inline constexpr int32_t kWobbleParamCount =
    static_cast<int32_t>(sizeof(kWobbleParams) / sizeof(kWobbleParams[0]));

class WobbleInstrument final : public Instrument {
 public:
  WobbleInstrument() {
    for (int32_t i = 0; i < kWobbleParamCount; i++) values_[i] = kWobbleParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Wobble; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    ampEnv_.reset();
    filter_.configure(sampleRate);
    dcBlockR_ = static_cast<float>(1.0 - kTwoPi * kDcCornerHz / sampleRate);
    framesPerBeat_ = sampleRate * 0.5;
    clearState();
  }

  void syncTransport(int64_t frame, double anchorFrame, double framesPerBeat) override {
    frame_ = static_cast<double>(frame);
    anchorFrame_ = anchorFrame;
    framesPerBeat_ = framesPerBeat;
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    const bool tied = tied_;
    tied_ = false;  // a tie applies to one note
    const bool glide = tied && core_.active && !core_.releasing && core_.pendingDelay == 0 && delayFrames <= 0;
    if (glide) {
      const float held = core_.velocity;
      core_.begin(velocity, 0, pan, pitch);
      core_.velocity = held;  // a velocity step would be an audible level jump
      startGlide(semitonesOf(core_.pitchRatio));
      return;
    }
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();
    pitchSemis_ = targetSemis_ = semitonesOf(core_.pitchRatio);
    glideStep_ = 0.0;
    zap_ = 1.0f;  // the note-onset opening (bite); a retrigger restarts it
    freeCycles_ = 0.0;
    ampEnv_.noteOnRestart(ampParams());
  }

  void duck(int32_t frames) override { ampEnv_.duck(frames); }

  void setTie(bool tied) override { tied_ = tied; }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kWobbleParams, kWobbleParamCount, values_, param, v);
  }

  void reset() override {
    ampEnv_.reset();
    clearState();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    float* gains = scratch[1];
    const double sr = core_.sampleRate;

    const int32_t shape = std::clamp(static_cast<int32_t>(std::lround(value(WobbleParam::Shape))), 0, 3);
    const int32_t rateStep =
        std::clamp(static_cast<int32_t>(std::lround(value(WobbleParam::Rate))), 0, kWobbleRateCount - 1);
    const double freeHz = static_cast<double>(value(WobbleParam::FreeRate));
    const double periodFrames = std::max(framesPerBeat_ * kWobbleRateBeats[rateStep], 1.0);
    const double tuneHz = static_cast<double>(value(WobbleParam::Tune));
    const float dtBlock = static_cast<float>(kSubBlock / sr);
    const float smoothA = 1.0f - std::exp(-dtBlock / kSmoothSeconds);
    const float lfoSlewA = 1.0f - std::exp(-dtBlock / kShapeSlewSeconds[shape]);
    const float zapFall = std::exp(-dtBlock / kZapSeconds);
    const float vel = std::min(core_.velocity, 1.0f);
    const float velGain = vel * (0.7f + 0.3f * vel);  // soft notes are quieter and silent at 0

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t j = 0; j < lead; j++) mono[j] = gains[j] = 0.0f;
    core_.pendingDelay -= lead;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (ampEnv_.stage() == AdsrEnvelope::Stage::Idle ||
          (ampEnv_.stage() == AdsrEnvelope::Stage::Sustain && ampEnv_.level() <= 0.0f)) {
        finished = true;
        for (int32_t j = i; j < numFrames; j++) mono[j] = gains[j] = 0.0f;
        break;
      }

      // --- control rate. Targets are smoothed so macro changes never zipper.
      if (snap_) {
        sWobble_ = value(WobbleParam::Wobble);
        sGrowl_ = value(WobbleParam::Growl);
        sBite_ = value(WobbleParam::Bite);
        sWeight_ = value(WobbleParam::Weight);
        sDrive_ = value(WobbleParam::Drive);
        sCutOct_ = std::log2(value(WobbleParam::Cutoff));
        sRes_ = value(WobbleParam::Resonance);
        snap_ = false;
      } else {
        sWobble_ += smoothA * (value(WobbleParam::Wobble) - sWobble_);
        sGrowl_ += smoothA * (value(WobbleParam::Growl) - sGrowl_);
        sBite_ += smoothA * (value(WobbleParam::Bite) - sBite_);
        sWeight_ += smoothA * (value(WobbleParam::Weight) - sWeight_);
        sDrive_ += smoothA * (value(WobbleParam::Drive) - sDrive_);
        sCutOct_ += smoothA * (std::log2(value(WobbleParam::Cutoff)) - sCutOct_);
        sRes_ += smoothA * (value(WobbleParam::Resonance) - sRes_);
      }
      const float w = sWobble_, growl = sGrowl_, bite = sBite_, weight = sWeight_, drive = sDrive_;

      // The wobble: one unipolar signal u (1 = open, at the start of the cycle), slewed per shape.
      double cycles;
      if (freeHz > 0.0) {
        cycles = freeCycles_;
        freeCycles_ += freeHz * n / sr;
        freeCycles_ -= std::floor(freeCycles_);
      } else {
        cycles = (frame_ + i + 0.5 * n - anchorFrame_) / periodFrames;
      }
      const float uRaw = wobbleShape(shape, cycles);
      lfoU_ += lfoSlewA * (uRaw - lfoU_);
      const float u = lfoU_;
      const float uc = u - 0.5f;  // centred, -0.5 .. +0.5

      zap_ *= zapFall;
      const float zapNow = zap_ * bite * (0.5f + 0.5f * vel);

      if (pitchSemis_ != targetSemis_) {
        pitchSemis_ += glideStep_ * n;
        if (glideStep_ > 0.0 ? pitchSemis_ >= targetSemis_ : pitchSemis_ <= targetSemis_) {
          pitchSemis_ = targetSemis_;
        }
      }
      const double f0 = std::clamp(tuneHz * std::exp2(pitchSemis_ / 12.0), 20.0, std::min(1000.0, sr * 0.2));
      const double inc = f0 / sr;
      const double modInc = inc * kModRatio;

      // Cutoff in octaves around the Cutoff control: the wobble pushes it up more than down (+3.2 / -1.8 at
      // full depth), bite adds an onset zap. Never below the fundamental; never above 0.4 sr.
      const float wDepth = w * (0.4f + 0.6f * w);  // gentle at the bottom of the knob
      const float cutOctEnd = sCutOct_ + kSweepOct * wDepth * (u - 0.36f) + kZapOct * zapNow + 0.35f * bite;
      const float lo = static_cast<float>(std::log2(std::max(f0 * 1.0, 30.0)));
      const float hi = static_cast<float>(std::log2(std::min(14000.0, sr * 0.4)));
      const float cutEnd = std::clamp(cutOctEnd, lo, hi);
      if (!cutInit_) { cutOct_ = cutEnd; cutInit_ = true; }
      const float cutStep = (cutEnd - cutOct_) / static_cast<float>(n);

      // Growl: FM index (cycles of phase modulation), breathing with the wobble.
      const float gEff = std::clamp(growl * (1.0f - 0.55f * w + 0.9f * w * u), 0.0f, 1.2f);
      const float fmEnd = kFmMax * gEff * gEff;
      const float fmStep = (fmEnd - fmIdx_) / static_cast<float>(n);
      const float detune = 1.0f + (0.0016f + 0.0075f * growl) * 1.0f;  // osc2 detune (up to ~16 cents)

      // Resonance: more at the closed end of the wobble, bounded short of self-oscillation.
      const float resEff = std::clamp(0.05f + 0.85f * sRes_ + 0.12f * w * (1.0f - u) + 0.04f * bite, 0.0f, 0.93f);

      // Formant "vowel": two band-passes riding the same wobble.
      const float vowel = std::clamp(0.42f + 0.9f * w * uc + 0.12f * growl, 0.0f, 1.0f);
      const double f1 = 300.0 * std::exp2(1.7 * vowel);
      const double f2 = 850.0 * std::exp2(1.9 * vowel);
      formant1_.set(f1, 4.5, sr);
      formant2_.set(f2, 5.0, sr);
      // The extra paths (formants, presence) bypass the ladder, so they follow how open it is: a closed filter
      // darkens the whole sound, and Cutoff stays the obvious performance control.
      const float follow = std::clamp(0.3f + 0.7f * (cutEnd - kFollowLoOct) / kFollowSpanOct, 0.3f, 1.0f);
      const float fGain = kFormantGain * growl * (0.6f + 0.4f * u) * follow;

      // Bite: a presence band that follows the wobble a little, folded for abrasion.
      const double teethHz = 1500.0 * std::exp2(0.9 * (0.3 + 0.7 * bite) + 1.0 * w * uc);
      teeth_.set(teethHz, 1.1, sr);
      const float teethGain = kTeethGain * bite * (0.55f + 0.45f * u) * follow;
      const float foldK = 1.5f + 3.0f * bite;

      // Weight: the sub, equal-power against the mids so the total stays level.
      const float subGain = kSubMin + (kSubMax - kSubMin) * weight;
      const float midGain = kMidMax - (kMidMax - kMidMin) * weight;
      const float subThird = 0.3f + 0.7f * weight;  // more saturation of the sub = more upper harmonics

      // Drive: gain-compensated tanh with a little asymmetry, so it adds density rather than level.
      const float g = 1.0f + 9.0f * std::pow(drive, 1.35f);
      const float bias = 0.04f + 0.14f * drive;
      const float biasTanh = std::tanh(bias);
      const float comp = kDriveRef / std::max(std::tanh(g * kDriveRef + bias) - biasTanh, 0.05f);
      // The wobble's own level swing (open = louder) is evened out a bit.
      // Loudness compensation, measured (rms of a held default note while sweeping each macro alone), zero at
      // the defaults: Weight, Drive and Growl all add energy, and the macros should change colour, not level.
      const float trimDb = -4.5f * (weight - 0.6f) - 4.0f * (std::pow(drive, 1.3f) - kDriveTrimRef) + growlTrimDb(growl);
      const float levelGain = velGain * kLevel * std::exp2(trimDb * 0.16609640474f);
      const float hpA = 1.0f - std::exp(-static_cast<float>(kTwoPi * kSplitRatio * f0 / sr));

      for (int32_t j = 0; j < n; j++) {
        phase_ += inc;
        phase_ -= std::floor(phase_);
        phase2_ += inc * detune;
        phase2_ -= std::floor(phase2_);
        modPhase_ += modInc;
        modPhase_ -= std::floor(modPhase_);
        subPhase_ += inc;
        subPhase_ -= std::floor(subPhase_);
        fmIdx_ += fmStep;
        cutOct_ += cutStep;

        const float modSig = static_cast<float>(std::sin(kTwoPi * modPhase_));
        const double pm = static_cast<double>(fmIdx_ * modSig);
        const float saw = polyBlepSaw(phase_ + pm, inc);
        const float saw2 = polyBlepSaw(phase2_ + pm * 0.6, inc);
        const float pulse = 0.5f * (saw2 - polyBlepSaw(phase2_ + pm * 0.6 + 0.5, inc));
        const float src = kSrcLevel * (saw + 0.55f * pulse);

        const float cutoffHz = std::exp2(cutOct_);
        float body = filter_.process(src * (1.0f + 0.6f * growl), cutoffHz, resEff, 0.0f, 0) * kLadderComp;
        // The fundamental belongs to the sub: two one-pole high-passes keep the filtered saw from beating or
        // cancelling against it, so the low end stays put while the mids wobble.
        hp1_ += hpA * (body - hp1_);
        body -= hp1_;
        hp2_ += hpA * (body - hp2_);
        body -= hp2_;

        const float teeth = teethGain * fastTanh(foldK * teethBp(src));
        const float vowelSig = fGain * (formant1_.process(src) + 0.7f * formant2_.process(src));

        float mid = body + teeth + vowelSig;
        mid = (std::tanh(g * mid + bias) - biasTanh) * comp;
        float y = mid - dcX1_ + dcBlockR_ * dcY1_;
        y = std::clamp(y, -2.0f, 2.0f);
        dcX1_ = mid;
        dcY1_ = y;

        const float sine = static_cast<float>(std::sin(kTwoPi * subPhase_));
        const float sub = (1.0f - 0.2f * subThird) * sine + 0.2f * subThird * fastTanh(3.0f * sine);

        mono[i + j] = midGain * y + subGain * sub;
        const float e = ampEnv_.next();
        gains[i + j] = e * e * levelGain;
      }
      i += n;
    }

    for (int32_t j = lead; j < numFrames; j++) mono[j] = softClip(mono[j] * gains[j]);
    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr int32_t kSubBlock = 16;       // control-rate step (~0.33 ms at 48 kHz)
  static constexpr double kDcCornerHz = 5.0;
  static constexpr double kModRatio = 2.0017;    // FM modulator / carrier: harmonic, with a slow beat
  static constexpr float kSmoothSeconds = 0.012f;
  static constexpr float kZapSeconds = 0.09f;
  static constexpr float kSweepOct = 6.0f;
  static constexpr float kFollowLoOct = 8.2f;     // log2(~295 Hz): at or below it the extra paths sit at 30 %
  static constexpr float kFollowSpanOct = 3.0f;   // fully open 3 octaves above
  static constexpr float kZapOct = 2.2f;
  static constexpr float kFmMax = 1.4f;
  static constexpr float kSrcLevel = 0.85f;
  static constexpr float kLadderComp = 1.0f;
  static constexpr double kSplitRatio = 1.7;      // high-pass of the filtered path, x f0
  static constexpr float kTeethGain = 0.5f;
  static constexpr float kFormantGain = 0.7f;
  static constexpr float kSubMin = 0.12f, kSubMax = 0.52f;
  static constexpr float kMidMin = 1.0f, kMidMax = 0.8f;
  static constexpr float kDriveRef = 0.5f;
  static constexpr float kLevel = 0.85f;
  static constexpr float kDriveTrimRef = 0.3042f;  // 0.4^1.3: the default drive
  // Slew of the wobble signal per shape: smooth none, square-ish softened, ramp keeps a snap, random glides.
  static constexpr float kShapeSlewSeconds[4] = {0.0003f, 0.004f, 0.0015f, 0.006f};

  static int32_t idx(WobbleParam p) { return static_cast<int32_t>(p); }
  float value(WobbleParam p) const { return values_[idx(p)]; }

  // Growl adds FM and formant energy unevenly (a little growl thins the fundamental, a lot adds a lot):
  // piecewise-linear trim in dB through the measured points at 0, .25, .5 (default), .75, 1.
  static float growlTrimDb(float g) {
    static constexpr float kPts[5] = {1.1f, 2.0f, 0.0f, -1.9f, -2.9f};
    const float x = std::clamp(g, 0.0f, 1.0f) * 4.0f;
    const int32_t k = std::min(static_cast<int32_t>(x), 3);
    return kPts[k] + (kPts[k + 1] - kPts[k]) * (x - static_cast<float>(k));
  }

  // Unipolar wobble 0..1, 1 at the start of the cycle (a note on the beat starts open: punch).
  static float wobbleShape(int32_t shape, double cycles) {
    const double p = cycles - std::floor(cycles);
    switch (shape) {
      case 0: return 0.5f + 0.5f * static_cast<float>(std::cos(kTwoPi * p));
      case 1: return p < 0.5 ? 1.0f : 0.0f;
      case 2: return static_cast<float>(1.0 - p);
      default: {
        // Stepped random, two steps per cycle; the first step of each cycle is always open.
        const double step = std::floor(cycles * 2.0);
        const float r = 0.5f + 0.5f * lfoSampleHold(step);
        return (static_cast<int64_t>(step) & 1) == 0 && p < 0.5 ? std::max(r, 0.85f) : 0.12f + 0.88f * r;
      }
    }
  }

  AdsrParams ampParams() const {
    return AdsrParams{0.002f, 0.0f, 1.0f, value(WobbleParam::Release)};
  }
  static double semitonesOf(double ratio) { return 12.0 * std::log2(ratio); }

  void startGlide(double target) {
    targetSemis_ = target;
    const double frames = static_cast<double>(value(WobbleParam::Glide)) * core_.sampleRate;
    if (frames < 1.0 || target == pitchSemis_) {
      pitchSemis_ = target;
      glideStep_ = 0.0;
    } else {
      glideStep_ = (target - pitchSemis_) / frames;
    }
  }

  void clearState() {
    phase_ = 0.0;
    phase2_ = 0.37;
    modPhase_ = 0.0;
    subPhase_ = 0.25;  // starts at the sine's peak: punch from the first cycle
    dcX1_ = dcY1_ = 0.0f;
    hp1_ = hp2_ = 0.0f;
    filter_.reset();
    formant1_.reset();
    formant2_.reset();
    teeth_.reset();
    lfoU_ = 1.0f;
    zap_ = 1.0f;
    fmIdx_ = 0.0f;
    cutInit_ = false;
    snap_ = true;
    freeCycles_ = 0.0;
    pitchSemis_ = targetSemis_ = 0.0;
    glideStep_ = 0.0;
  }

  float teethBp(float x) { return teeth_.process(x); }

  static float fastTanh(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
  }

  // Saw in -1..1 with its edge at phase 0, PolyBLEP-corrected (`inc` = nominal phase step per sample).
  static float polyBlepSaw(double ph, double inc) {
    ph -= std::floor(ph);
    float saw = static_cast<float>(2.0 * ph - 1.0);
    if (ph < inc) {
      const double x = ph / inc;
      saw -= static_cast<float>(x + x - x * x - 1.0);
    } else if (ph > 1.0 - inc) {
      const double x = (ph - 1.0) / inc;
      saw -= static_cast<float>(x * x + x + x + 1.0);
    }
    return saw;
  }

  // Transparent below 0.6, then a smooth knee bounded by exactly 1.
  static float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.6f) return x;
    const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
    const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
    const float c = std::min(y, 1.0f);
    return x < 0.0f ? -c : c;
  }

  float values_[kWobbleParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_;
  MatrixFilter filter_;
  PercBandpass formant1_, formant2_, teeth_;
  bool tied_ = false;  // set by the engine just before noteOn (Instrument::setTie)

  // Transport (Instrument::syncTransport).
  double frame_ = 0.0, anchorFrame_ = 0.0, framesPerBeat_ = 24000.0;

  // Smoothed macros and wobble state.
  float sWobble_ = 0.0f, sGrowl_ = 0.0f, sBite_ = 0.0f, sWeight_ = 0.0f, sDrive_ = 0.0f, sCutOct_ = 9.0f,
        sRes_ = 0.0f;
  float lfoU_ = 1.0f, zap_ = 1.0f, fmIdx_ = 0.0f, cutOct_ = 9.0f;
  bool cutInit_ = false, snap_ = true;
  double freeCycles_ = 0.0;

  double phase_ = 0.0, phase2_ = 0.37, modPhase_ = 0.0, subPhase_ = 0.25;
  float dcX1_ = 0.0f, dcY1_ = 0.0f, dcBlockR_ = 0.999f, hp1_ = 0.0f, hp2_ = 0.0f;
  double pitchSemis_ = 0.0, targetSemis_ = 0.0, glideStep_ = 0.0;  // semitones above Tune
};

}  // namespace webdsp
