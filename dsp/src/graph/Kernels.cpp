// The kernel instances. A cluster with a device mix that is not listed runs on the dynamic-shape kernel: same
// results, slower. Adding a line here is all a new mix needs.
#include <algorithm>
#include <cfloat>
#include <cmath>

#include "graph/Cluster.h"
#include "solver/Newton.h"

namespace magmora::graph {
namespace {

// A Jacobian row is left out of the Newton step when |row| times its largest K entry is below this.
constexpr double kDropThreshold = 1e-4;
constexpr int kContinuationSteps = 8;
// Adaptive continuation (bench recipe 7). The attempt budget bounds what one sample can cost the audio thread.
constexpr double kContinuationFirstStep = 0.25;
constexpr double kContinuationSmallestStep = 1.0 / 1024.0;
constexpr int kContinuationIterations = 20;
constexpr int kContinuationAttempts = 64;
constexpr int kDegradedIterations = 8;

template <class Shape, template <class> class DeviceLaws>
class Kernel final : public Cluster {
 public:
  Kernel(std::unique_ptr<solver::ClusterSystem> system, const SolverSettings& settings, Shape shape)
      : Cluster(std::move(system), settings), shape_(shape) {
    newton_.bind(shape_, system_->devices(), K_.data(), ul_.data() + system_->matrices.inputs, kDropThreshold);
  }

  void step() override {
    const solver::ClusterMatrices& m = system_->matrices;
    double* ul = ul_.data();
    const int inputs = m.inputs;
    for (int k = 0; k < inputs; ++k) {
      const InputSource& s = sources[static_cast<size_t>(k)];
      ul[k] = s.scale * (s.now * *s.value + s.before * *s.previous);
    }
    const bool moving = active_ > 0;
    if (moving) lowRankInputs();

    if constexpr (Shape::kMaxPorts > 0) {
      double* p = p_.data();
      if constexpr (Shape::kFixed) mulAdd<kPairs>(m.Pu.data(), 2 * kPairs, inputs, ul, ps_.data(), p);
      else portKernel_(m.Pu.data(), m.portRows, inputs, ul, ps_.data(), p);
      if (moving) subtractLowRank(m.CP.data(), m.portRows, p);
      const bool funded = !budget || budget->perSample == 0.0 || budget->tokens > 0.0;
      const solver::NewtonOptions options{settings_.tolerance, funded ? settings_.maxIterations : kDegradedIterations,
                                          settings_.fastAccept};
      const long before = newton_.counters.evaluations;
      solveStart_ = before;
      const int it = newton_.solve(p, options, true);
      if (it < 0) {
        const long failed = newton_.counters.evaluations;
        if (funded) recover();
        else keepLastIterate();
        stats_.failedEvaluations += failed - before;
        const long spent = newton_.counters.evaluations - failed;
        stats_.recoveryEvaluations += spent;
        stats_.worstRecovery = std::max(stats_.worstRecovery, failed - before + spent);
      } else {
        newton_.counters.iterations += it;
      }
      // a degraded solve is already bounded; charging it too would keep the bucket empty and the solver degraded
      if (budget && it < 0 && funded) budget->tokens -= static_cast<double>(newton_.counters.evaluations - before);
      for (int a = 0; a < rows(); ++a) pLast_[static_cast<size_t>(a)] = p[a];
    }
    advance(moving);
  }

 private:
  static constexpr int kPairs = padded(Shape::kMaxPorts) / 2;
  long solveStart_ = 0;  // device evaluations before this sample's solve

  int rows() const { return Shape::kFixed ? 2 * kPairs : system_->matrices.portRows; }

  MAGMORA_NOINLINE void lowRankInputs() {
    const solver::ClusterMatrices& m = system_->matrices;
    for (int k = 0; k < m.variables; ++k) {
      double sum = rs_[static_cast<size_t>(k)];
      for (int j = 0; j < m.inputs; ++j) sum += m.RL[static_cast<size_t>(j * m.variables + k)] * ul_[static_cast<size_t>(j)];
      r_[static_cast<size_t>(k)] = sum;
    }
  }

  MAGMORA_NOINLINE void lowRankOutputs() {
    const solver::ClusterMatrices& m = system_->matrices;
    for (int k = 0; k < m.variables; ++k) {
      double sum = r_[static_cast<size_t>(k)];
      for (int j = m.inputs; j < m.inputs + m.ports; ++j)
        sum += m.RL[static_cast<size_t>(j * m.variables + k)] * ul_[static_cast<size_t>(j)];
      r_[static_cast<size_t>(k)] = sum;
    }
    subtractLowRank(m.CW.data(), m.outRows, o_.data());
  }

