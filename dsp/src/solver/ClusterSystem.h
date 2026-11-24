// Nodal K-method discretisation of one circuit (port of proto/blockamp/dk.py: build), arranged for the kernels.
//
// Same equations as the prototype, rearranged without approximation:
//  * a capacitor's state is w = s / g (a voltage), an inductor's w = s / r (a current). These do not change when a
//    resistor moves or a relay switches, so they carry straight across a rebuild.
//  * a capacitor that closes a loop of capacitors (a tube's three, parallel ones) has no state of its own: its w is
//    the signed sum around the loop.
//  * constant sources are folded into constant vectors; only the per-sample inputs stay matrix columns.
//  * a diode reversed across a pentode's plate and cathode is folded into that plate port.
//  * knob resistors keep their Schur-complement terms, so that a moving knob is an exact low-rank update:
//        A' = A - A D Phi D' A,   Phi = (I + dG Q)^-1 dG,   Q = D' A D.
//
// All matrices are column-major, with the port and [state; output] row counts padded to even.
#pragma once
#include <vector>

#include "circuit/Netlist.h"
#include "devices/Tubes.h"

namespace magmora::solver {

struct ClusterMatrices {
  int ports = 0, inputs = 0, outputs = 0, variables = 0;  // fixed by prepare()
  int portRows = 0;
  int states = 0, outRows = 0;  // set by build(): the states depend on which relay variant is in force

  //   p = (Pc + Pw w) + Pu u
  //   [w_next; y] = (Wc + WS w) + WL [u; i]
  std::vector<double> Pc, Pw, Pu, K, Wc, WS, WL;
  // the moving-knob terms:  r = Rc + RS w + RL [u; i],  t = Phi r,  p -= CP t,  [w_next; y] -= CW t
  std::vector<double> CP, CW, Rc, RS, RL, Q;
  std::vector<double> baseConductance;
};

struct Port {
  int plus, minus;
};

struct AssemblyMode {
  bool dc;
  double capLeak;  // conductance that stands in for a capacitor at DC
};

class ClusterSystem {
 public:
  // Allocates. The circuit and the library must outlive this object.
  bool prepare(const circuit::Circuit&, double sampleRate, devices::Library&, bool foldDiodes = true);
  // Recomputes `matrices` in place: no allocation, no locks.
  bool build(const circuit::Controls&);

  // The nodal matrix, row-major [unknowns x unknowns].
  void assemble(const circuit::Controls&, AssemblyMode, double* G) const;
  // Unknown whose value is the branch current of a voltage source or inductor.
  int branch(int item) const { return extra_[static_cast<size_t>(item)]; }
  double value(int item, const circuit::Controls&) const;

  const circuit::Circuit& circuit() const { return *circuit_; }
  const devices::DeviceSet& devices() const { return devices_; }
  int unknowns() const { return unknowns_; }
  const std::vector<Port>& ports() const { return ports_; }
  // Item and terminal pair behind each port (for tests, which match by name).
  const std::vector<int>& portItems() const { return portItems_; }

  const std::vector<int>& variableItems() const { return variableItems_; }
  int variableKnob(int variable) const { return variableKnobs_[static_cast<size_t>(variable)]; }
  double conductance(int variable, const circuit::Controls&) const;

  // Reactive items; `expansion` gives each one's w from the state vector of the current build.
  const std::vector<int>& reactiveItems() const { return reactive_; }
  bool present(int reactive) const { return present_[static_cast<size_t>(reactive)] != 0; }
  double expansion(int reactive, int state) const {
    return expansion_[static_cast<size_t>(reactive) * reactive_.size() + static_cast<size_t>(state)];
  }
  double companion(int reactive) const { return companion_[static_cast<size_t>(reactive)]; }
  int stateReactive(int state) const { return stateReactive_[static_cast<size_t>(state)]; }
  // q = capacitor voltage or inductor current in a solution x, for the nodes in force at `switches`.
  double reactiveValue(int reactive, unsigned switches, const double* x) const;
  // An output of the circuit evaluated on a solution x.
  double outputValue(int output, const double* x) const;

  bool dependsOnSwitches() const { return dependsOnSwitches_; }

  ClusterMatrices matrices;

 private:
  void chooseStates(unsigned switches);

  const circuit::Circuit* circuit_ = nullptr;
  double sampleRate_ = 0.0;
  int unknowns_ = 0;
  bool dependsOnSwitches_ = false;
  std::vector<int> extra_;
  std::vector<Port> ports_;
  std::vector<int> portItems_;
  std::vector<int> reactive_, variableItems_, variableKnobs_;
  devices::DeviceSet devices_;

  // work, sized by prepare()
  std::vector<double> G_, matrix_, X_, rhs_, correction_, expansion_, companion_, pathCoefficient_;
  std::vector<int> pivot_, stateReactive_, root_, queue_, via_;
  std::vector<char> present_;
};

}  // namespace magmora::solver
