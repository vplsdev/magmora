// Cutting the amplifier into clusters (port of proto/blockamp/partition.py: build_clusters).
#pragma once
#include <string>
#include <vector>

#include "circuit/Netlist.h"

namespace magmora::circuit {

enum class Partition {
  Split,   // the product: A B C D T P and the linear supply cluster S
  Merged,  // supply inside P; what the single-stage SPICE references were generated with
};

struct Wire {
  int dst, input;   // cluster, index among its inputs
  int src, output;  // cluster, output index
  double scale;
  bool companion = false;  // the remainder of a companion-coupled cut (graph/AmpGraph.cpp), not a plain output
};

// One cut where a cluster's input voltage source copies the upstream cluster's output node.
struct Cut {
  int upstream, downstream;
  std::string node;
};

struct PartitionedAmp {
  std::vector<std::string> names;
  std::vector<bool> extrapolated;
  std::vector<Circuit> clusters;
  std::vector<Wire> wires;
  std::vector<Cut> cuts;

  int find(const std::string& name) const;
};

Circuit monolithic(unsigned structural, int valueSet);
PartitionedAmp partitionAmp(Partition, unsigned structural, int valueSet, const std::vector<std::string>& probes);

}  // namespace magmora::circuit
