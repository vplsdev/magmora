#include "graph/AmpGraph.h"

#include <algorithm>

#include "devices/Evaluators.h"
#include "solver/DenseLU.h"

namespace magmora::graph {
namespace {
constexpr int kLoadIterations = 6;  // fixed point on the DC load and sink currents
}

AmpGraph::AmpGraph() = default;
AmpGraph::~AmpGraph() = default;

namespace {

// The cut and rail sources of a cluster at the monolithic operating point; everything else at zero.
bool inputsAtOperatingPoint(const circuit::Circuit& c, const std::vector<std::string>& nodes,
                            const std::vector<double>& voltages, std::vector<double>& inputs) {
  inputs.assign(c.inputs.size(), 0.0);
  for (size_t j = 0; j < c.inputs.size(); ++j) {
    const circuit::Item& source = c.items[static_cast<size_t>(c.inputs[j])];
    const bool copiesVoltage = source.name == "cut_in" || source.name.compare(0, 5, "rail_") == 0;
    if (!copiesVoltage) continue;
    const std::string& name = c.nodes[static_cast<size_t>(source.variants[0].nodes[0])];
    const auto it = std::find(nodes.begin(), nodes.end(), name);
    if (it == nodes.end()) return false;
    inputs[j] = voltages[static_cast<size_t>(it - nodes.begin())];
  }
  return true;
}

}  // namespace

bool AmpGraph::prepare(const GraphOptions& options, const circuit::Controls& controls) {
  schedule_.clear();
  switched_.clear();
  adjustable_.clear();
  clusters_.clear();
  companions_.clear();
  controls_ = controls;
  structural_ = controls.switches & circuit::kStructuralSwitches;
  external_ = 0.0;
  library_ = std::make_unique<devices::Library>();
  amp_ = std::make_unique<circuit::PartitionedAmp>(
      circuit::partitionAmp(options.partition, structural_, options.valueSet, options.probes));

  // the monolithic operating point: the cut and rail voltages, and the DC table of the probe tool
  {
    const solver::DcOptions dc{options.laws == Laws::Closed, options.dcCapLeak};
    const circuit::Circuit whole = circuit::monolithic(structural_, options.valueSet);
    solver::ClusterSystem system;
    solver::OperatingPoint point;
    if (!system.prepare(whole, options.sampleRate, *library_) ||
        !solver::solveOperatingPoint(system, controls_, std::vector<double>(whole.inputs.size(), 0.0), dc, point))
      return false;
    dcNodes_ = whole.nodes;
    dcVoltages_.assign(point.x.begin(), point.x.begin() + static_cast<std::ptrdiff_t>(whole.nodes.size()));
  }
  if (options.companion && !addCompanions(options, *library_, dcNodes_, dcVoltages_)) return false;

  const size_t count = amp_->clusters.size();
  std::vector<std::unique_ptr<solver::ClusterSystem>> systems;
  for (const circuit::Circuit& c : amp_->clusters) {
    systems.push_back(std::make_unique<solver::ClusterSystem>());
    if (!systems.back()->prepare(c, options.sampleRate, *library_) || !systems.back()->build(controls_)) return false;
  }
  std::vector<solver::OperatingPoint> points(count);
  std::vector<std::vector<double>> inputs(count);
  if (!findOperatingPoint(systems, *library_, options, points, inputs)) return false;

  for (size_t k = 0; k < count; ++k) {
    std::unique_ptr<Cluster> c = Cluster::create(std::move(systems[k]), options.laws, options.solver);
    if (!c) return false;
    clusters_.push_back(std::move(c));
  }
  budget_ = {options.recoveryCap, options.recoveryPerSample, options.recoveryCap};
  for (const auto& c : clusters_) c->budget = &budget_;
  for (const Companion& c : companions_)  // held on every cut (proto: partition.PartSim, no_extrap = ())
    clusters_[static_cast<size_t>(c.cluster)]->setCompanionOutput(c.output, c.cutInput, c.tubes, true);
  for (size_t k = 0; k < count; ++k) clusters_[k]->setOperatingPoint(points[k], inputs[k]);
  for (const auto& c : clusters_) {
    schedule_.push_back(c.get());
    if (c->system().dependsOnSwitches()) switched_.push_back(c.get());
    if (c->system().matrices.variables > 0) adjustable_.push_back(c.get());
  }
  connect(options.companion);
  reset(controls_);
  return true;
}

// Companion coupling (port of partition.build_clusters, companion=True). Within a sample the load that
// a downstream cluster presents at its cut is, to first order, a conductance g: its companion network with the tubes
// linearised at the operating point. g becomes a resistor in the upstream cluster, solved implicitly there; only the
// remainder i - g v is passed back. Cuts are taken from the output towards the input, because each g includes the
// resistor already placed behind it.
bool AmpGraph::addCompanions(const GraphOptions& options, devices::Library& library, const std::vector<std::string>& nodes,
                             const std::vector<double>& voltages) {
  circuit::Controls at = controls_;
  if (options.companionAtDefaults) {
    at = circuit::Controls();
    at.switches = (at.switches & ~circuit::kStructuralSwitches) | structural_;
  }
  for (auto cut = amp_->cuts.rbegin(); cut != amp_->cuts.rend(); ++cut) {
    circuit::Circuit& down = amp_->clusters[static_cast<size_t>(cut->downstream)];
    solver::ClusterSystem system;
    if (!system.prepare(down, options.sampleRate, library) || !system.build(at)) return false;
    std::vector<double> inputs;
    if (!inputsAtOperatingPoint(down, nodes, voltages, inputs)) return false;
    solver::OperatingPoint point;
    if (!solver::solveOperatingPoint(system, at, inputs, {true, options.dcCapLeak}, point)) return false;

    // the tubes' Jacobian at that point, dense
    const solver::ClusterMatrices& m = system.matrices;
    const int np = m.ports, rows = m.portRows;
    std::vector<double> J(static_cast<size_t>(np * np), 0.0), i(static_cast<size_t>(rows), 0.0);
    {
      const devices::DeviceSet& set = system.devices();
      const int nt = static_cast<int>(set.triodes.size()), npen = static_cast<int>(set.pentodes.size());
      const int nd = static_cast<int>(set.diodes.size());
      devices::ClosedLaws<solver::DynamicShape> laws;
      laws.bind(set);
      std::vector<double> compact(static_cast<size_t>(3 * nt + 7 * npen + nd) + 1, 0.0);
      laws.eval(solver::DynamicShape(nt, npen, nd), point.v.data(), i.data(), compact.data());
      const auto set_ = [&](int r, int c, double v) { J[static_cast<size_t>(r * np + c)] = v; };
      const double* j = compact.data();
      for (int d = 0; d < nt; ++d, j += devices::kTriodeJacobian) {
        set_(2 * d, 2 * d, j[0]);
        set_(2 * d, 2 * d + 1, j[1]);
        set_(2 * d + 1, 2 * d + 1, j[2]);
      }
      for (int d = 0; d < npen; ++d, j += devices::kPentodeJacobian) {
        const int q = 2 * nt + 3 * d;
        for (int c = 0; c < 3; ++c) {
          set_(q, q + c, j[c]);
          set_(q + 1, q + c, j[3 + c]);
        }
        set_(q + 2, q + 2, j[6]);
      }
      for (int d = 0; d < nd; ++d) set_(2 * nt + 3 * npen + d, 2 * nt + 3 * npen + d, j[d]);
    }
    const int cutInput = down.findInput("cut_in"), branchOutput = down.findOutput("i_in");
    if (cutInput < 0 || branchOutput < 0) return false;
    // z = (I - K J)^-1 Pv(cut);  g = -Yv(i_in, cut) - Yi(i_in) J z
    std::vector<double> M(static_cast<size_t>(np * np)), z(static_cast<size_t>(std::max(np, 1)));
    std::vector<int> pivot(static_cast<size_t>(std::max(np, 1)));
    for (int r = 0; r < np; ++r) {
      for (int c = 0; c < np; ++c) {
        double kj = 0.0;
        for (int k = 0; k < np; ++k) kj += m.K[static_cast<size_t>(k * rows + r)] * J[static_cast<size_t>(k * np + c)];
        M[static_cast<size_t>(r * np + c)] = (r == c ? 1.0 : 0.0) - kj;
      }
      z[static_cast<size_t>(r)] = m.Pu[static_cast<size_t>(cutInput * rows + r)];
    }
    if (np > 0) {
      if (!solver::luFactor(M.data(), pivot.data(), np)) return false;
      solver::luSolve(M.data(), pivot.data(), np, z.data(), 1, 1);
    }
    const int row = m.states + branchOutput, stride = m.outRows;
    double tubes = 0.0;
    for (int p = 0; p < np; ++p) {
      double jz = 0.0;
      for (int k = 0; k < np; ++k) jz += J[static_cast<size_t>(p * np + k)] * z[static_cast<size_t>(k)];
      tubes -= m.WL[static_cast<size_t>((m.inputs + p) * stride + row)] * jz;
    }
    const double g = -m.WL[static_cast<size_t>(cutInput * stride + row)] + tubes;
    if (!(g > 0.0)) return false;

    const int source = down.inputs[static_cast<size_t>(cutInput)];
    down.addCompanionOutput("i_res", source, cut->node.c_str(), g);
    amp_->clusters[static_cast<size_t>(cut->upstream)].addResistor("Rcomp", cut->node.c_str(), "0", 1.0 / g);
    const int output = static_cast<int>(down.outputs.size()) - 1;
    const int load = amp_->clusters[static_cast<size_t>(cut->upstream)].findInput("load");
    for (circuit::Wire& w : amp_->wires)
      if (w.dst == cut->upstream && w.input == load && w.src == cut->downstream) {
        w.output = output;
        w.companion = true;
      }
    companions_.push_back({cut->downstream, output, cutInput, g, tubes});
  }
  return true;
}

// The load and sink currents settle in a few passes, from the monolithic cut and rail voltages.
bool AmpGraph::findOperatingPoint(std::vector<std::unique_ptr<solver::ClusterSystem>>& systems, devices::Library&,
                                  const GraphOptions& options, std::vector<solver::OperatingPoint>& points,
                                  std::vector<std::vector<double>>& inputs) {
  const solver::DcOptions dc{options.laws == Laws::Closed, options.dcCapLeak};
  const size_t count = systems.size();
  for (size_t k = 0; k < count; ++k)
    if (!inputsAtOperatingPoint(amp_->clusters[k], dcNodes_, dcVoltages_, inputs[k])) return false;
  for (int pass = 0; pass < kLoadIterations; ++pass) {
    for (size_t k = 0; k < count; ++k)
      if (!solver::solveOperatingPoint(*systems[k], controls_, inputs[k], dc, points[k])) return false;
    for (const circuit::Wire& w : amp_->wires) {
      const circuit::Circuit& to = amp_->clusters[static_cast<size_t>(w.dst)];
      if (to.items[static_cast<size_t>(to.inputs[static_cast<size_t>(w.input)])].kind != circuit::Kind::CurrentSource) continue;
      const double current = systems[static_cast<size_t>(w.src)]->outputValue(w.output, points[static_cast<size_t>(w.src)].x.data());
      inputs[static_cast<size_t>(w.dst)][static_cast<size_t>(w.input)] = w.scale * current;
    }
  }
  return true;
}

void AmpGraph::connect(bool companion) {
  for (const auto& c : clusters_)
    for (InputSource& s : c->sources) s = {&external_, &external_, 1.0, 0.0, 1.0};
  for (const circuit::Wire& w : amp_->wires) {
    const Cluster& from = *clusters_[static_cast<size_t>(w.src)];
    if (w.companion) {  // the remainder, already formed (and held) by the downstream cluster
      clusters_[static_cast<size_t>(w.dst)]->sources[static_cast<size_t>(w.input)] = {
          from.companionOutput(), from.companionOutput(), 1.0, 0.0, w.scale};
      continue;
    }
    // An upstream value is this sample's. A downstream one is a sample old, extrapolated unless that cluster forbids
    // it (the load that P presents to T, under the delayed coupling; with companion coupling none does).
    const bool extrapolate = w.src >= w.dst && (companion || amp_->extrapolated[static_cast<size_t>(w.src)]);
    clusters_[static_cast<size_t>(w.dst)]->sources[static_cast<size_t>(w.input)] = {
        from.output(w.output), from.previousOutput(w.output), extrapolate ? 2.0 : 1.0, extrapolate ? -1.0 : 0.0, w.scale};
  }
}

bool AmpGraph::isolate(const std::string& name, const std::string& input) {
  const int k = amp_->find(name);
  if (k < 0) return false;
  const int driven = amp_->clusters[static_cast<size_t>(k)].findInput(input);
  if (driven < 0) return false;
  Cluster& c = *clusters_[static_cast<size_t>(k)];
  for (size_t j = 0; j < c.sources.size(); ++j) {
    const double* held = &c.operatingInputs()[j];
    c.sources[j] = {held, held, 1.0, 0.0, 1.0};
  }
  c.sources[static_cast<size_t>(driven)] = {&external_, &external_, 1.0, 0.0, 1.0};
  schedule_.assign(1, &c);
  return true;
}

void AmpGraph::reset(const circuit::Controls& controls) {
  budget_.tokens = budget_.cap;
  controls_ = controls;
  controls_.switches = (controls.switches & ~circuit::kStructuralSwitches) | structural_;
  external_ = 0.0;
  for (const auto& c : clusters_) c->reset(controls_);
}

void AmpGraph::setKnobs(const circuit::Controls& controls) {
  controls_.knobs = controls.knobs;
  for (Cluster* c : adjustable_) c->moveKnobs(controls_);
}

void AmpGraph::setSwitches(const circuit::Controls& controls) {
  controls_.switches = (controls.switches & ~circuit::kStructuralSwitches) | structural_;
  for (Cluster* c : switched_) c->rebuild(controls_);
}

void AmpGraph::settle() {
  for (Cluster* c : adjustable_)
    if (c->moving()) c->rebuild(controls_);
}

long AmpGraph::fallbacks() const {
  long total = 0;
  for (const auto& c : clusters_) total += c->fallbacks();
  return total;
}

long AmpGraph::retries() const {
  long total = 0;
  for (const auto& c : clusters_) total += c->retries();
  return total;
}

long AmpGraph::rebuilds() const {
  long total = 0;
  for (const auto& c : clusters_) total += c->rebuilds();
  return total;
}

long AmpGraph::nonFinite() const {
  long total = 0;
  for (const auto& c : clusters_) total += c->nonFinite();
  return total;
}

const double* AmpGraph::output(const std::string& cluster, const std::string& label) const {
  const int k = amp_->find(cluster);
  if (k < 0) return nullptr;
  const int o = amp_->clusters[static_cast<size_t>(k)].findOutput(label);
  return o < 0 ? nullptr : clusters_[static_cast<size_t>(k)]->output(o);
}

const double* AmpGraph::probe(const std::string& node) const {
  const std::string label = node == "spk" ? "spk" : "probe_" + node;
  for (size_t k = clusters_.size(); k-- > 0;) {
    const int o = amp_->clusters[k].findOutput(label);
    if (o >= 0) return clusters_[k]->output(o);
  }
  return nullptr;
}

}  // namespace magmora::graph
