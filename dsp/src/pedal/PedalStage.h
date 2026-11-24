// The pedal as the engine's pre-cluster stage (XP2): runs in the head's oversampled loop ahead of cluster A, one
// Pedal per amp core. Bypass is a 12 ms crossfade (as the head's relays); fully bypassed, the stage reports itself
// inactive and the engine does not call it, so the head is bit-identical (G-X1).
#pragma once
#include <magmora/Engine.h>

#include "pedal/Pedal.h"
#include "resample/HalfbandFIR.h"

namespace magmora::pedal {

class PedalStage final : public engine::InternalStage {
 public:
  // main thread; allocates. oversample: 1, or 2 to run the pedal at twice the head's internal rate between a
  // minimum-phase half-band up and down stage of its own (aliasing of a hard-clipped tone), or kMiddle2x to run
  // only O2 and O3 (the clipping amplifier and the tone stage) at twice the rate, O1 and O4 at the head's.
  static constexpr int kMiddle2x = 3;
  void prepare(double internalRate, double hostRate, int cores, int oversample = 1);
  void setControls(bool on, double drive, double tone, double level);  // audio thread, once per block
  bool active() const override { return on_ || fade_ > 0.0; }
  void process(double* const* lanes, int frames) override;  // one host sample's worth of internal samples
  void reset();
  const Pedal& pedal(int core) const { return pedal_[core]; }
  Pedal& pedal(int core) { return pedal_[core]; }  // tools: settings that prepare() reads are set before it
  long fallbacks() const override { return pedal_[0].stats().fallbacks + (cores_ == 2 ? pedal_[1].stats().fallbacks : 0); }
  int oversample() const { return oversample_; }

 private:
  void resetCore(int c);

  Pedal pedal_[2];
  int cores_ = 1, oversample_ = 1;
  resample::HalfbandFIR up_[2], down_[2];
  bool on_ = false;
  bool starting_ = false;  // this block: the operating point was solved; the pedal starts in the next block
  double fade_ = 0.0, step_ = 0.0;
};

}  // namespace magmora::pedal