  // The budget is spent: the previous sample's solution, with its currents, and a fallback on the meter. (Keeping the
  // last iterate instead was measured worse: the next samples start from a bad point and cost more.)
  MAGMORA_NOINLINE void keepLastIterate() {
    newton_.counters.iterations += kDegradedIterations;
    ++stats_.fallbacks;
    ++stats_.degraded;
    std::copy(vPrevious_.begin(), vPrevious_.end(), newton_.v);
    newton_.eval(newton_.v);
  }

  MAGMORA_NOINLINE void recover() {
    newton_.counters.iterations += settings_.maxIterations;
    ++stats_.retries;
    std::copy(vPrevious_.begin(), vPrevious_.end(), newton_.v);
    const bool converged = settings_.fallback == Fallback::Adaptive ? continueAdaptively() : retryThenWalk();
    if (!converged) ++stats_.fallbacks;
  }

  void excitationBetween(double fraction, double* out) const {
    for (int a = 0; a < rows(); ++a) {
      const double last = pLast_[static_cast<size_t>(a)];
      out[a] = last + (p_[static_cast<size_t>(a)] - last) * fraction;
    }
  }

  // Walk p from the last sample's value to this one's; a substep that fails is halved, one that converges doubled.
  bool continueAdaptively() {
    solver::NewtonOptions options{settings_.tolerance, kContinuationIterations, settings_.fastAccept};
    double between[solver::DynamicShape::kMaxPorts + 1], accepted[solver::DynamicShape::kMaxPorts + 1];
    double done = 0.0, step = kContinuationFirstStep;
    for (int attempt = 0; attempt < kContinuationAttempts; ++attempt) {
      if (budgetSpent()) {  // what the bucket held is spent: the previous sample's solution, as a degraded solve has it
        std::copy(vPrevious_.begin(), vPrevious_.end(), newton_.v);
        newton_.eval(newton_.v);
        return false;
      }
      const double next = std::min(1.0, done + step);
      excitationBetween(next, between);
      std::copy(newton_.v, newton_.v + rows(), accepted);
      const int used = newton_.solve(between, options, false);
      newton_.counters.iterations += used < 0 ? options.maxIterations : used;
      if (used < 0) {
        std::copy(accepted, accepted + rows(), newton_.v);
        step *= 0.5;
        if (step < kContinuationSmallestStep) break;
      } else {
        done = next;
        step *= 2.0;
        if (done >= 1.0) return true;
      }
    }
    std::copy(vPrevious_.begin(), vPrevious_.end(), newton_.v);
    options.maxIterations = settings_.maxIterations;
    newton_.solve(p_.data(), options, false);  // keep the last accepted iterate
    return false;
  }

  // The recovery budget is checked before every continuation attempt, not only on entry, so that one failed solve
  // and its recovery cost at most what the bucket held plus one attempt.
  bool budgetSpent() const {
    return budget && budget->perSample != 0.0 &&
           static_cast<double>(newton_.counters.evaluations - solveStart_) >= budget->tokens;
  }

  // Rule 6 as the prototype has it.
  bool retryThenWalk() {
    solver::NewtonOptions options{settings_.tolerance, 4 * settings_.maxIterations, settings_.fastAccept};
    const int it = newton_.solve(p_.data(), options, false);
    if (it >= 0) {
      newton_.counters.iterations += it;
      return true;
    }
    std::copy(vPrevious_.begin(), vPrevious_.end(), newton_.v);
    options.maxIterations = 2 * settings_.maxIterations;
    double between[solver::DynamicShape::kMaxPorts + 1];
    bool converged = true;
    for (int k = 1; k <= kContinuationSteps; ++k) {
      excitationBetween(k / static_cast<double>(kContinuationSteps), between);
      const int used = newton_.solve(between, options, false);
      newton_.counters.iterations += used < 0 ? options.maxIterations : used;
      converged = converged && used >= 0;
    }
    return converged;
  }

