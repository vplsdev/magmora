#include "solver/ClusterSystem.h"

#include <algorithm>

#include "common/MatVec.h"
#include "solver/DenseLU.h"

namespace magmora::solver {
namespace {

using circuit::Item;
using circuit::kGround;
using circuit::Kind;
using circuit::Variant;

constexpr double kInductorDcResistance = 1e-9;
constexpr double kTransformerLeak = 1e-12;

size_t at(int row, int column, int stride) { return static_cast<size_t>(column * stride + row); }  // column-major

bool isDevice(Kind k) { return k == Kind::Triode || k == Kind::Pentode || k == Kind::Diode; }

}  // namespace

bool ClusterSystem::prepare(const circuit::Circuit& c, double sampleRate, devices::Library& library, bool foldDiodes) {
  circuit_ = &c;
  sampleRate_ = sampleRate;
  matrices = {};
  devices_ = {};
  devices_.library = &library;
  ports_.clear();
  portItems_.clear();
  reactive_.clear();
  variableItems_.clear();
  variableKnobs_.clear();
  dependsOnSwitches_ = false;

  const int count = static_cast<int>(c.items.size());
  extra_.assign(static_cast<size_t>(count), -1);
  int next = static_cast<int>(c.nodes.size());

  // relay combinations that can occur with this structure
  const uint64_t possible = c.combinations;

  for (int k = 0; k < count; ++k) {
    const Item& item = c.items[static_cast<size_t>(k)];
    uint64_t covered = 0;
    bool hasLaw = false;
    int knob = -1;
    for (const Variant& v : item.variants) {
      covered |= v.combinations;
      if (v.law) {
        if (hasLaw && v.law->knob != knob) return false;  // one element, one knob
        hasLaw = true;
        knob = v.law->knob;
      }
    }
    const bool varies = item.variants.size() > 1 || (covered & possible) != possible;
    dependsOnSwitches_ = dependsOnSwitches_ || varies;
    if (varies && (isDevice(item.kind) || item.kind == Kind::Transformer)) return false;  // not representable
    if (hasLaw) {
      if (item.kind != Kind::Resistor) return false;
      variableItems_.push_back(k);
      variableKnobs_.push_back(knob);
    }
    if (item.kind == Kind::Capacitor || item.kind == Kind::Inductor) reactive_.push_back(k);
    if (item.kind == Kind::VoltageSource || item.kind == Kind::Inductor) {
      extra_[static_cast<size_t>(k)] = next++;
    } else if (item.kind == Kind::Transformer) {
      extra_[static_cast<size_t>(k)] = next;
      next += item.variants[0].numNodes / 2 + 1;
    }
  }
  unknowns_ = next;

  // Port order: triodes, pentodes, remaining diodes (the kernels depend on it).
  std::vector<int> pentodeItems;
  for (int k = 0; k < count; ++k) {
    const Item& item = c.items[static_cast<size_t>(k)];
    if (item.kind != Kind::Triode) continue;
    const auto& n = item.variants[0].nodes;  // plate, grid, cathode
    ports_.push_back({n[0], n[2]});
    ports_.push_back({n[1], n[2]});
    portItems_.insert(portItems_.end(), 2, k);
    devices_.triodes.push_back(library.addTriode(item.params));
  }
  for (int k = 0; k < count; ++k) {
    const Item& item = c.items[static_cast<size_t>(k)];
    if (item.kind != Kind::Pentode) continue;
    const auto& n = item.variants[0].nodes;  // plate, screen, grid, cathode
    ports_.push_back({n[0], n[3]});
    ports_.push_back({n[1], n[3]});
    ports_.push_back({n[2], n[3]});
    portItems_.insert(portItems_.end(), 3, k);
    devices_.pentodes.push_back(library.addPentode(item.params));
    devices_.flyback.push_back(nullptr);
    pentodeItems.push_back(k);
  }
  for (int k = 0; k < count; ++k) {
    const Item& item = c.items[static_cast<size_t>(k)];
    if (item.kind != Kind::Diode) continue;
    const auto& n = item.variants[0].nodes;  // anode, cathode
    const devices::DiodeLaw* law = library.addDiode(item.params);
    bool folded = false;
    for (size_t p = 0; foldDiodes && !folded && p < pentodeItems.size(); ++p) {
      const auto& host = c.items[static_cast<size_t>(pentodeItems[p])].variants[0].nodes;
      if (!devices_.flyback[p] && n[0] == host[3] && n[1] == host[0]) {
        devices_.flyback[p] = law;
        folded = true;
      }
    }
    if (folded) continue;
    ports_.push_back({n[0], n[1]});
    portItems_.push_back(k);
    devices_.diodes.push_back(law);
  }

  ClusterMatrices& m = matrices;
  m.ports = static_cast<int>(ports_.size());
  m.portRows = padded(m.ports);
  m.inputs = static_cast<int>(c.inputs.size());
  m.outputs = static_cast<int>(c.outputs.size());
  m.variables = static_cast<int>(variableItems_.size());
  const int maxStates = static_cast<int>(reactive_.size());
  const int maxOutRows = padded(maxStates + m.outputs);
  const auto size = [](int rows, int cols) { return static_cast<size_t>(rows * cols); };
  m.Pc.assign(static_cast<size_t>(m.portRows), 0.0);
  m.Pw.assign(size(m.portRows, maxStates), 0.0);
  m.Pu.assign(size(m.portRows, m.inputs), 0.0);
  m.K.assign(size(m.portRows, m.ports), 0.0);
  m.Wc.assign(static_cast<size_t>(maxOutRows), 0.0);
  m.WS.assign(size(maxOutRows, maxStates), 0.0);
  m.WL.assign(size(maxOutRows, m.inputs + m.ports), 0.0);
  m.CP.assign(size(m.portRows, m.variables), 0.0);
  m.CW.assign(size(maxOutRows, m.variables), 0.0);
  m.Rc.assign(static_cast<size_t>(m.variables), 0.0);
  m.RS.assign(size(m.variables, maxStates), 0.0);
  m.RL.assign(size(m.variables, m.inputs + m.ports), 0.0);
  m.Q.assign(size(m.variables, m.variables), 0.0);
  m.baseConductance.assign(static_cast<size_t>(m.variables), 0.0);

  const int nodesWithGround = static_cast<int>(c.nodes.size()) + 1;
  G_.assign(size(unknowns_, unknowns_), 0.0);
  const size_t solutionSize = size(unknowns_, m.inputs + 1 + maxStates + m.ports + m.variables);
  for (auto* v : {&X_, &rhs_, &correction_}) v->assign(solutionSize, 0.0);
  matrix_.assign(G_.size(), 0.0);
  pivot_.assign(static_cast<size_t>(unknowns_), 0);
  expansion_.assign(size(maxStates, maxStates), 0.0);
  companion_.assign(static_cast<size_t>(maxStates), 0.0);
  present_.assign(static_cast<size_t>(maxStates), 0);
  stateReactive_.assign(static_cast<size_t>(maxStates), 0);
  root_.assign(static_cast<size_t>(nodesWithGround), 0);
  queue_.assign(static_cast<size_t>(nodesWithGround), 0);
  via_.assign(static_cast<size_t>(nodesWithGround), 0);
  pathCoefficient_.assign(static_cast<size_t>(nodesWithGround), 0.0);
  return true;
}

double ClusterSystem::value(int item, const circuit::Controls& controls) const {
  const Variant* v = circuit_->items[static_cast<size_t>(item)].variant(controls.switches);
  return v ? circuit::valueOf(*v, controls) : 0.0;
}

double ClusterSystem::conductance(int variable, const circuit::Controls& controls) const {
  const Item& item = circuit_->items[static_cast<size_t>(variableItems_[static_cast<size_t>(variable)])];
  const Variant* v = item.variant(controls.switches);
  return v ? 1.0 / circuit::valueOf(*v, controls) : 0.0;
}

double ClusterSystem::reactiveValue(int reactive, unsigned switches, const double* x) const {
  const int index = reactive_[static_cast<size_t>(reactive)];
  const Item& item = circuit_->items[static_cast<size_t>(index)];
  if (item.kind == Kind::Inductor) return x[branch(index)];
  const Variant* v = item.variant(switches);
  if (!v) v = &item.variants[0];
  const auto node = [x](int n) { return n == kGround ? 0.0 : x[n]; };
  return node(v->nodes[0]) - node(v->nodes[1]);
}

double ClusterSystem::outputValue(int output, const double* x) const {
  const circuit::Output& o = circuit_->outputs[static_cast<size_t>(output)];
  const auto node = [x](int n) { return n == kGround ? 0.0 : x[n]; };
  const double voltage = node(o.plus) - node(o.minus);
  if (o.branch < 0) return voltage;
  return x[branch(o.branch)] + o.gain * voltage;
}

void ClusterSystem::assemble(const circuit::Controls& controls, AssemblyMode mode, double* G) const {
  const int n = unknowns_;
  const auto& items = circuit_->items;
  const double T = 1.0 / sampleRate_;
  std::fill(G, G + n * n, 0.0);
  const auto g = [G, n](int r, int c) -> double& { return G[r * n + c]; };
  const auto conductanceBetween = [&g](int a, int b, double y) {
    if (a != kGround) g(a, a) += y;
    if (b != kGround) g(b, b) += y;
    if (a != kGround && b != kGround) {
      g(a, b) -= y;
      g(b, a) -= y;
    }
  };
  const auto coupleBranch = [&g](int a, int b, int j) {
    if (a != kGround) {
      g(a, j) += 1.0;
      g(j, a) += 1.0;
    }
    if (b != kGround) {
      g(b, j) -= 1.0;
      g(j, b) -= 1.0;
    }
  };

  for (size_t k = 0; k < items.size(); ++k) {
    const Variant* v = items[k].variant(controls.switches);
    if (!v) continue;
    const int a = v->nodes[0], b = v->nodes[1], j = extra_[k];
    switch (items[k].kind) {
      case Kind::Resistor:
        conductanceBetween(a, b, 1.0 / circuit::valueOf(*v, controls));
        break;
      case Kind::Capacitor:
        conductanceBetween(a, b, mode.dc ? mode.capLeak : 2.0 * v->value / T);
        break;
      case Kind::Inductor:
        coupleBranch(a, b, j);
        g(j, j) -= mode.dc ? kInductorDcResistance : 2.0 * v->value / T;
        break;
      case Kind::VoltageSource:
        coupleBranch(a, b, j);
        break;
      case Kind::Transformer: {
        const int windings = v->numNodes / 2, e = j + windings;
        for (int w = 0; w < windings; ++w) {
          coupleBranch(v->nodes[static_cast<size_t>(2 * w)], v->nodes[static_cast<size_t>(2 * w + 1)], j + w);
          const double turns = items[k].params[static_cast<size_t>(w)];
          g(j + w, e) -= turns;
          g(e, j + w) += turns;
        }
        g(e, e) += kTransformerLeak;
        break;
      }
      default:
        break;
    }
  }
  // A node whose every element is switched out, or the branch of an absent element.
  for (int r = 0; r < n; ++r) {
    bool empty = true;
    for (int c = 0; c < n && empty; ++c) empty = g(r, c) == 0.0;
    if (empty) g(r, r) = 1.0;
  }
}

// A spanning forest of the capacitors present; every capacitor outside it closes a loop through the forest.
void ClusterSystem::chooseStates(unsigned switches) {
  const auto& items = circuit_->items;
  const int count = static_cast<int>(reactive_.size());
  const int ground = static_cast<int>(circuit_->nodes.size());
  const auto node = [ground](int n) { return n == kGround ? ground : n; };
  const auto find = [this](int n) {
    while (root_[static_cast<size_t>(n)] != n) n = root_[static_cast<size_t>(n)];
    return n;
  };
  for (int n = 0; n <= ground; ++n) root_[static_cast<size_t>(n)] = n;
  std::fill(expansion_.begin(), expansion_.end(), 0.0);

  int states = 0;
  for (int r = 0; r < count; ++r) {
    const Item& item = items[static_cast<size_t>(reactive_[static_cast<size_t>(r)])];
    const Variant* v = item.variant(switches);
    present_[static_cast<size_t>(r)] = v != nullptr;
    companion_[static_cast<size_t>(r)] = v ? 2.0 * v->value / (1.0 / sampleRate_) : 0.0;
    if (!v) continue;
    bool isState = item.kind == Kind::Inductor;
    if (!isState) {
      const int a = find(node(v->nodes[0])), b = find(node(v->nodes[1]));
      if (a != b) {
        root_[static_cast<size_t>(a)] = b;
        isState = true;
      }
    }
    if (isState) {
      stateReactive_[static_cast<size_t>(states)] = r;
      expansion_[static_cast<size_t>(r * count + states)] = 1.0;
      ++states;
    }
  }

  int nextState = 0;
  for (int r = 0; r < count; ++r) {
    if (nextState < states && stateReactive_[static_cast<size_t>(nextState)] == r) {
      ++nextState;
      continue;
    }
    if (!present_[static_cast<size_t>(r)]) continue;
    const Variant& v = *items[static_cast<size_t>(reactive_[static_cast<size_t>(r)])].variant(switches);
    const int from = node(v.nodes[0]), to = node(v.nodes[1]);
    // breadth-first from `to` through the forest, then walk back from `from`
    std::fill(via_.begin(), via_.end(), -1);
    int head = 0, tail = 0;
    queue_[static_cast<size_t>(tail++)] = to;
    via_[static_cast<size_t>(to)] = states;  // visited
    while (head < tail && via_[static_cast<size_t>(from)] < 0) {
      const int here = queue_[static_cast<size_t>(head++)];
      for (int t = 0; t < states; ++t) {
        const Item& edge = items[static_cast<size_t>(reactive_[static_cast<size_t>(stateReactive_[static_cast<size_t>(t)])])];
        if (edge.kind != Kind::Capacitor) continue;
        const Variant& e = *edge.variant(switches);
        const int a = node(e.nodes[0]), b = node(e.nodes[1]);
        const int other = a == here ? b : (b == here ? a : -1);
        if (other < 0 || via_[static_cast<size_t>(other)] >= 0) continue;
        via_[static_cast<size_t>(other)] = t;
        // the edge's contribution to x[other] - x[here]
        pathCoefficient_[static_cast<size_t>(other)] = a == other ? 1.0 : -1.0;
        queue_[static_cast<size_t>(tail++)] = other;
      }
    }
    for (int here = from; here != to;) {
      const int t = via_[static_cast<size_t>(here)];
      const double sign = pathCoefficient_[static_cast<size_t>(here)];
      expansion_[static_cast<size_t>(r * count + t)] += sign;
      const Variant& e = *items[static_cast<size_t>(reactive_[static_cast<size_t>(stateReactive_[static_cast<size_t>(t)])])].variant(switches);
      here = node(sign > 0.0 ? e.nodes[1] : e.nodes[0]);
    }
  }
  matrices.states = states;
  matrices.outRows = padded(states + matrices.outputs);
}

bool ClusterSystem::build(const circuit::Controls& controls) {
  chooseStates(controls.switches);
  ClusterMatrices& m = matrices;
  const auto& c = *circuit_;
  const int n = unknowns_, ns = m.states, np = m.ports, nin = m.inputs, nv = m.variables;
  const int colConst = nin, colState = nin + 1, colPort = colState + ns, colVariable = colPort + np;
  const int columns = colVariable + nv;
  const unsigned switches = controls.switches;

  assemble(controls, {false, 0.0}, matrix_.data());
  std::copy(matrix_.begin(), matrix_.end(), G_.begin());
  if (!luFactor(G_.data(), pivot_.data(), n)) return false;

  double* X = X_.data();
  std::fill(X, X + n * columns, 0.0);
  const auto rhs = [X, columns](int row, int column, double value) {
    if (row != kGround) X[row * columns + column] += value;
  };
  int input = 0;
  for (size_t k = 0; k < c.items.size(); ++k) {
    const Item& item = c.items[k];
    if (item.kind != Kind::VoltageSource && item.kind != Kind::CurrentSource) continue;
    const int column = item.input ? input++ : colConst;
    const Variant* v = item.variant(switches);
    if (!v) continue;
    const double amount = item.input ? 1.0 : v->value;
    if (item.kind == Kind::VoltageSource) {
      rhs(extra_[k], column, amount);
    } else {  // the current leaves node a and enters node b
      rhs(v->nodes[0], column, -amount);
      rhs(v->nodes[1], column, amount);
    }
  }
  for (int r = 0; r < static_cast<int>(reactive_.size()); ++r) {
    if (!present(r)) continue;
    const int index = reactive_[static_cast<size_t>(r)];
    const Item& item = c.items[static_cast<size_t>(index)];
    const Variant& v = *item.variant(switches);
    for (int t = 0; t < ns; ++t) {
      const double e = expansion(r, t);
      if (e == 0.0) continue;
      const double s = e * companion(r);
      if (item.kind == Kind::Inductor) {
        rhs(extra_[static_cast<size_t>(index)], colState + t, -s);
      } else {
        rhs(v.nodes[0], colState + t, s);
        rhs(v.nodes[1], colState + t, -s);
      }
    }
  }
  for (int p = 0; p < np; ++p) {  // device current flows from plus to minus inside the device
    rhs(ports_[static_cast<size_t>(p)].plus, colPort + p, -1.0);
    rhs(ports_[static_cast<size_t>(p)].minus, colPort + p, 1.0);
  }
  for (int k = 0; k < nv; ++k) {
    const Variant* v = c.items[static_cast<size_t>(variableItems_[static_cast<size_t>(k)])].variant(switches);
    m.baseConductance[static_cast<size_t>(k)] = conductance(k, controls);
    if (!v) continue;
    rhs(v->nodes[0], colVariable + k, 1.0);
    rhs(v->nodes[1], colVariable + k, -1.0);
  }
  std::copy(X, X + n * columns, rhs_.begin());
  luSolve(G_.data(), pivot_.data(), n, X, columns, columns);
  luRefine(matrix_.data(), G_.data(), pivot_.data(), n, X, rhs_.data(), correction_.data(), columns);

  // factor (x[a] - x[b]) - offset, taking the differences in both parts before they are added
  const double* low = correction_.data();
  const auto difference = [X, low, columns](int a, int b, int column, double factor = 1.0, double offset = 0.0) {
    const double high = (a == kGround ? 0.0 : X[a * columns + column]) - (b == kGround ? 0.0 : X[b * columns + column]);
    const double rest = (a == kGround ? 0.0 : low[a * columns + column]) - (b == kGround ? 0.0 : low[b * columns + column]);
    return (factor * high - offset) + factor * rest;
  };

  // Rows seen through an incidence (a, b): x[a] - x[b].
  for (int p = 0; p < np; ++p) {
    const int a = ports_[static_cast<size_t>(p)].plus, b = ports_[static_cast<size_t>(p)].minus, stride = m.portRows;
    const auto row = [&](int column) { return difference(a, b, column); };
    m.Pc[static_cast<size_t>(p)] = row(colConst);
    for (int k = 0; k < nin; ++k) m.Pu[at(p, k, stride)] = row(k);
    for (int k = 0; k < ns; ++k) m.Pw[at(p, k, stride)] = row(colState + k);
    for (int k = 0; k < np; ++k) m.K[at(p, k, stride)] = row(colPort + k);
    for (int k = 0; k < nv; ++k) m.CP[at(p, k, stride)] = row(colVariable + k);
  }

  const int stride = m.outRows;
  if (ns + m.outputs < stride) {  // the padding row
    const int r = ns + m.outputs;
    m.Wc[static_cast<size_t>(r)] = 0.0;
    for (int k = 0; k < ns; ++k) m.WS[at(r, k, stride)] = 0.0;
    for (int k = 0; k < nin + np; ++k) m.WL[at(r, k, stride)] = 0.0;
    for (int k = 0; k < nv; ++k) m.CW[at(r, k, stride)] = 0.0;
  }
  const auto outRow = [&](int r, int a, int b, double factor, int ownState) {
    const auto row = [&](int column) { return difference(a, b, column, factor); };
    m.Wc[static_cast<size_t>(r)] = row(colConst);
    for (int k = 0; k < ns; ++k)
      m.WS[at(r, k, stride)] = difference(a, b, colState + k, factor, k == ownState ? 1.0 : 0.0);
    for (int k = 0; k < nin; ++k) m.WL[at(r, k, stride)] = row(k);
    for (int k = 0; k < np; ++k) m.WL[at(r, nin + k, stride)] = row(colPort + k);
    for (int k = 0; k < nv; ++k) m.CW[at(r, k, stride)] = row(colVariable + k);
  };
  for (int t = 0; t < ns; ++t) {  // w_next = 2 q - w
    const int index = reactive_[static_cast<size_t>(stateReactive(t))];
    const Item& item = c.items[static_cast<size_t>(index)];
    if (item.kind == Kind::Inductor) {
      outRow(t, extra_[static_cast<size_t>(index)], kGround, 2.0, t);
    } else {
      const Variant& v = *item.variant(switches);
      outRow(t, v.nodes[0], v.nodes[1], 2.0, t);
    }
  }
  for (int o = 0; o < m.outputs; ++o) {
    const circuit::Output& out = c.outputs[static_cast<size_t>(o)];
    if (out.branch < 0) {
      outRow(ns + o, out.plus, out.minus, 1.0, -1);
      continue;
    }
    const int j = extra_[static_cast<size_t>(out.branch)];
    if (out.gain == 0.0) {
      outRow(ns + o, j, kGround, 1.0, -1);
      continue;
    }
    // branch current + gain * (v(plus) - v(minus)), each part taken in both precisions first
    const int r = ns + o;
    const auto both = [&](int column) {
      return difference(j, kGround, column) + out.gain * difference(out.plus, out.minus, column);
    };
    m.Wc[static_cast<size_t>(r)] = both(colConst);
    for (int k = 0; k < ns; ++k) m.WS[at(r, k, stride)] = both(colState + k);
    for (int k = 0; k < nin; ++k) m.WL[at(r, k, stride)] = both(k);
    for (int k = 0; k < np; ++k) m.WL[at(r, nin + k, stride)] = both(colPort + k);
    for (int k = 0; k < nv; ++k) m.CW[at(r, k, stride)] = both(colVariable + k);
  }

  for (int k = 0; k < nv; ++k) {
    const Variant* v = c.items[static_cast<size_t>(variableItems_[static_cast<size_t>(k)])].variant(switches);
    const int a = v ? v->nodes[0] : kGround, b = v ? v->nodes[1] : kGround;
    const auto row = [&](int column) { return difference(a, b, column); };
    m.Rc[static_cast<size_t>(k)] = row(colConst);
    for (int t = 0; t < ns; ++t) m.RS[at(k, t, nv)] = row(colState + t);
    for (int j = 0; j < nin; ++j) m.RL[at(k, j, nv)] = row(j);
    for (int j = 0; j < np; ++j) m.RL[at(k, nin + j, nv)] = row(colPort + j);
    for (int j = 0; j < nv; ++j) m.Q[at(k, j, nv)] = row(colVariable + j);
  }
  return true;
}

}  // namespace magmora::solver
