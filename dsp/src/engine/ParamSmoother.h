// Slews a control in its own units (knob rotation, dB), once per host sample.
#pragma once
#include <cmath>

namespace magmora::engine {

// 30 ms is the slew. The two 3 ms poles behind it round off the corners that block-rate automation
// leaves: with the 30 ms pole alone, or with one 5 ms pole behind it, those corners put lines at multiples of the
// block rate above the -80 dB of gate G5 (measured: -74 dB with one).
constexpr double kSlewSeconds = 0.030;
constexpr double kRoundingSeconds = 0.003;

class ParamSmoother {
 public:
  // rounding and slew: the head's knobs use the defaults; the EQ its own (eq/Eq.h, kSlewEqSeconds)
  void prepare(double sampleRate, double rounding = kRoundingSeconds, double slew = kSlewSeconds) {
    slew_ = 1.0 - std::exp(-1.0 / (slew * sampleRate));
    rounding_ = 1.0 - std::exp(-1.0 / (rounding * sampleRate));
  }

  void snap(double value) {
    target_ = first_ = second_ = value_ = value;
    moving_ = false;
  }

  void setTarget(double value) {
    if (value == target_) return;
    target_ = value;
    moving_ = true;
  }

  bool moving() const { return moving_; }
  double value() const { return value_; }

  double next() {
    if (moving_) {
      first_ += slew_ * (target_ - first_);
      second_ += rounding_ * (first_ - second_);
      value_ += rounding_ * (second_ - value_);
      if (std::fabs(target_ - first_) + std::fabs(target_ - second_) + std::fabs(target_ - value_) < kSettled) snap(target_);
    }
    return value_;
  }

 private:
  static constexpr double kSettled = 1e-6;

  double slew_ = 1.0, rounding_ = 1.0;
  double target_ = 0.0, first_ = 0.0, second_ = 0.0, value_ = 0.0;
  bool moving_ = false;
};

}  // namespace magmora::engine