  MAGMORA_INLINE void advance(bool moving) {
    const solver::ClusterMatrices& m = system_->matrices;
    double* o = o_.data();
    outKernel_(m.WL.data(), m.outRows, m.inputs + m.ports, ul_.data(), os_.data(), o);
    if (moving) lowRankOutputs();

    const int ns = m.states;
    double sum = 0.0;
    for (int k = 0; k < m.outputs; ++k) {
      yPrevious_[static_cast<size_t>(k)] = y_[static_cast<size_t>(k)];
      y_[static_cast<size_t>(k)] = o[ns + k];
      sum += o[ns + k];
    }
    // A bad state shows in an output or a port voltage by the next sample, before anything downstream reads it.
    if constexpr (Shape::kMaxPorts > 0)
      for (int a = 0; a < rows(); ++a) sum += newton_.v[a];  // some laws would hide a NaN port voltage
    if (!(std::fabs(sum) <= DBL_MAX)) {
      ++stats_.nonFinite;
      restoreOperatingPoint();
      return;
    }
    prepareNext();
  }

  MAGMORA_INLINE void prepareNext() {
    const solver::ClusterMatrices& m = system_->matrices;
    if constexpr (Shape::kFixed) mulAdd<kPairs>(m.Pw.data(), 2 * kPairs, m.states, o_.data(), m.Pc.data(), ps_.data());
    else portKernel_(m.Pw.data(), m.portRows, m.states, o_.data(), m.Pc.data(), ps_.data());
    outKernel_(m.WS.data(), m.outRows, m.states, o_.data(), m.Wc.data(), os_.data());
    if (active_ > 0) updateLowRankResidual();
    if (hasCompanion()) updateCompanion();
    if constexpr (Shape::kMaxPorts > 0) {
      for (int a = 0; a < rows(); ++a) {  // predictor: 2 v[n-1] - v[n-2]
        const double guess = 2.0 * newton_.v[a] - vPrevious_[static_cast<size_t>(a)];
        vPrevious_[static_cast<size_t>(a)] = newton_.v[a];
        newton_.v[a] = guess;
      }
      newton_.prime();
    }
  }

  double* newtonVoltages() override { return newton_.v; }

  void matricesChanged(bool currentsStillValid) override {
    if constexpr (Shape::kMaxPorts > 0) {
      newton_.updateK();
      if (currentsStillValid) newton_.reprime();
      else newton_.prime();
    } else {
      (void)currentsStillValid;
    }
  }

  void restart() override { matricesChanged(false); }

  void counters(ClusterStats& s) const override {
    s.iterations = newton_.counters.iterations;
    s.evaluations = newton_.counters.evaluations;
    s.factorisations = newton_.counters.factorisations;
  }

  Shape shape_;
  solver::Newton<Shape, DeviceLaws> newton_;
};

template <template <class> class DeviceLaws, class... Shapes>
struct Factory;

template <template <class> class DeviceLaws>
struct Factory<DeviceLaws> {
  static std::unique_ptr<Cluster> make(std::unique_ptr<solver::ClusterSystem>& system, const SolverSettings& settings,
                                       int t, int p, int d) {
    using Shape = solver::DynamicShape;
    if (!Shape::fits(t, p, d)) return nullptr;
    return std::unique_ptr<Cluster>(new Kernel<Shape, DeviceLaws>(std::move(system), settings, Shape(t, p, d)));
  }
};

template <template <class> class DeviceLaws, class First, class... Rest>
struct Factory<DeviceLaws, First, Rest...> {
  static std::unique_ptr<Cluster> make(std::unique_ptr<solver::ClusterSystem>& system, const SolverSettings& settings,
                                       int t, int p, int d) {
    if (t == First::kMaxTriodes && p == First::kMaxPentodes && d == First::kMaxDiodes)
      return std::unique_ptr<Cluster>(new Kernel<First, DeviceLaws>(std::move(system), settings, First()));
    return Factory<DeviceLaws, Rest...>::make(system, settings, t, p, d);
  }
};

template <template <class> class DeviceLaws>
using Mixes = Factory<DeviceLaws, solver::FixedShape<0, 0, 0>, solver::FixedShape<1, 0, 0>, solver::FixedShape<2, 0, 0>,
                      solver::FixedShape<3, 0, 0>, solver::FixedShape<2, 2, 0>>;

}  // namespace

std::unique_ptr<Cluster> Cluster::create(std::unique_ptr<solver::ClusterSystem> system, Laws laws,
                                         const SolverSettings& settings) {
  const devices::DeviceSet& set = system->devices();
  const int t = static_cast<int>(set.triodes.size()), p = static_cast<int>(set.pentodes.size());
  const int d = static_cast<int>(set.diodes.size());
  return laws == Laws::Closed ? Mixes<devices::ClosedLaws>::make(system, settings, t, p, d)
                              : Mixes<devices::TableLaws>::make(system, settings, t, p, d);
}

}  // namespace magmora::graph
