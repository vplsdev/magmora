// DC operating point by adaptive source stepping (proto/blockamp/dk.py: dc_op). Allocates: prepare() only.
#pragma once
#include <vector>

#include "solver/ClusterSystem.h"

namespace magmora::solver {

struct OperatingPoint {
  std::vector<double> x;  // all unknowns
  std::vector<double> v;  // port voltages, padded
};

struct DcOptions {
  bool closedForm;
  double capLeak;
};

// `inputValues` holds one value per circuit input.
bool solveOperatingPoint(const ClusterSystem&, const circuit::Controls&, const std::vector<double>& inputValues,
                         DcOptions, OperatingPoint&);

}  // namespace magmora::solver
