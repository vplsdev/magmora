// The partitioned amplifier (port of proto/blockamp/partition.py: PartSim, run_clusters).
#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "circuit/AmpNetlist.h"
#include "graph/Cluster.h"

// Compile switch: run the product on the closed-form device laws.
#ifdef MAGMORA_CLOSED_FORM_DEVICES
#define MAGMORA_DEFAULT_LAWS ::magmora::graph::Laws::Closed
#else
#define MAGMORA_DEFAULT_LAWS ::magmora::graph::Laws::Table
#endif

namespace magmora::graph {

struct GraphOptions {
  double sampleRate = 192000.0;
  circuit::Partition partition = circuit::Partition::Split;
  int valueSet = circuit::kDefaultValueSet;
  std::vector<std::string> probes;
  Laws laws = MAGMORA_DEFAULT_LAWS;
  SolverSettings solver{1e-4, 40, true, Fallback::Adaptive};  // product tolerance; the prototype and the golden data use 1e-6
  // Recovery budget in device evaluations per internal sample and its ceiling (0: unlimited, as the prototype).
  double recoveryPerSample = 0.0, recoveryCap = 0.0;
  double dcCapLeak = circuit::netdata::kDcCapLeak;
  bool companion = circuit::netdata::kCompanionCoupling;  // coupling of the signal cuts
  // The controls at which the companion conductances are taken. The prototype takes the controls it is built with;
  // any fixed choice is exact (the remainder uses the same conductance), so the product fixes them to the defaults
  // and an engine's output never depends on where its knobs stood at prepare().
  bool companionAtDefaults = false;               // as the prototype (dk.DC_CAP_LEAK)
};

class AmpGraph {
 public:
  AmpGraph();
  ~AmpGraph();
  AmpGraph(const AmpGraph&) = delete;
  AmpGraph& operator=(const AmpGraph&) = delete;

  // Allocates and finds the operating point. The structural switches are fixed here.
  bool prepare(const GraphOptions&, const circuit::Controls&);

  // Audio thread: no allocation, locks or system calls.
  void reset(const circuit::Controls&);
  void setKnobs(const circuit::Controls&);     // exact, cheap enough for every host sample
  void setSwitches(const circuit::Controls&);  // rebuilds the clusters that contain a relay
  void settle();                               // rebuilds the clusters whose knobs moved
  MAGMORA_INLINE void processSample(double input) {
    external_ = input;
    if (budget_.perSample != 0.0 && budget_.tokens < budget_.cap)
      budget_.tokens = std::min(budget_.cap, budget_.tokens + budget_.perSample);
    for (Cluster* c : schedule_) c->step();
  }
  long fallbacks() const;
  long nonFinite() const;
  long retries() const;
  long rebuilds() const;

  // Runs one cluster alone: every input is held at its operating value except `input`, which gets processSample's.
  bool isolate(const std::string& cluster, const std::string& input);

  // Stable pointers to outputs, or null.
  const double* output(const std::string& cluster, const std::string& label) const;
  const double* probe(const std::string& node) const;  // from the last cluster that contains the node

  const circuit::PartitionedAmp& amp() const { return *amp_; }
  const Cluster& cluster(int k) const { return *clusters_[static_cast<size_t>(k)]; }
  int size() const { return static_cast<int>(clusters_.size()); }

  const std::vector<std::string>& dcNodes() const { return dcNodes_; }
  const std::vector<double>& dcVoltages() const { return dcVoltages_; }

 private:
  // Adds each cut's companion resistor and remainder output (allocates; before the systems are built).
  bool addCompanions(const GraphOptions&, devices::Library&, const std::vector<std::string>& nodes,
                     const std::vector<double>& voltages);
  bool findOperatingPoint(std::vector<std::unique_ptr<solver::ClusterSystem>>&, devices::Library&, const GraphOptions&,
                          std::vector<solver::OperatingPoint>&, std::vector<std::vector<double>>&);
  void connect(bool companion);

  circuit::Controls controls_;
  unsigned structural_ = 0;
  std::unique_ptr<devices::Library> library_;
  std::unique_ptr<circuit::PartitionedAmp> amp_;
  std::vector<std::unique_ptr<Cluster>> clusters_;
  std::vector<Cluster*> schedule_, switched_, adjustable_;
  std::vector<std::string> dcNodes_;
  std::vector<double> dcVoltages_;
  struct Companion {
    int cluster, output, cutInput;
    double conductance, tubes;
  };
  std::vector<Companion> companions_;
  double external_ = 0.0;
  RecoveryBudget budget_;
};

}  // namespace magmora::graph
