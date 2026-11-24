// The committed bench signals: deterministic, so every machine times the same samples.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace magmora::tools {

constexpr double kBenchPi = 3.14159265358979323846;

// Palm-muted eighths on E2 for half of it, then a ringing power chord: levels and spectrum of a boosted guitar DI.
inline std::vector<float> guitarLike(double rate, double seconds) {
  std::vector<float> out(static_cast<size_t>(rate * seconds));
  static const double chord[] = {82.41, 123.47, 164.81, 246.94};
  for (size_t n = 0; n < out.size(); ++n) {
    const double t = static_cast<double>(n) / rate;
    double x = 0.0;
    if (t < 0.5 * seconds) {
      const double since = std::fmod(t, 0.125);
      for (int h = 1; h <= 18; ++h)
        x += std::sin(2.0 * kBenchPi * 82.41 * h * since + 0.3 * h * h) * std::exp(-since * (18.0 + 4.0 * h)) / h;
      x *= std::fmin(1.0, since / 0.001);
    } else {
      const double since = t - 0.5 * seconds;
      for (double f : chord)
        for (int h = 1; h <= 10; ++h)
          x += 0.45 * std::sin(2.0 * kBenchPi * f * h * since + 0.7 * h) * std::exp(-since * (0.8 + 0.5 * h)) / h;
      x *= std::fmin(1.0, since / 0.004);
    }
    out[n] = static_cast<float>(0.17 * x);
  }
  return out;
}

inline std::vector<float> fullScaleNoise(double rate, double seconds) {
  std::vector<float> out(static_cast<size_t>(rate * seconds));
  uint64_t state = 0x9e3779b97f4a7c15ull;
  for (float& v : out) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    v = static_cast<float>(static_cast<double>(state >> 11) / 9007199254740992.0 * 2.0 - 1.0);
  }
  return out;
}

}  // namespace magmora::tools
