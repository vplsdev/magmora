#include "resample/Oversampler.h"

namespace magmora::resample {

int factorFor(double hostRate, double targetRate) {
  int best = kMaxFactor;
  for (int base : {1, 3})
    for (int f = base; f <= kMaxFactor; f *= 2)
      if (hostRate * f >= targetRate * (1.0 - 1e-6) && f < best) best = f;
  return best;
}

// 2x stages until the rate is high enough for the 3x design, then the 3x stage, then the remaining 2x stages.
void Oversampler::prepare(double hostRate, int factor, bool minimumPhase) {
  stages_ = 0;
  factor_ = 1;
  double rate = hostRate;
  int remaining = factor;
  while (remaining > 1 && stages_ < kMaxStages) {
    Stage& stage = stage_[static_cast<size_t>(stages_)];
    const bool third = remaining % 3 == 0 && (rate >= chooseThirdDesign(rate).lowerRate || remaining % 2 != 0);
    stage.factor = third ? 3 : 2;
    if (third) stage.third.prepare(chooseThirdDesign(rate), minimumPhase);
    else stage.half.prepare(chooseDesign(rate, stages_ == 0), minimumPhase);
    rate *= stage.factor;
    factor_ *= stage.factor;
    remaining /= stage.factor;
    ++stages_;
  }
  latency_ = 0;  // seen from the innermost stage outwards
  for (int s = stages_ - 1; s >= 0; --s) {
    Stage& stage = stage_[static_cast<size_t>(s)];
    stage.padding = latency_ % stage.factor == 0 ? 0 : stage.factor - latency_ % stage.factor;
    latency_ += stage.padding;
    latency_ = (stage.factor == 3 ? stage.third.delay() : stage.half.delay()) + latency_ / stage.factor;
  }
  reset();
}

void Oversampler::reset() {
  for (int s = 0; s < stages_; ++s) {
    Stage& stage = stage_[static_cast<size_t>(s)];
    stage.half.reset();
    stage.third.reset();
    stage.held[0] = stage.held[1] = 0.0;
  }
}

void Oversampler::up(double x, double* out) {
  double lower[kMaxFactor];
  out[0] = x;
  int count = 1;
  for (int s = 0; s < stages_; ++s) {
    Stage& stage = stage_[static_cast<size_t>(s)];
    for (int k = 0; k < count; ++k) lower[k] = out[k];
    for (int k = 0; k < count; ++k) {
      if (stage.factor == 3) stage.third.up(lower[k], out + 3 * k);
      else stage.half.up(lower[k], out + 2 * k);
    }
    count *= stage.factor;
  }
}

double Oversampler::down(double* in) {
  int count = factor_;
  for (int s = stages_ - 1; s >= 0; --s) {
    Stage& stage = stage_[static_cast<size_t>(s)];
    for (int p = 0; p < stage.padding; ++p) {  // delay the upper-rate stream by `padding` samples
      for (int k = 0; k < count; ++k) {
        const double newest = in[k];
        in[k] = stage.held[p];
        stage.held[p] = newest;
      }
    }
    count /= stage.factor;
    for (int k = 0; k < count; ++k)
      in[k] = stage.factor == 3 ? stage.third.down(in + 3 * k) : stage.half.down(in[2 * k], in[2 * k + 1]);
  }
  return in[0];
}

}  // namespace magmora::resample
