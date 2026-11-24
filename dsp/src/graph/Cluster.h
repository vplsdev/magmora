// The per-sample kernel of one cluster (recipe: bench/kernel_fast.cpp).
//
// A step is split so that the out-of-order core can work ahead of the serial chain through the Newton solves: whatever
// needs only the state (linear part, predictor, first device evaluation, factorisation) is computed for the next
// sample right behind this cluster's solve.
#pragma once
#include <memory>
#include <vector>

#include "common/MatVec.h"
#include "solver/ClusterSystem.h"
#include "solver/OperatingPoint.h"

namespace magmora::graph {

enum class Laws { Table, Closed };

enum class Fallback {
  Adaptive,   // continuation in the excitation with step-size control (bench recipe 7)
  Prototype,  // retry from the previous solution, then eight equal steps (dk.py)
};

struct SolverSettings {
  double tolerance;
  int maxIterations;
  bool fastAccept;
  Fallback fallback;
};

// What failed solves may cost the audio thread, in device evaluations: a bucket shared by every cluster of one graph,
// filled a little every sample. While it holds anything a failed solve gets the full recovery (a retry, then a walk to the new input); when a
// storm of failures has emptied it, a solve gets kDegradedIterations and no recovery, keeps the previous sample's
// solution and counts as a fallback, until the bucket refills. That bounds what failures can add to a block.
struct RecoveryBudget {
  double tokens = 0.0;
  double perSample = 0.0;  // 0: unlimited
  double cap = 0.0;
};

struct ClusterStats {
  long iterations = 0, evaluations = 0, factorisations = 0;
  long retries = 0, fallbacks = 0, nonFinite = 0;
  long failedEvaluations = 0, recoveryEvaluations = 0, worstRecovery = 0;  // device evaluations spent on recoveries
  long degraded = 0;  // failed solves while the recovery budget was empty
};

// Where an input comes from: scale * (now * *value + before * *previous).
struct InputSource {
  const double* value;
  const double* previous;
  double now, before, scale;
};

class Cluster {
 public:
  // Picks the fixed-size kernel for the device mix. The system is prepared and built.
  static std::unique_ptr<Cluster> create(std::unique_ptr<solver::ClusterSystem>, Laws, const SolverSettings&);
  virtual ~Cluster() = default;
  Cluster(const Cluster&) = delete;
  Cluster& operator=(const Cluster&) = delete;

  // prepare() time: the operating point and the input values it was computed with.
  void setOperatingPoint(const solver::OperatingPoint&, const std::vector<double>& inputValues);

  // Audio thread. None of these allocate.
  virtual void step() = 0;
  void rebuild(const circuit::Controls&);    // exact rebuild; states carry across
  void moveKnobs(const circuit::Controls&);  // exact low-rank update against the last rebuild
  void reset(const circuit::Controls&);      // rebuild, then the operating point
  bool moving() const { return active_ > 0; }

  const solver::ClusterSystem& system() const { return *system_; }
  const double* output(int k) const { return &y_[static_cast<size_t>(k)]; }
  const double* previousOutput(int k) const { return &yPrevious_[static_cast<size_t>(k)]; }
  const std::vector<double>& operatingInputs() const { return u0_; }

  // Companion coupling: this cluster also publishes the remainder of output `output` (i_res) for its upstream cluster.
  void setCompanionOutput(int output, int cutInput, double tubeConductance, bool hold);
  const double* companionOutput() const { return &companion_; }
  ClusterStats stats() const;
  long fallbacks() const { return stats_.fallbacks; }
  long nonFinite() const { return stats_.nonFinite; }
  long retries() const { return stats_.retries; }
  long rebuilds() const { return rebuilds_; }

  std::vector<InputSource> sources;
  RecoveryBudget* budget = nullptr;

 protected:
  Cluster(std::unique_ptr<solver::ClusterSystem>, const SolverSettings&);

  virtual double* newtonVoltages() = 0;
  virtual void matricesChanged(bool currentsStillValid) = 0;
  virtual void restart() = 0;  // after a state change: linear part and first evaluation at v
  virtual void counters(ClusterStats&) const = 0;

  void linearPart();
  void updateLowRankResidual();
  void subtractLowRank(const double* C, int stride, double* out) const;
  void restoreOperatingPoint();
  void updateCompanion();
  bool hasCompanion() const { return companionRow_ >= 0; }

  std::unique_ptr<solver::ClusterSystem> system_;
  SolverSettings settings_;
  ClusterStats stats_;
  MulAddFn portKernel_, outKernel_;

  std::vector<double> u0_, v0_, x0_, y0_, y_, yPrevious_;
  std::vector<double> ul_;  // [u; i]
  std::vector<double> ps_, os_, p_, pLast_, vPrevious_;
  std::vector<double> o_;   // [w; y]: its head is the state vector, so advancing needs no copy
  std::vector<double> K_;   // K in force (the base matrix, or its low-rank update)
  std::vector<double> rs_, r_, t_;
  int active_ = 0;
  std::vector<double> phi_;
  std::vector<int> activeIndex_;

 private:
  void updateK();

  unsigned switches_ = 0;
  bool built_ = false;
  long rebuilds_ = 0;
  std::vector<double> M_, delta_, element_, knobSeen_;
  std::vector<int> pivot_;
  int companionRow_ = -1, companionCut_ = 0;
  bool companionHold_ = false;
  double companionTubes_ = 0.0, companionCutBefore_ = 0.0, companion_ = 0.0;
  std::vector<double> companionCurrents_;
  std::vector<char> wasPresent_;
};

}  // namespace magmora::graph
