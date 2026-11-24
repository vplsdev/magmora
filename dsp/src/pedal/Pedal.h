// The overdrive pedal for one amp core (XP2; docs/research/PEDAL_SPEC.md): four clusters solved in order within
// each internal sample, no delay between them; O2 and O3 run a cheap form (their op-amps linear) while its guard
// holds and the full form otherwise, on the same states (spec 8). Newton as the head's, with its bounded-work
// recovery. prepare() allocates; everything else runs on the audio thread without allocation.
#pragma once
#include <array>

#include "engine/ParamSmoother.h"
#include "pedal/PedalCluster.h"

namespace magmora::pedal {

const char* modelHash();

struct PedalStats {
  long samples = 0, fullO2 = 0, fullO3 = 0, iterations = 0, recoveries = 0, fallbacks = 0;
  long clusterIterations[4] = {};  // O1 .. O4, cheap and full forms together, recoveries included
  long midSamples = 0;             // samples of O2 and O3 (twice `samples` with a 2x middle)
  long charged = 0;                // iterations charged to the work budget (Newton solves of clusters with devices)
};

// Resistances of the three pots' sections at panel positions 0..10 (spec 4), in PedalElement::knob order.
void potOhms(double drive, double tone, double level, double* ohms);

class Pedal {
 public:
  // internalRate: the rate of O1 and O4, and of process() / front() / back(); midFactor 1 or 2: O2 and O3 (the clipping
  // amplifier and the tone stage, where the aliasing is made) at that multiple of it, through mid().
  void prepare(double internalRate, double hostRate, int midFactor = 1);
  // Host-rate targets (panel positions 0..10).
  void setKnobs(double drive, double tone, double level);
  // To the operating point at the target positions (smoothing snapped).
  void reset();
  // Once per host sample: knob smoothing and, while a knob moves, a rebuild every 32 host samples.
  void hostTick();
  double process(double x);  // one internal sample: volts at the input jack in, volts at the output jack out
  // The same in three parts, for a middle at another rate: process(x) == back(mid(front(x))) with midFactor 1.
  // front: one internal sample of O1 (and the work budget's refill); mid: one sample of O2 and O3 at the middle's
  // rate; back: one internal sample of O4.
  double front(double x);
  double mid(double p1);
  double back(double o2);
  const PedalStats& stats() const { return stats_; }
  // The operating point's p1 (O1's output, O2's input) and o2 (O3's output, O4's input), as of the last reset().
  double dcP1() const { return dcP1_; }
  double dcO2() const { return dcO2_; }
  // Probes of the last sample: p1, e1, o1, o2, out, e2.
  double probe(int k) const { return probe_[k]; }
  // Measured on 3 V full-band noise at 288 kHz against the prototype (pedal.SplitSim, 1e-7 V): tolerance 1e-4 V
  // -59.8 dB, 1e-5 V -79.5 dB, 1e-6 V -97.8 dB; typical playing 2.5, 2.7, 3.2 % of a core.
  NewtonSettings settings{1e-6, 60, true, true};
  bool cheap = true;  // false: the full forms only (for the comparison with the prototype's SplitSim)
  // The bounded-work rule with the pedal's own figures, in Newton iterations (all of them, every cluster and form),
  // refilled per front() call (per internal sample with the 2x middle): playing uses about 7 there and G4's hardest
  // tone (0.5 V at 10 kHz, drive 10) about 17, so the refill is 18; the credit is a short 32 samples' worth, so that a
  // block's work is at most (N + 32) x 18 iterations plus one failed solve's recovery. While it is spent a cluster holds
  // its last solution at no cost.
  double budgetPerSample = 16.0, budgetCeilingSamples = 32.0, budgetCeiling = 0.0;
  // Set by the stage: 0.75 while its 12 ms bypass crossfade runs, when the head behind it still sees the unclipped
  // input (switching on during 3 V full-band noise at 96 kHz: worst block 92 % at the full refill).
  double budgetScale = 1.0;

 private:
  void rebuild(unsigned which, bool refined);
  bool solveCluster(PedalCluster& c, double u, double* s, const double* seed = nullptr);
  int counting_ = 0;  // the cluster whose iterations solveCluster is counting

  PedalCluster o1_, o2_, o3_, o4_, o2c_, o3c_;
  std::array<double, 8> s1_{}, s2_{}, s3_{}, s4_{};
  double target_[3] = {5, 5, 5}, rot_[3] = {5, 5, 5};
  engine::ParamSmoother knob_[3];
  bool moving_ = false, o2Full_ = false, o3Full_ = false, held_ = false;
  int tick_ = 0;
  unsigned dirty_ = 0, blending_ = 0;
  double tokens_ = 0.0;
  PedalStats stats_;
  double probe_[6] = {};
  double dcP1_ = 0.0, dcO2_ = 0.0;
};

}  // namespace magmora::pedal
