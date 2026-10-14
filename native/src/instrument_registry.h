#pragma once
#include <memory>
#include "instrument.h"
#include "instruments/kick.h"

// The one place that knows which instrument kinds exist. Adding a machine = a new header under
// instruments/, a new InstrumentKind value, and a case in each function here.
namespace webdsp {

inline std::unique_ptr<Instrument> makeInstrument(int32_t kind) {
  switch (static_cast<InstrumentKind>(kind)) {
    case InstrumentKind::Kick:
      return std::make_unique<KickInstrument>();
  }
  return nullptr;
}

// Static metadata, queryable without an instance. Count is 0 for an unknown kind.
inline int32_t instrumentParamCount(int32_t kind) {
  return kind == static_cast<int32_t>(InstrumentKind::Kick) ? kKickParamCount : 0;
}

inline const InstrumentParamInfo* instrumentParamInfo(int32_t kind, int32_t index) {
  if (index < 0 || index >= instrumentParamCount(kind)) return nullptr;
  return &kKickParams[index];
}

}  // namespace webdsp
