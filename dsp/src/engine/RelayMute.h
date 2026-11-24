// The output mute around relay switching: 5 ms raised cosine down, switch, 2 ms closed, 5 ms up (12 ms in all).
// The closed part covers the tail of the down-sampler, which still holds sound from before the switch.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace magmora::engine {

class RelayMute {
 public:
  void prepare(double sampleRate) {  // allocates
    ramp_ = std::max(1, static_cast<int>(std::lround(kRampSeconds * sampleRate)));
    holdSamples_ = std::max(1, static_cast<int>(std::lround(kHoldSeconds * sampleRate)));
    gain_.resize(static_cast<size_t>(ramp_) + 1);
    for (int k = 0; k <= ramp_; ++k) gain_[static_cast<size_t>(k)] = 0.5 - 0.5 * std::cos(kPi * k / ramp_);
    gain_.back() = 1.0;
  }

  void snap(bool muted) {
    position_ = muted ? 0 : ramp_;
    hold_ = 0;
  }

  void close() {  // at once, and stay closed for the hold time
    position_ = 0;
    hold_ = holdSamples_;
  }

  bool closed() const { return position_ == 0; }

  double next(bool muted) {
    if (hold_ > 0) {
      --hold_;
      muted = true;
    }
    if (muted && position_ > 0) --position_;
    else if (!muted && position_ < ramp_) ++position_;
    return gain_[static_cast<size_t>(position_)];
  }

 private:
  static constexpr double kRampSeconds = 0.005, kHoldSeconds = 0.002;
  static constexpr double kPi = 3.14159265358979323846;

  int ramp_ = 1, holdSamples_ = 1, position_ = 0, hold_ = 0;
  std::vector<double> gain_;
};

}  // namespace magmora::engine
