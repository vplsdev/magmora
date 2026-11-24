// Small signal helpers for the engine tests.
#pragma once
#include <magmora/Engine.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace magmora::test {

constexpr double kPi = 3.14159265358979323846;

inline std::vector<double> sine(double rate, double hz, double amplitude, int samples) {
  std::vector<double> x(static_cast<size_t>(samples));
  for (int n = 0; n < samples; ++n) x[static_cast<size_t>(n)] = amplitude * std::sin(2.0 * kPi * hz * n / rate);
  return x;
}

inline std::vector<double> run(Engine& e, const std::vector<double>& x, int block = 64) {
  std::vector<double> y(x.size());
  for (size_t at = 0; at < x.size(); at += static_cast<size_t>(block)) {
    const double* in[1] = {x.data() + at};
    double* out[1] = {y.data() + at};
    e.process(in, out, static_cast<int>(std::min(x.size() - at, static_cast<size_t>(block))));
  }
  return y;
}

inline double peak(const std::vector<double>& y, size_t from = 0, size_t to = static_cast<size_t>(-1)) {
  double big = 0.0;
  for (size_t n = from; n < std::min(to, y.size()); ++n) big = std::max(big, std::fabs(y[n]));
  return big;
}

inline double db(double x) { return 20.0 * std::log10(x + 1e-30); }

}  // namespace magmora::test
