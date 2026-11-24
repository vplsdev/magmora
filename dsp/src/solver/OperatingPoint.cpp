#include "solver/OperatingPoint.h"

#include <algorithm>
#include <memory>

#include "solver/DenseLU.h"
#include "solver/Newton.h"

namespace magmora::solver {
namespace {

constexpr int kInitialSteps = 20;
constexpr double kTolerance = 1e-9;
constexpr int kIterations = 100;
constexpr double kSmallestStep = 1e-6;
constexpr double kLargestStep = 0.25;
constexpr int kEasyIterations = 12;

template <template <class> class Laws>
bool stepSources(const ClusterSystem& system, const std::vector<double>& K, const std::vector<double>& pFull,
                 std::vector<double>& v, std::vector<double>& i) {
  const devices::DeviceSet& set = system.devices();
  const int t = static_cast<int>(set.triodes.size()), p = static_cast<int>(set.pentodes.size());
  const int d = static_cast<int>(set.diodes.size());
  if (!DynamicShape::fits(t, p, d)) return false;
  const DynamicShape shape(t, p, d);
  const auto newton = std::make_unique<Newton<DynamicShape, Laws>>();
  newton->bind(shape, set, K.data(), i.data(), 0.0);
  const NewtonOptions options{kTolerance, kIterations, false};
  std::vector<double> scaled(pFull.size());
  double factor = 0.0, step = 1.0 / kInitialSteps;
  while (factor < 1.0) {
    const double next = std::min(1.0, factor + step);
    for (size_t k = 0; k < scaled.size(); ++k) scaled[k] = pFull[k] * next;
    std::copy(v.begin(), v.end(), newton->v);
    const int it = newton->solve(scaled.data(), options, false);
    if (it < 0) {
      step *= 0.5;
      if (step < kSmallestStep) return false;
      continue;
    }
    std::copy(newton->v, newton->v + v.size(), v.begin());
    factor = next;
    if (it < kEasyIterations) step = std::min(step * 1.5, kLargestStep);
  }
  newton->eval(v.data());
  return true;
}

}  // namespace

bool solveOperatingPoint(const ClusterSystem& system, const circuit::Controls& controls,
                         const std::vector<double>& inputValues, DcOptions options, OperatingPoint& out) {
  using circuit::kGround;
  using circuit::Kind;
  const auto& c = system.circuit();
  const int n = system.unknowns(), np = system.matrices.ports, rows = padded(np), columns = 1 + np;
  std::vector<double> G(static_cast<size_t>(n * n)), X(static_cast<size_t>(n * columns));
  std::vector<int> pivot(static_cast<size_t>(n));
  system.assemble(controls, {true, options.capLeak}, G.data());
  if (!luFactor(G.data(), pivot.data(), n)) return false;

  const auto rhs = [&X, columns](int row, int column, double value) {
    if (row != kGround) X[static_cast<size_t>(row * columns + column)] += value;
  };
  size_t input = 0;
  for (size_t k = 0; k < c.items.size(); ++k) {
    const circuit::Item& item = c.items[k];
    if (item.kind != Kind::VoltageSource && item.kind != Kind::CurrentSource) continue;
    double value = item.input ? inputValues.at(input++) : 0.0;
    const circuit::Variant* v = item.variant(controls.switches);
    if (!v) continue;
    if (!item.input) value = v->value;
    if (item.kind == Kind::VoltageSource) {
      rhs(system.branch(static_cast<int>(k)), 0, value);
    } else {
      rhs(v->nodes[0], 0, -value);
      rhs(v->nodes[1], 0, value);
    }
  }
  for (int p = 0; p < np; ++p) {
    rhs(system.ports()[static_cast<size_t>(p)].plus, 1 + p, -1.0);
    rhs(system.ports()[static_cast<size_t>(p)].minus, 1 + p, 1.0);
  }
  luSolve(G.data(), pivot.data(), n, X.data(), columns, columns);

  const auto x = [&X, columns](int row, int column) {
    return row == kGround ? 0.0 : X[static_cast<size_t>(row * columns + column)];
  };
  out.v.assign(static_cast<size_t>(rows), 0.0);
  std::vector<double> i(static_cast<size_t>(rows));
  if (np > 0) {
    std::vector<double> K(static_cast<size_t>(rows * np)), p(static_cast<size_t>(rows));
    for (int a = 0; a < np; ++a) {
      const Port port = system.ports()[static_cast<size_t>(a)];
      p[static_cast<size_t>(a)] = x(port.plus, 0) - x(port.minus, 0);
      for (int b = 0; b < np; ++b) K[static_cast<size_t>(b * rows + a)] = x(port.plus, 1 + b) - x(port.minus, 1 + b);
    }
    const bool solved = options.closedForm ? stepSources<devices::ClosedLaws>(system, K, p, out.v, i)
                                           : stepSources<devices::TableLaws>(system, K, p, out.v, i);
    if (!solved) return false;
  }
  out.x.assign(static_cast<size_t>(n), 0.0);
  for (int r = 0; r < n; ++r) {
    double sum = x(r, 0);
    for (int b = 0; b < np; ++b) sum += x(r, 1 + b) * i[static_cast<size_t>(b)];
    out.x[static_cast<size_t>(r)] = sum;
  }
  return true;
}

}  // namespace magmora::solver
