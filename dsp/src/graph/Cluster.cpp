#include "graph/Cluster.h"

#include <algorithm>

#include "solver/DenseLU.h"

namespace magmora::graph {

Cluster::Cluster(std::unique_ptr<solver::ClusterSystem> system, const SolverSettings& settings)
    : system_(std::move(system)), settings_(settings) {
  const solver::ClusterMatrices& m = system_->matrices;
  const size_t maxStates = system_->reactiveItems().size();
  const size_t ports = static_cast<size_t>(m.portRows), variables = static_cast<size_t>(m.variables);
  const size_t outRows = static_cast<size_t>(padded(static_cast<int>(maxStates) + m.outputs));
  sources.resize(static_cast<size_t>(m.inputs));
  
  y_.assign(static_cast<size_t>(m.outputs), 0.0);
  yPrevious_.assign(y_.size(), 0.0);
  y0_.assign(y_.size(), 0.0);
  u0_.assign(static_cast<size_t>(m.inputs), 0.0);
  ul_.assign(static_cast<size_t>(m.inputs) + ports, 0.0);
  for (auto* v : {&v0_, &ps_, &p_, &pLast_, &vPrevious_}) v->assign(ports, 0.0);
  os_.assign(outRows, 0.0);
  o_.assign(outRows, 0.0);
  K_ = m.K;
  for (auto* v : {&rs_, &r_, &t_, &delta_}) v->assign(variables, 0.0);
  phi_.assign(variables * variables, 0.0);
  M_.assign(variables * variables, 0.0);
  activeIndex_.assign(variables, 0);
  knobSeen_.assign(variables, 0.0);
  pivot_.assign(variables, 0);
  element_.assign(maxStates, 0.0);
  wasPresent_.assign(maxStates, 0);
  companionCurrents_.assign(ports, 0.0);
  portKernel_ = selectMulAdd(m.portRows);
  outKernel_ = selectMulAdd(m.outRows);
}

ClusterStats Cluster::stats() const {
  ClusterStats s = stats_;
  counters(s);
  return s;
}

void Cluster::setOperatingPoint(const solver::OperatingPoint& op, const std::vector<double>& inputValues) {
  x0_ = op.x;
  v0_ = op.v;
  u0_ = inputValues;
  for (size_t k = 0; k < y0_.size(); ++k) y0_[k] = system_->outputValue(static_cast<int>(k), x0_.data());
}

void Cluster::setCompanionOutput(int output, int cutInput, double tubeConductance, bool hold) {
  companionRow_ = output;
  companionCut_ = cutInput;
  companionTubes_ = tubeConductance;
  companionHold_ = hold;
}

// The remainder i_res - g v of the cut this cluster is driven from (companion coupling), for the upstream cluster's next
// sample: this sample's state, inputs and final tube currents, plus a first-order hold on the tubes' departure from
// their operating-point slopes. Called after the state advanced and before the predictor overwrites the currents.
void Cluster::updateCompanion() {
  const solver::ClusterMatrices& m = system_->matrices;
  const int row = m.states + companionRow_, stride = m.outRows, inputs = m.inputs;
  const double* i = ul_.data() + inputs;
  double value = os_[static_cast<size_t>(row)];
  for (int j = 0; j < inputs + m.ports; ++j) value += m.WL[static_cast<size_t>(j * stride + row)] * ul_[static_cast<size_t>(j)];
  if (active_ > 0) {
    for (int k = 0; k < m.variables; ++k) {
      double sum = rs_[static_cast<size_t>(k)];
      for (int j = 0; j < inputs + m.ports; ++j) sum += m.RL[static_cast<size_t>(j * m.variables + k)] * ul_[static_cast<size_t>(j)];
      r_[static_cast<size_t>(k)] = sum;
    }
    for (int a = 0; a < active_; ++a) {
      double t = 0.0;
      for (int b = 0; b < active_; ++b)
        t += phi_[static_cast<size_t>(a * active_ + b)] * r_[static_cast<size_t>(activeIndex_[static_cast<size_t>(b)])];
      value -= m.CW[static_cast<size_t>(activeIndex_[static_cast<size_t>(a)] * stride + row)] * t;
    }
  }
  const double cut = ul_[static_cast<size_t>(companionCut_)];
  if (companionHold_) {
    double departure = companionTubes_ * (cut - companionCutBefore_);
    for (int j = 0; j < m.ports; ++j)
      departure += m.WL[static_cast<size_t>((inputs + j) * stride + row)] * (i[j] - companionCurrents_[static_cast<size_t>(j)]);
    if (active_ > 0) {  // with the row of the moved matrices, exactly as a rebuild would have it
      for (int k = 0; k < m.variables; ++k) {
        double sum = 0.0;
        for (int j = 0; j < m.ports; ++j)
          sum += m.RL[static_cast<size_t>((inputs + j) * m.variables + k)] * (i[j] - companionCurrents_[static_cast<size_t>(j)]);
        r_[static_cast<size_t>(k)] = sum;
      }
      for (int a = 0; a < active_; ++a) {
        double t = 0.0;
        for (int c = 0; c < active_; ++c)
          t += phi_[static_cast<size_t>(a * active_ + c)] * r_[static_cast<size_t>(activeIndex_[static_cast<size_t>(c)])];
        departure -= m.CW[static_cast<size_t>(activeIndex_[static_cast<size_t>(a)] * stride + row)] * t;
      }
    }
    value += departure;
  }
  for (int j = 0; j < m.ports; ++j) companionCurrents_[static_cast<size_t>(j)] = i[j];
  companionCutBefore_ = cut;
  companion_ = value;
}

void Cluster::rebuild(const circuit::Controls& controls) {
  solver::ClusterSystem& s = *system_;
  solver::ClusterMatrices& m = s.matrices;
  const int count = static_cast<int>(s.reactiveItems().size());
  const bool remap = built_ && controls.switches != switches_;
  if (remap) {
    for (int r = 0; r < count; ++r) {
      wasPresent_[static_cast<size_t>(r)] = s.present(r);
      double sum = 0.0;
      for (int t = 0; t < m.states; ++t) sum += s.expansion(r, t) * o_[static_cast<size_t>(t)];
      element_[static_cast<size_t>(r)] = sum;
    }
  }
  if (!s.build(controls)) return;  // cannot happen for a circuit whose operating point was found
  ++rebuilds_;
  if (remap) {
    for (int t = 0; t < m.states; ++t) {
      const int r = s.stateReactive(t);
      o_[static_cast<size_t>(t)] = wasPresent_[static_cast<size_t>(r)]
                                       ? element_[static_cast<size_t>(r)]
                                       : s.reactiveValue(r, controls.switches, x0_.data());  // switched in discharged
    }
  }
  switches_ = controls.switches;
  std::fill(delta_.begin(), delta_.end(), 0.0);
  for (int k = 0; k < m.variables; ++k)
    knobSeen_[static_cast<size_t>(k)] = controls.knobs[static_cast<size_t>(s.variableKnob(k))];
  outKernel_ = selectMulAdd(m.outRows);
  active_ = 0;
  std::copy(m.K.begin(), m.K.end(), K_.begin());
  if (built_) {
    linearPart();
    matricesChanged(true);
  }
}

void Cluster::moveKnobs(const circuit::Controls& controls) {
  const solver::ClusterMatrices& m = system_->matrices;
  const int nv = m.variables;
  if (nv == 0) return;
  bool changed = false;
  for (int k = 0; k < nv; ++k) {
    const double knob = controls.knobs[static_cast<size_t>(system_->variableKnob(k))];
    if (knob == knobSeen_[static_cast<size_t>(k)]) continue;  // the taper costs a pow()
    knobSeen_[static_cast<size_t>(k)] = knob;
    delta_[static_cast<size_t>(k)] = system_->conductance(k, controls) - m.baseConductance[static_cast<size_t>(k)];
    changed = true;
  }
  if (!changed) return;
  int active = 0;
  for (int k = 0; k < nv; ++k)
    if (delta_[static_cast<size_t>(k)] != 0.0) activeIndex_[static_cast<size_t>(active++)] = k;
  active_ = active;
  if (active > 0) {  // Phi = (I + dG Q)^-1 dG on the conductances that differ from the base
    std::fill(phi_.begin(), phi_.begin() + active * active, 0.0);
    for (int a = 0; a < active; ++a) {
      const int va = activeIndex_[static_cast<size_t>(a)];
      const double d = delta_[static_cast<size_t>(va)];
      for (int b = 0; b < active; ++b)
        M_[static_cast<size_t>(a * active + b)] =
            (a == b ? 1.0 : 0.0) + d * m.Q[static_cast<size_t>(activeIndex_[static_cast<size_t>(b)] * nv + va)];
      phi_[static_cast<size_t>(a * active + a)] = d;
    }
    solver::luFactor(M_.data(), pivot_.data(), active);  // regular: every total conductance stays positive
    solver::luSolve(M_.data(), pivot_.data(), active, phi_.data(), active, active);
  }
  updateK();
  updateLowRankResidual();
  matricesChanged(true);
}

// K' = K - CP Phi RL(i)
void Cluster::updateK() {
  const solver::ClusterMatrices& m = system_->matrices;
  std::copy(m.K.begin(), m.K.end(), K_.begin());
  const int nv = m.variables;
  for (int port = 0; port < m.ports; ++port) {
    const double* row = m.RL.data() + (m.inputs + port) * nv;
    double* column = K_.data() + port * m.portRows;
    for (int a = 0; a < active_; ++a) {
      double t = 0.0;
      for (int b = 0; b < active_; ++b) t += phi_[static_cast<size_t>(a * active_ + b)] * row[activeIndex_[static_cast<size_t>(b)]];
      const double* c = m.CP.data() + activeIndex_[static_cast<size_t>(a)] * m.portRows;
      for (int r = 0; r < m.portRows; ++r) column[r] -= c[r] * t;
    }
  }
}

void Cluster::updateLowRankResidual() {
  const solver::ClusterMatrices& m = system_->matrices;
  for (int a = 0; a < active_; ++a) {
    const int k = activeIndex_[static_cast<size_t>(a)];
    double sum = m.Rc[static_cast<size_t>(k)];
    for (int t = 0; t < m.states; ++t) sum += m.RS[static_cast<size_t>(t * m.variables + k)] * o_[static_cast<size_t>(t)];
    rs_[static_cast<size_t>(k)] = sum;
  }
}

// r holds d' x0 for the active conductances: out -= C Phi r
void Cluster::subtractLowRank(const double* C, int stride, double* out) const {
  for (int a = 0; a < active_; ++a) {
    double t = 0.0;
    for (int b = 0; b < active_; ++b)
      t += phi_[static_cast<size_t>(a * active_ + b)] * r_[static_cast<size_t>(activeIndex_[static_cast<size_t>(b)])];
    const double* c = C + activeIndex_[static_cast<size_t>(a)] * stride;
    for (int r = 0; r < stride; ++r) out[r] -= c[r] * t;
  }
}

void Cluster::linearPart() {
  const solver::ClusterMatrices& m = system_->matrices;
  portKernel_(m.Pw.data(), m.portRows, m.states, o_.data(), m.Pc.data(), ps_.data());
  outKernel_(m.WS.data(), m.outRows, m.states, o_.data(), m.Wc.data(), os_.data());
  if (active_ > 0) updateLowRankResidual();
}

void Cluster::restoreOperatingPoint() {
  const solver::ClusterMatrices& m = system_->matrices;
  for (int t = 0; t < m.states; ++t)
    o_[static_cast<size_t>(t)] = system_->reactiveValue(system_->stateReactive(t), switches_, x0_.data());
  std::copy(u0_.begin(), u0_.end(), ul_.begin());
  std::copy(v0_.begin(), v0_.end(), newtonVoltages());
  std::copy(v0_.begin(), v0_.end(), vPrevious_.begin());
  std::copy(y0_.begin(), y0_.end(), y_.begin());
  std::copy(y0_.begin(), y0_.end(), yPrevious_.begin());
  linearPart();
  portKernel_(m.Pu.data(), m.portRows, m.inputs, u0_.data(), ps_.data(), pLast_.data());
  restart();
  if (companionRow_ >= 0) {  // at the operating point the remainder is its DC value and nothing departs
    companion_ = y0_[static_cast<size_t>(companionRow_)];
    companionCutBefore_ = u0_[static_cast<size_t>(companionCut_)];
    for (int j = 0; j < m.ports; ++j)
      companionCurrents_[static_cast<size_t>(j)] = ul_[static_cast<size_t>(m.inputs + j)];
  }
}

void Cluster::reset(const circuit::Controls& controls) {
  built_ = false;  // nothing to carry over
  rebuild(controls);
  built_ = true;
  restoreOperatingPoint();
}

}  // namespace magmora::graph
