// Cascade of half-band stages between the host rate and the internal rate.
#pragma once
#include <array>

#include "resample/HalfbandFIR.h"

namespace magmora::resample {

constexpr int kMaxStages = 4;
constexpr int kMaxFactor = 16;

// The smallest factor 2^k or 3 * 2^k (at most kMaxFactor) that brings `hostRate` to at least `targetRate`.
int factorFor(double hostRate, double targetRate);

class Oversampler {
 public:
  void prepare(double hostRate, int factor, bool minimumPhase);  // allocates
  void reset();

  // out receives factor() samples.
  void up(double x, double* out);
  // Reads factor() samples; may overwrite them.
  double down(double* in);

  int factor() const { return factor_; }
  // Round trip in host samples: exact in linear phase, zero (as reported) in minimum phase.
  int latency() const { return latency_; }

 private:
  struct Stage {
    int factor = 2;  // 2: half-band, 3: third-band
    HalfbandFIR half;
    ThirdbandFIR third;
    int padding = 0;  // samples of delay at the upper rate, which keep the total a whole number
    double held[2] = {0.0, 0.0};
  };

  std::array<Stage, kMaxStages> stage_;
  int stages_ = 0, factor_ = 1;
  int latency_ = 0;
};

}  // namespace magmora::resample
