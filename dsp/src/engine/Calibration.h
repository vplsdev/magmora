// Digital full scale to volts and back, and dB.
#pragma once
#include <cmath>

namespace magmora::engine {

constexpr double kInputClamp = 16.0;   // full scale; keeps absurd host samples from costing a solver fallback
constexpr double kOutputClamp = 4.0;   // last line of defence: +12 dBFS
constexpr double kSagAtFullScale = 0.15;  // Meters::supplySag = 1

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace magmora::engine
