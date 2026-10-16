#pragma once
#include <memory>
#include "instrument.h"
#include "instruments/bass.h"
#include "instruments/clap.h"
#include "instruments/hihat.h"
#include "instruments/kick.h"
#include "instruments/pulse.h"
#include "instruments/snare.h"
#include "instruments/tom.h"

// The one place that knows which instrument kinds exist. Adding a machine = a new header under
// instruments/, a new InstrumentKind value, and a case in each function here.
namespace webdsp {

// Identity of a kind, independent of any instance.
//   id       — stable machine identifier; persist/refer to this, never `name`. Never changes.
//   name     — human-facing display text; free to change.
//   category — coarse grouping a UI may filter on ("drum" today; e.g. "synth", "sample" later).
//              Plain string on purpose: no taxonomy is committed to yet.
struct InstrumentKindMeta {
  const char* id;
  const char* name;
  const char* category;
};

inline std::unique_ptr<Instrument> makeInstrument(int32_t kind) {
  switch (static_cast<InstrumentKind>(kind)) {
    case InstrumentKind::Kick: return std::make_unique<KickInstrument>();
    case InstrumentKind::Snare: return std::make_unique<SnareInstrument>();
    case InstrumentKind::Clap: return std::make_unique<ClapInstrument>();
    case InstrumentKind::Tom: return std::make_unique<TomInstrument>();
    case InstrumentKind::HiHat: return std::make_unique<HiHatInstrument>();
    case InstrumentKind::Bass: return std::make_unique<BassInstrument>();
    case InstrumentKind::Pulse: return std::make_unique<PulseInstrument>();
  }
  return nullptr;
}

// Null for an unknown kind.
inline const InstrumentKindMeta* instrumentKindMeta(int32_t kind) {
  static constexpr InstrumentKindMeta kMeta[] = {
      {"kick", "Kick", "drum"},
      {"snare", "Snare", "drum"},
      {"clap", "Clap", "drum"},
      {"tom", "Tom", "drum"},
      {"hihat", "Hi-Hat", "drum"},
      {"bass", "Bass", "bass"},
      {"pulse", "Pulse", "synth"},
  };
  if (kind < 0 || kind >= static_cast<int32_t>(sizeof(kMeta) / sizeof(kMeta[0]))) return nullptr;
  return &kMeta[kind];
}

// Static parameter metadata, queryable without an instance. Count is 0 for an unknown kind.
inline int32_t instrumentParamCount(int32_t kind) {
  switch (static_cast<InstrumentKind>(kind)) {
    case InstrumentKind::Kick: return kKickParamCount;
    case InstrumentKind::Snare: return kSnareParamCount;
    case InstrumentKind::Clap: return kClapParamCount;
    case InstrumentKind::Tom: return kTomParamCount;
    case InstrumentKind::HiHat: return kHiHatParamCount;
    case InstrumentKind::Bass: return kBassParamCount;
    case InstrumentKind::Pulse: return kPulseParamCount;
  }
  return 0;
}

inline const InstrumentParamInfo* instrumentParamInfo(int32_t kind, int32_t index) {
  if (index < 0 || index >= instrumentParamCount(kind)) return nullptr;
  switch (static_cast<InstrumentKind>(kind)) {
    case InstrumentKind::Kick: return &kKickParams[index];
    case InstrumentKind::Snare: return &kSnareParams[index];
    case InstrumentKind::Clap: return &kClapParams[index];
    case InstrumentKind::Tom: return &kTomParams[index];
    case InstrumentKind::HiHat: return &kHiHatParams[index];
    case InstrumentKind::Bass: return &kBassParams[index];
    case InstrumentKind::Pulse: return &kPulseParams[index];
  }
  return nullptr;
}

}  // namespace webdsp
