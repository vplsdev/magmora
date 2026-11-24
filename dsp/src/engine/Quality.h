// How fast the whole chain runs inside for each quality.
#pragma once
#include <array>

#include "resample/Oversampler.h"

namespace magmora::engine {

// Oversampling factor by host-rate band: below 86 kHz (44.1, 48), below 172 kHz (88.2, 96), above (176.4, 192).
// Standard: 264.6 to 288 kHz inside. High: 352.8 to 384 kHz, except at 176.4 / 192 kHz, where 2x (352.8 / 384 kHz)
// cannot be held under one core with 64-sample blocks on the review's stress (worst block 114 % measured, and a
// solver budget small enough to bound it would also cut into normal playing), so High runs as Standard there.
// Ultra: 705.6 to 768 kHz everywhere, offline only.
struct QualityPolicy {
  int factor[3];
  bool realtime;  // false: offline rendering only
};

constexpr std::array<QualityPolicy, 3> kQualities = {{
    {{6, 3, 1}, true},
    {{8, 4, 1}, true},
    {{16, 8, 4}, false},
}};

inline int oversamplingFactor(size_t quality, double hostRate) {
  const int band = hostRate < 86000.0 ? 0 : (hostRate < 172000.0 ? 1 : 2);
  return kQualities[quality].factor[band];
}

}  // namespace magmora::engine
