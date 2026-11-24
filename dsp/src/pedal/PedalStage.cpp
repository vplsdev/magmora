#include "pedal/PedalStage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace magmora::pedal {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kBypassSeconds = 0.012;
}  // namespace

void PedalStage::prepare(double internalRate, double hostRate, int cores, int oversample) {
  cores_ = std::clamp(cores, 1, 2);
  oversample_ = oversample == 2 || oversample == kMiddle2x ? oversample : 1;
  for (int c = 0; c < cores_; ++c) {
    if (const char* e = std::getenv("MAGMORA_PEDAL_BUDGET")) pedal_[c].budgetPerSample = std::atof(e);  // measurement only
    if (oversample_ == kMiddle2x) pedal_[c].prepare(internalRate, hostRate, 2);
    else pedal_[c].prepare(internalRate * oversample_, hostRate);
    if (oversample_ != 1) {
      up_[c].prepare(resample::chooseDesign(internalRate, false), true);
      down_[c].prepare(resample::chooseDesign(internalRate, false), true);
    }
  }
  step_ = 1.0 / std::max(1.0, kBypassSeconds * internalRate);
  on_ = false;
  fade_ = 0.0;
}

void PedalStage::setControls(bool on, double drive, double tone, double level) {
  starting_ = false;
  // From fully bypassed: start at the operating point of these settings. That is a DC solve on the audio thread, in
  // the block in which the pedal is switched on. Its cost is bounded: one rebuild of each cluster and, per cluster,
  // twelve continuation steps of a Newton solve with an iteration cap (PedalCluster::operatingPoint); measured at
  // 48 kHz with 64-sample blocks, the switch-on block took 664 us and the next 900 us of a 1333 us budget. It is not
  // precomputed on another thread because the operating point depends on the knobs at the moment of switching.
  if (on && !on_ && fade_ == 0.0) {
    for (int c = 0; c < cores_; ++c) {
      pedal_[c].setKnobs(drive, tone, level);
      resetCore(c);
    }
    // The pedal starts in the next block: the operating point and a block of the pedal's solves never share a block
    // (the worst block: switching on 3 V full-band noise at 96 kHz took the head's worst block past 90 %).
    starting_ = true;
  }
  on_ = on;
  for (int c = 0; c < cores_; ++c) pedal_[c].setKnobs(drive, tone, level);
}

void PedalStage::resetCore(int c) {
  pedal_[c].reset();
  if (oversample_ == kMiddle2x) {  // the 2x middle's resamplers carry the bias of p1 and o2: start them at it
    up_[c].reset(pedal_[c].dcP1(), 0.0);
    down_[c].reset(0.0, pedal_[c].dcO2());
  } else {
    up_[c].reset();
    down_[c].reset();
  }
}

void PedalStage::reset() {
  for (int c = 0; c < cores_; ++c) resetCore(c);
}

void PedalStage::process(double* const* lanes, int frames) {
  if (starting_) return;  // the block in which it was switched on: still bypassed
  for (int c = 0; c < cores_; ++c) {
    pedal_[c].budgetScale = fade_ >= 1.0 ? 1.0 : 0.75;
    pedal_[c].hostTick();
  }
  for (int k = 0; k < frames; ++k) {
    const double w = 0.5 - 0.5 * std::cos(kPi * fade_);
    for (int c = 0; c < cores_; ++c) {
      if (!lanes[c]) continue;
      const double dry = lanes[c][k];
      double wet;
      if (oversample_ == kMiddle2x) {
        Pedal& p = pedal_[c];
        double two[2];
        up_[c].up(p.front(dry), two);
        const double a = p.mid(two[0]);
        wet = p.back(down_[c].down(a, p.mid(two[1])));
      } else if (oversample_ == 2) {
        double two[2];
        up_[c].up(dry, two);
        const double a = pedal_[c].process(two[0]);
        wet = down_[c].down(a, pedal_[c].process(two[1]));
      } else {
        wet = pedal_[c].process(dry);
      }
      lanes[c][k] = fade_ >= 1.0 ? wet : dry + w * (wet - dry);
    }
    if (on_ ? fade_ < 1.0 : fade_ > 0.0) fade_ = std::clamp(fade_ + (on_ ? step_ : -step_), 0.0, 1.0);
  }
}

}  // namespace magmora::pedal
