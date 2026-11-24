// One cluster of the pedal (XP2; docs/research/PEDAL_SPEC.md 5, 6): nodal K-method with trapezoidal companions and
// scalar device ports, exactly as the prototype's system2 / run2 / newton2 (proto/blockamp/pedal.py). Its states
// live outside, so the cheap and the full form of a cluster (spec 8) share them.
#pragma once
#include <vector>

namespace magmora::pedal {

struct PedalElement {
  char kind;  // R, C, V, I, G (a voltage-controlled current source)
  int a, b, cp, cn;
  double value;
  int knob;     // 0, or the pot section it is (1 drive, 2 tone a, 3 tone b, 4 level a, 5 level b)
  int prewarp;  // capacitor scaled to place its reactance exactly at kPedalPrewarpHz
  int input;    // the cluster's input source
};
struct PedalDevice {
  int kind;  // 0 diode pair, 1 tanh, 2 clamp, 3 junction (transistor part)
  int ctrlNode[3];
  double ctrlCoef[3];
  int injNode[2];
  double injCoef[2];
  double par[4];
};
struct PedalOutput {
  const char* name;
  int plus, minus;
};
struct PedalClusterDef {
  const char* name;
  int nodes;
  const PedalElement* elements;
  int nElements;
  const PedalDevice* devices;
  int nDevices;
  const PedalOutput* outputs;
  int nOutputs;
};

constexpr int kMaxPorts = 3;

// Current and conductance of a scalar device (pedal.dev_eval, exponentials continued as lines above 40).
void evaluate(int kind, const double* par, double u, double& i, double& g);

struct NewtonSettings {
  double tolerance = 1e-4;
  int maxIterations = 60;
  bool fastAccept = true;  // on convergence, the currents follow the last step to first order (no new evaluation)
  // Step control. false: the prototype's (newton2: the whole step scaled so no port moves more than its limit).
  // true: each junction's step into conduction is damped logarithmically (SPICE's pnjlim) and steps out of it are
  // free; the same solution, in far fewer iterations when a junction swings between cut-off and conduction.
  bool junctionLimiting = true;
};

class PedalCluster {
 public:
  // Allocates (main thread).
  void prepare(const PedalClusterDef& def, double rate);
  // Assembles and solves for the knob resistances (6 values, index = PedalElement::knob - 1). refined: one step of
  // iterative refinement with a double-length residual (the companion conductances span nine decades). No allocation.
  void build(const double* knobOhms, bool refined);
  // DC solution for an input source value: sets the outputs `y`, the ports `v` and the states s (size states()).
  bool operatingPoint(double input, double* s);
  // One sample: input u, states s (read). Newton from the prediction 2 v[n-1] - v[n-2], or from `seed` if given.
  // Returns iterations, negative on reaching the cap. Leaves the outputs in y() and the currents for commit().
  int solve(double u, const double* s, const NewtonSettings& settings, const double* seed = nullptr);
  // The recovery of the head: back to the last accepted ports, walk p from its last value in 8
  // steps. Returns false if that fails too.
  bool recover(const NewtonSettings& settings, int& iterations, int steps = 8);
  // Keep the last accepted solution, or accept the last iterate with its currents (fallbacks).
  void keepLast();
  void hold(double u, const double* s) {  // keepLast for this sample's input and states
    uLast_ = u;
    sLast_ = s;
    keepLast();
  }
  void acceptIterate();
  // Accept this sample: outputs from the currents, states advanced into s.
  void commit(double u, double* s);

  // Coefficients moving between two builds (a knob in motion): from() keeps what is in force, to() what build() made;
  // blend(t) puts from + t (to - from) in force. No allocation.
  void captureFrom() { pack(from_.data()); }
  void captureTo() { pack(to_.data()); }
  void blend(double t);

  int ports() const { return n_; }
  int states() const { return ns_; }
  int outputs() const { return ny_; }
  const double* y() const { return y_.data(); }
  const double* v() const { return v_; }
  void setPorts(const double* v) {
    for (int a = 0; a < n_; ++a) v_[a] = vPrev_[a] = vLast_[a] = v[a];
  }
  double outputDc(int k) const { return y_[static_cast<size_t>(k)]; }

 private:
  int newton(const double* p, const NewtonSettings& settings);
  template <int N>
  int newtonFixed(const double* p, const NewtonSettings& settings);
  void assemble(double* G, bool dc) const;
  void refreshOutputs();
  void pack(double* out) const;
  std::vector<double>* sets_[12] = {};
  void evaluatePorts(double* g);
  template <int N, int NS, int NY>
  int solveFixed(double u, const double* s, const NewtonSettings& settings, const double* seed);
  template <int N, int NS>
  void commitFixed(double u, double* s);

  const PedalClusterDef* def_ = nullptr;
  double rate_ = 0.0;
  int n_ = 0, ns_ = 0, ny_ = 0, nu_ = 0, nx_ = 0, in_ = -1;
  std::vector<int> extra_, sourceOf_;  // branch unknown of each V element; source column of each V / I element
  std::vector<double> knobs_, u0_;     // pot resistances in force; source values (DC)
  std::vector<double> gs_;             // companion conductance of each state
  std::vector<double> G_, lu_, rhs_, X_, corr_, work_;
  std::vector<int> pivot_;
  // per-sample matrices (row-major) and folded constants
  std::vector<double> K_, Pin_, P0_, Ps_, Yin_, Y0_, Ys_, Yi_, Sin_, S0_, Ss_, Si_;
  std::vector<double> y_, dcKeep_, dcX_, from_, to_;
  double v_[kMaxPorts] = {}, vPrev_[kMaxPorts] = {}, vLast_[kMaxPorts] = {}, pLast_[kMaxPorts] = {}, p_[kMaxPorts] = {};
  double cur_[kMaxPorts] = {}, curLast_[kMaxPorts] = {};
  int kind_[kMaxPorts] = {};
  bool transistor_ = false;
  double uLast_ = 0.0;
  const double* sLast_ = nullptr;
  const double* par_[kMaxPorts] = {};
};

}  // namespace magmora::pedal
