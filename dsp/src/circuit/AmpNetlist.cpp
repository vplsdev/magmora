#include "circuit/AmpNetlist.h"

#include <algorithm>
#include <iterator>

namespace magmora::circuit {
namespace {

unsigned sectionBit(const char* name) {
  for (size_t s = 0; s < std::size(netdata::kSections); ++s)
    if (std::string(netdata::kSections[s]) == name) return 1u << s;
  return 0;
}

struct NamedWire {
  int dst;
  std::string input;
  int src;
  std::string output;
  double scale;
};

}  // namespace

int PartitionedAmp::find(const std::string& name) const {
  const auto it = std::find(names.begin(), names.end(), name);
  return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}

Circuit monolithic(unsigned structural, int valueSet) {
  return Circuit::fromSections((1u << std::size(netdata::kSections)) - 1u, structural, valueSet);
}

PartitionedAmp partitionAmp(Partition partition, unsigned structural, int valueSet,
                            const std::vector<std::string>& probes) {
  const bool split = partition == Partition::Split;
  const ClusterDef* defs = split ? netdata::kSplitPartition : netdata::kMergedPartition;
  const int count = static_cast<int>(split ? std::size(netdata::kSplitPartition) : std::size(netdata::kMergedPartition));
  const unsigned supply = sectionBit("supply"), power = sectionBit("power");

  std::vector<std::string> allRails;
  for (int c = 0; c < count; ++c)
    for (int r = 0; r < defs[c].numRails; ++r) allRails.emplace_back(defs[c].rails[r]);
  std::sort(allRails.begin(), allRails.end());
  allRails.erase(std::unique(allRails.begin(), allRails.end()), allRails.end());

  PartitionedAmp out;
  std::vector<NamedWire> wires;
  for (int ci = 0; ci < count; ++ci) {
    const ClusterDef& def = defs[ci];
    Circuit c = Circuit::fromSections(def.sections, structural, valueSet);
    if (def.cutIn) {
      const int source = c.addSource(Kind::VoltageSource, "cut_in", def.cutIn, "0", 0.0, true);
      c.addBranchOutput("i_in", source);
      wires.push_back({ci, "cut_in", ci - 1, "v_out", 1.0});
      wires.push_back({ci - 1, "load", ci, "i_in", -1.0});  // the branch current is minus the current drawn
    }
    if (def.cutOut) {
      c.addSource(Kind::CurrentSource, "load", def.cutOut, "0", 0.0, true);
      c.addOutput("v_out", def.cutOut);
    }
    for (int r = 0; r < def.numRails; ++r) {
      const std::string rail = def.rails[r];
      const int source = c.addSource(Kind::VoltageSource, "rail_" + rail, def.rails[r], "0", 0.0, true);
      c.addBranchOutput("i_" + rail, source);
      wires.push_back({ci, "rail_" + rail, count - 1, "v_" + rail, 1.0});
    }
    if (def.sections & supply) {
      for (const std::string& rail : allRails) c.addOutput("v_" + rail, rail.c_str());
      for (int cj = 0; cj < count; ++cj)
        for (int r = 0; r < defs[cj].numRails; ++r) {
          const std::string rail = defs[cj].rails[r];
          const std::string name = "sink_" + rail + "_" + defs[cj].name;
          c.addSource(Kind::CurrentSource, name, rail.c_str(), "0", 0.0, true);
          wires.push_back({ci, name, cj, "i_" + rail, -1.0});
        }
    }
    if (def.sections & power) c.addOutput("spk", "spk");
    if (def.cutIn && ci > 0 && defs[ci - 1].cutOut && std::string(defs[ci - 1].cutOut) == def.cutIn)
      out.cuts.push_back({ci - 1, ci, def.cutIn});
    for (const std::string& probe : probes)
      if (c.findNode(probe) >= 0) c.addOutput("probe_" + probe, probe.c_str());
    out.names.emplace_back(def.name);
    out.extrapolated.push_back(def.extrapolated);
    out.clusters.push_back(std::move(c));
  }
  for (const NamedWire& w : wires)
    out.wires.push_back({w.dst, out.clusters[static_cast<size_t>(w.dst)].findInput(w.input), w.src,
                         out.clusters[static_cast<size_t>(w.src)].findOutput(w.output), w.scale});
  return out;
}

}  // namespace magmora::circuit
