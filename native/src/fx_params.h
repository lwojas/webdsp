#pragma once
#include <cstdint>
#include "fx_chain_kinds.h"
#include "instrument.h"  // InstrumentParamInfo, ModDomain
#include "params.h"

// Self-description of every effect's parameters: the FX counterpart of an instrument's parameter
// table, in the same shape (InstrumentParamInfo) so one modulation layer (modulation.h) serves
// both. `id` is the NodeParam, `name` the stable string an application addresses it by. Ranges
// are the ones the effect enforces; `def` is the neutral/inert default.
//
// Modulation domain: inferred exactly as for synths (resolveModDomain). Parameters that cannot be
// moved continuously without artefacts are pinned to ModDomain::None: stepped controls (filter
// mode/slope, phaser stages, bitcrusher downsample, Dub Delay division, EQ band on/type/slope) and Delay time (the read pointer jumps, so a
// moving time would click; automate it as a base value instead).
namespace webdsp {

#define WD_NP(x) static_cast<int32_t>(NodeParam::x)
inline constexpr ModDomain kNoMod = ModDomain::None;

inline constexpr InstrumentParamInfo kFilterFxParams[] = {
    {WD_NP(FilterMode), "mode", 0.0f, 1.0f, 0.0f, "step", kNoMod},
    {WD_NP(FilterCutoff), "cutoff", 40.0f, 18000.0f, 18000.0f, "Hz"},
    {WD_NP(FilterResonance), "resonance", 0.1f, 20.0f, 0.707f, "Q"},
    {WD_NP(FilterSlope), "slope", 12.0f, 24.0f, 12.0f, "step", kNoMod},
};
inline constexpr InstrumentParamInfo kChorusFxParams[] = {
    {WD_NP(ChorusFlangerRate), "rate", 0.01f, 10.0f, 2.0f, "Hz"},
    {WD_NP(ChorusFlangerDepth), "depth", 0.0f, 20.0f, 5.0f, "ms"},
    {WD_NP(ChorusFlangerDelay), "delay", 0.1f, 40.0f, 15.0f, "ms"},
    {WD_NP(ChorusFlangerFeedback), "feedback", -0.95f, 0.95f, 0.0f, ""},
    {WD_NP(ChorusFlangerStereoPhase), "stereoPhase", 0.0f, 1.0f, 0.25f, ""},
    {WD_NP(ChorusFlangerMix), "mix", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr InstrumentParamInfo kDelayFxParams[] = {
    {WD_NP(DelayTime), "time", 0.02f, 1.5f, 0.25f, "s", kNoMod},
    {WD_NP(DelayFeedback), "feedback", 0.0f, 0.95f, 0.3f, ""},
    {WD_NP(DelayMix), "mix", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr InstrumentParamInfo kReverbFxParams[] = {
    {WD_NP(ReverbDecay), "decay", 0.0f, 0.99f, 0.5f, ""},
    {WD_NP(ReverbDamping), "damping", 0.0f, 0.99f, 0.2f, ""},
    {WD_NP(ReverbMix), "mix", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr InstrumentParamInfo kCompressorFxParams[] = {
    {WD_NP(CompThreshold), "threshold", -60.0f, 0.0f, -18.0f, "dB"},
    {WD_NP(CompRatio), "ratio", 1.0f, 20.0f, 4.0f, ""},
    {WD_NP(CompAttack), "attack", 0.0001f, 1.0f, 0.01f, "s"},
    {WD_NP(CompRelease), "release", 0.001f, 3.0f, 0.15f, "s"},
    {WD_NP(CompKnee), "knee", 0.0f, 24.0f, 6.0f, "dB"},
    {WD_NP(CompMakeup), "makeup", -24.0f, 24.0f, 0.0f, "dB"},
};
inline constexpr InstrumentParamInfo kSaturationFxParams[] = {
    {WD_NP(SatDrive), "drive", 0.0f, 40.0f, 0.0f, "dB"},
    {WD_NP(SatAsymmetry), "asymmetry", -1.0f, 1.0f, 0.0f, ""},
    {WD_NP(SatOutputGain), "outputGain", -24.0f, 24.0f, 0.0f, "dB"},
    {WD_NP(SatMix), "mix", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr InstrumentParamInfo kPhaserFxParams[] = {
    {WD_NP(PhaserRate), "rate", 0.01f, 10.0f, 0.5f, "Hz"},
    {WD_NP(PhaserDepth), "depth", 0.0f, 1.0f, 0.5f, ""},
    {WD_NP(PhaserCenter), "center", 100.0f, 5000.0f, 1000.0f, "Hz"},
    {WD_NP(PhaserFeedback), "feedback", -0.95f, 0.95f, 0.0f, ""},
    {WD_NP(PhaserStages), "stages", 2.0f, 12.0f, 4.0f, "step", kNoMod},
    {WD_NP(PhaserMix), "mix", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr InstrumentParamInfo kBitcrusherFxParams[] = {
    {WD_NP(CrushBits), "bits", 1.0f, 16.0f, 16.0f, ""},
    {WD_NP(CrushDownsample), "downsample", 1.0f, 64.0f, 1.0f, "step", kNoMod},
    {WD_NP(CrushMix), "mix", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr InstrumentParamInfo kDubDelayFxParams[] = {
    {WD_NP(DubDelayDivision), "division", 0.0f, 9.0f, 2.0f, "step", kNoMod},
    {WD_NP(DubDelayFeedback), "feedback", 0.0f, 0.95f, 0.45f, ""},
    {WD_NP(DubDelayTone), "tone", 200.0f, 16000.0f, 3000.0f, "Hz"},
    {WD_NP(DubDelayMix), "mix", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr InstrumentParamInfo kUtilityFxParams[] = {
    {WD_NP(UtilityGain), "gain", -60.0f, 12.0f, 0.0f, "dB"},
};
inline constexpr InstrumentParamInfo kEqFxParams[] = {
    {WD_NP(EqBand1On), "b1On", 0.0f, 1.0f, 0.0f, "step", kNoMod},
    {WD_NP(EqBand1Type), "b1Type", 0.0f, 5.0f, 1.0f, "step", kNoMod},
    {WD_NP(EqBand1Freq), "b1Freq", 20.0f, 20000.0f, 100.0f, "Hz"},
    {WD_NP(EqBand1Gain), "b1Gain", -18.0f, 18.0f, 0.0f, "dB"},
    {WD_NP(EqBand1Q), "b1Q", 0.1f, 18.0f, 0.707f, "Q"},
    {WD_NP(EqBand1ShelfSlope), "b1ShelfSlope", 0.1f, 1.0f, 1.0f, ""},
    {WD_NP(EqBand1Slope), "b1Slope", 12.0f, 24.0f, 12.0f, "step", kNoMod},
    {WD_NP(EqBand2On), "b2On", 0.0f, 1.0f, 0.0f, "step", kNoMod},
    {WD_NP(EqBand2Type), "b2Type", 0.0f, 5.0f, 0.0f, "step", kNoMod},
    {WD_NP(EqBand2Freq), "b2Freq", 20.0f, 20000.0f, 500.0f, "Hz"},
    {WD_NP(EqBand2Gain), "b2Gain", -18.0f, 18.0f, 0.0f, "dB"},
    {WD_NP(EqBand2Q), "b2Q", 0.1f, 18.0f, 0.707f, "Q"},
    {WD_NP(EqBand2ShelfSlope), "b2ShelfSlope", 0.1f, 1.0f, 1.0f, ""},
    {WD_NP(EqBand2Slope), "b2Slope", 12.0f, 24.0f, 12.0f, "step", kNoMod},
    {WD_NP(EqBand3On), "b3On", 0.0f, 1.0f, 0.0f, "step", kNoMod},
    {WD_NP(EqBand3Type), "b3Type", 0.0f, 5.0f, 0.0f, "step", kNoMod},
    {WD_NP(EqBand3Freq), "b3Freq", 20.0f, 20000.0f, 2000.0f, "Hz"},
    {WD_NP(EqBand3Gain), "b3Gain", -18.0f, 18.0f, 0.0f, "dB"},
    {WD_NP(EqBand3Q), "b3Q", 0.1f, 18.0f, 0.707f, "Q"},
    {WD_NP(EqBand3ShelfSlope), "b3ShelfSlope", 0.1f, 1.0f, 1.0f, ""},
    {WD_NP(EqBand3Slope), "b3Slope", 12.0f, 24.0f, 12.0f, "step", kNoMod},
    {WD_NP(EqBand4On), "b4On", 0.0f, 1.0f, 0.0f, "step", kNoMod},
    {WD_NP(EqBand4Type), "b4Type", 0.0f, 5.0f, 2.0f, "step", kNoMod},
    {WD_NP(EqBand4Freq), "b4Freq", 20.0f, 20000.0f, 8000.0f, "Hz"},
    {WD_NP(EqBand4Gain), "b4Gain", -18.0f, 18.0f, 0.0f, "dB"},
    {WD_NP(EqBand4Q), "b4Q", 0.1f, 18.0f, 0.707f, "Q"},
    {WD_NP(EqBand4ShelfSlope), "b4ShelfSlope", 0.1f, 1.0f, 1.0f, ""},
    {WD_NP(EqBand4Slope), "b4Slope", 12.0f, 24.0f, 12.0f, "step", kNoMod},
};
inline constexpr InstrumentParamInfo kGlitchFxParams[] = {
    {WD_NP(GlitchMix), "mix", 0.0f, 1.0f, 0.0f, ""},
    {WD_NP(GlitchMode), "mode", 0.0f, 2.0f, 0.0f, "step", kNoMod},
    {WD_NP(GlitchDivision), "division", 0.0f, 5.0f, 1.0f, "step", kNoMod},
    {WD_NP(GlitchSpeed), "speed", -1.0f, 2.0f, 1.0f, ""},
};
#undef WD_NP

#define WD_FXT(arr) \
  { arr, static_cast<int32_t>(sizeof(arr) / sizeof(arr[0])) }
struct FxParamTable {
  const InstrumentParamInfo* params;
  int32_t count;
};
// Indexed by FxKind.
inline constexpr FxParamTable kFxParamTables[] = {
    WD_FXT(kFilterFxParams),     WD_FXT(kChorusFxParams),     WD_FXT(kDelayFxParams),
    WD_FXT(kReverbFxParams),     WD_FXT(kCompressorFxParams), WD_FXT(kSaturationFxParams),
    WD_FXT(kPhaserFxParams),     WD_FXT(kBitcrusherFxParams), WD_FXT(kUtilityFxParams),
    WD_FXT(kDubDelayFxParams),   WD_FXT(kEqFxParams),         WD_FXT(kGlitchFxParams),
};
#undef WD_FXT

inline int32_t fxParamCount(int32_t kind) {
  return kind >= 0 && kind < kFxKindCount ? kFxParamTables[kind].count : 0;
}
inline const InstrumentParamInfo* fxParamInfo(int32_t kind, int32_t index) {
  return index >= 0 && index < fxParamCount(kind) ? &kFxParamTables[kind].params[index] : nullptr;
}

}  // namespace webdsp
