// A simple input gate: instant-attack peak detector, hysteresis, hold, and a twice-smoothed gain (no corners).
#pragma once
#include <algorithm>
#include <cmath>

#include "engine/Calibration.h"

namespace magmora::engine {

class Gate {
 public:
  void prepare(double sampleRate) {
    fall_ = std::exp(-1.0 / (kDetectorSeconds * sampleRate));
    attack_ = 1.0 - std::exp(-1.0 / (kAttackSeconds * sampleRate));
    release_ = 1.0 - std::exp(-1.0 / (kReleaseSeconds * sampleRate));
    holdSamples_ = static_cast<int>(kHoldSeconds * sampleRate);
  }

  void setThreshold(double db, double offAtOrBelow) {
    if (db == db_) return;
    db_ = db;
    enabled_ = db > offAtOrBelow;
    opens_ = dbToGain(db);
    closes_ = opens_ * kHysteresis;
  }

  void reset() {
    envelope_ = 0.0;
    hold_ = 0;
    open_ = !enabled_;
    gain_ = first_ = open_ ? 1.0 : 0.0;
  }

  double gain() const { return gain_; }

  double process(double x) {
    envelope_ = std::max(std::fabs(x), envelope_ * fall_);
    if (!enabled_) {
      open_ = true;
    } else if (envelope_ >= (open_ ? closes_ : opens_)) {
      open_ = true;
      hold_ = holdSamples_;
    } else if (hold_ > 0) {
      --hold_;
    } else {
      open_ = false;
    }
    const double target = open_ ? 1.0 : 0.0;
    first_ += (open_ ? attack_ : release_) * (target - first_);
    gain_ += attack_ * (first_ - gain_);
    if (std::fabs(target - first_) + std::fabs(target - gain_) < kSettled) gain_ = first_ = target;
    return x * gain_;
  }

 private:
  static constexpr double kDetectorSeconds = 0.020, kAttackSeconds = 0.001, kReleaseSeconds = 0.060;
  static constexpr double kHoldSeconds = 0.050, kHysteresis = 0.5, kSettled = 1e-8;

  double fall_ = 0.0, attack_ = 1.0, release_ = 1.0;
  int holdSamples_ = 0, hold_ = 0;
  double db_ = -1e9, opens_ = 0.0, closes_ = 0.0;
  double envelope_ = 0.0, gain_ = 1.0, first_ = 1.0;
  bool enabled_ = false, open_ = true;
};

}  // namespace magmora::engine
