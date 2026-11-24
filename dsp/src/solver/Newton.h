// Damped Newton for v = p + K i(v). The reference: proto/blockamp/dk.py: newton.
//
// Cheaper than the prototype, same fixed point (recipe: bench/kernel_fast.cpp):
//  - the step is solved in the space of the device currents (Woodbury): with J = E Jr, where Jr holds the Jacobian
//    rows that matter,  dv = F + K E (I - Jr K E)^-1 Jr F.  Which rows those are:
//      fixed shapes      plates, screens and diodes always, plus a row for each grid that conducts right now. The
//                        size is then known at compile time for each number of conducting grids: straight-line code,
//                        and a dry grid costs nothing.
//      dynamic shape     the rows active right now, found at run time.
//    A row is dropped from the step only, never from the residual.
//  - the small systems are eliminated without pivot search; a pivot below kSmallPivot sends that one factorisation
//    to partial pivoting. Either way only the step is affected, and the monotonicity test guards the step.
//  - when the monotonicity test finds the next correction already below tolerance it is applied and the sample is
//    done, so a sample needs one factorisation.
#pragma once
#include <cmath>
#include <utility>

#include "circuit/Netlist.h"
#include "common/MatVec.h"
#include "devices/Evaluators.h"
#include "solver/Shape.h"

namespace magmora::solver {

struct NewtonOptions {
  double tolerance;
  int maxIterations;
  bool fastAccept;
};

struct NewtonCounters {
  long iterations = 0, evaluations = 0, factorisations = 0;
};

namespace detail {

constexpr double kMinimumLambda = 0.02;

constexpr int atLeast(int n, int least) { return n > least ? n : least; }

constexpr double kSmallPivot = 1e-6;

// Elimination in the natural order. False if a pivot is too small to trust.
template <int R>
MAGMORA_INLINE bool factorUnpivoted(double* A) {
  bool sound = true;
  for (int a = 0; a < R; ++a) {
    const double pivot = A[a * R + a];
    sound &= std::fabs(pivot) >= kSmallPivot;
    const double inverse = 1.0 / pivot;
    A[a * R + a] = inverse;
    for (int b = a + 1; b < R; ++b) {
      const double f = A[b * R + a] * inverse;
      A[b * R + a] = f;
      for (int c = a + 1; c < R; ++c) A[b * R + c] -= f * A[a * R + c];
    }
  }
  return sound;
}

template <int R>
MAGMORA_INLINE void solveUnpivoted(const double* A, double* y) {
  for (int b = 1; b < R; ++b)
    for (int a = 0; a < b; ++a) y[b] -= A[b * R + a] * y[a];
  for (int a = R - 1; a >= 0; --a) {
    double sum = y[a];
    for (int c = a + 1; c < R; ++c) sum -= A[a * R + c] * y[c];
    y[a] = sum * A[a * R + a];
  }
}

void factorAny(double* A, int* pivot, int r);
void solveAny(const double* A, const int* pivot, int r, double* y);

// The run-time sized set: sizes up to eight are dispatched to the fixed code.
template <int R>
MAGMORA_INLINE bool factorUpTo(int r, double* A) {
  if constexpr (R == 0) {
    (void)r, (void)A;
    return true;
  } else {
    return r == R ? factorUnpivoted<R>(A) : factorUpTo<R - 1>(r, A);
  }
}

template <int R>
MAGMORA_INLINE void solveUpTo(int r, const double* A, double* y) {
  if constexpr (R == 0) {
    (void)r, (void)A, (void)y;
  } else {
    if (r == R) solveUnpivoted<R>(A, y);
    else solveUpTo<R - 1>(r, A, y);
  }
}

struct Row {
  int port = 0;    // the current this row gives
  int first = 0;   // first port voltage it depends on
  int width = 0;
  int jacobian = 0;  // its first entry in the Jacobian
  bool grid = false;
};

template <int N>
struct RowTable {
  Row all[N > 0 ? N : 1];
  int count = 0;
  int dry[N > 0 ? N : 1] = {};    // indices of the rows that are not grid currents
  int grids[N > 0 ? N : 1] = {};
  int dryCount = 0, gridCount = 0;

  constexpr RowTable() = default;
  constexpr RowTable(int nt, int np, int nd) {
    for (int d = 0; d < nt; ++d) {
      const int q = 2 * d, j = devices::kTriodeJacobian * d;
      add({q, q, 2, j, false});
      add({q + 1, q + 1, 1, j + 2, true});
    }
    for (int d = 0; d < np; ++d) {
      const int q = 2 * nt + 3 * d, j = devices::kTriodeJacobian * nt + devices::kPentodeJacobian * d;
      add({q, q, 3, j, false});
      add({q + 1, q, 3, j + 3, false});
      add({q + 2, q + 2, 1, j + 6, true});
    }
    for (int d = 0; d < nd; ++d)
      add({2 * nt + 3 * np + d, 2 * nt + 3 * np + d, 1,
           devices::kTriodeJacobian * nt + devices::kPentodeJacobian * np + d, false});
  }

  constexpr void add(Row row) {
    all[count] = row;
    if (row.grid) grids[gridCount++] = count;
    else dry[dryCount++] = count;
    ++count;
  }
};

}  // namespace detail

template <class Shape, template <class> class Laws>
class Newton {
 public:
  static constexpr int kPorts = Shape::kMaxPorts;
  static constexpr int kPairs = padded(kPorts) / 2;
  static constexpr int kStorage = detail::atLeast(padded(kPorts), 2);

  // K is column-major with padded(ports) rows; i is the caller's current vector (padded).
  // dropThreshold: a Jacobian row with |row| max|K column| below it is left out of the step (0 keeps every row).
  void bind(Shape shape, const devices::DeviceSet& set, const double* K, double* i, double dropThreshold) {
    shape_ = shape;
    set_ = &set;
    laws_.bind(set);
    K_ = K;
    i_ = i;
    dropThreshold_ = dropThreshold;
    stride_ = padded(shape.ports());
    if constexpr (!Shape::kFixed) table_ = detail::RowTable<kPorts>(shape.triodes(), shape.pentodes(), shape.diodes());
    updateK();
  }

  // Call after the contents of K changed.
  void updateK() {
    for (int b = 0; b < shape_.ports(); ++b) {
      double big = 0.0;
      for (int a = 0; a < stride_; ++a) big = std::fmax(big, std::fabs(K_[b * stride_ + a]));
      negligible_[b] = dropThreshold_ / big;  // a row below this (NaN: never) stays out of the step
    }
  }

  // What needs only the start point: device evaluation, the residual without p, and the factorisation.
  MAGMORA_INLINE void prime() {
    eval(v);
    reprime();
  }

  // As prime(), but the currents at v are still valid (only K changed).
  MAGMORA_INLINE void reprime() {
    double minusV[kStorage];
    for (int a = 0; a < rows(); ++a) minusV[a] = -v[a];
    addKi(minusV);
    factor();
  }

  // Iterations used, or minus maxIterations.
  MAGMORA_INLINE int solve(const double* p, const NewtonOptions& options, bool primed) {
    if (primed) {
      for (int a = 0; a < rows(); ++a) F_[a] += p[a];
    } else {
      eval(v);
      residual(p, v);
    }
    for (int it = 0; it < options.maxIterations; ++it) {
      if (!primed || it) factor();
      step(F_, dv_);
      const double scale = junctionLimit();
      if (scale < 1.0)
        for (int a = 0; a < rows(); ++a) dv_[a] *= scale;
      const double length = maxAbs(dv_);
      if (length < options.tolerance) {
        acceptFirstOrder(dv_);
        for (int a = 0; a < rows(); ++a) v[a] += dv_[a];
        return it + 1;
      }
      double lambda = 1.0;
      for (;;) {
        for (int a = 0; a < rows(); ++a) vt_[a] = v[a] + lambda * dv_[a];
        eval(vt_);
        residual(p, vt_);
        if (scale < 1.0 || lambda < detail::kMinimumLambda) break;  // take it: the test would be meaningless
        step(F_, db_);
        const double next = maxAbs(db_);
        if (next < options.tolerance) {
          if (options.fastAccept) {
            acceptFirstOrder(db_);
            for (int a = 0; a < rows(); ++a) v[a] = vt_[a] + db_[a];
            return it + 2;
          }
          break;
        }
        if (next <= (1.0 - 0.25 * lambda) * length) break;
        lambda *= 0.5;
      }
      for (int a = 0; a < rows(); ++a) v[a] = vt_[a];
    }
    return -options.maxIterations;
  }

  MAGMORA_INLINE void eval(const double* at) {
    ++counters.evaluations;
    laws_.eval(shape_, at, i_, J_);
  }

  alignas(64) double v[kStorage] = {};
  NewtonCounters counters;

 private:
  MAGMORA_INLINE int rows() const { return Shape::kFixed ? padded(kPorts) : stride_; }

  MAGMORA_INLINE void addKi(const double* base) {
    if constexpr (Shape::kFixed) {
      mulAdd<kPairs>(K_, padded(kPorts), kPorts, i_, base, F_);
    } else {
      for (int a = 0; a < stride_; ++a) {
        double sum = base[a];
        for (int b = 0; b < shape_.ports(); ++b) sum += K_[b * stride_ + a] * i_[b];
        F_[a] = sum;
      }
    }
  }

  MAGMORA_INLINE void residual(const double* p, const double* at) {
    double base[kStorage];
    for (int a = 0; a < rows(); ++a) base[a] = p[a] - at[a];
    addKi(base);
  }

  static constexpr detail::RowTable<Shape::kFixed ? kPorts : 0> kTable = [] {
    if constexpr (Shape::kFixed) return detail::RowTable<kPorts>(Shape::kMaxTriodes, Shape::kMaxPentodes, Shape::kMaxDiodes);
    else return detail::RowTable<0>();
  }();

  enum class Rows { Fixed, Active, Pivoted };

  // Row k of the set with some grids conducting: the dry rows, then those grids (each a width-one row).
  MAGMORA_INLINE detail::Row rowOf(int k) const {
    if (k < kTable.dryCount) return kTable.all[kTable.dry[k]];
    const int w = k - kTable.dryCount;
    return {wetPort_[w], wetPort_[w], 1, wetJacobian_[w], true};
  }

  MAGMORA_INLINE void factor() {
    ++counters.factorisations;
    if constexpr (kPorts == 0) {
      return;
    } else if constexpr (Shape::kFixed) {
      int wet = 0;
      for (int k = 0; k < kTable.gridCount; ++k) {
        const detail::Row& row = kTable.all[kTable.grids[k]];
        wetPort_[wet] = row.port;
        wetJacobian_[wet] = row.jacobian;
        wet += static_cast<int>(!(std::fabs(J_[row.jacobian]) < negligible_[row.port]));
      }
      wet_ = wet;
      mode_ = Rows::Fixed;
      if (!factorWet<kTable.gridCount>(wet)) pivotWet(wet);
    } else {
      factorActive();
    }
  }

  template <int G>
  MAGMORA_INLINE bool factorWet(int wet) {
    if constexpr (G == 0) {
      (void)wet;
      return factorSet<kTable.dryCount>();
    } else {
      return wet == G ? factorSet<kTable.dryCount + G>() : factorWet<G - 1>(wet);
    }
  }

  template <int G>
  MAGMORA_INLINE void stepWet(int wet, const double* in, double* out) {
    if constexpr (G == 0) {
      (void)wet;
      stepSet<kTable.dryCount>(in, out);
    } else {
      if (wet == G) stepSet<kTable.dryCount + G>(in, out);
      else stepWet<G - 1>(wet, in, out);
    }
  }

  MAGMORA_NOINLINE void pivotWet(int wet) {
    int which[detail::atLeast(kPorts, 1)], r = 0;
    for (int k = 0; k < kTable.dryCount; ++k) which[r++] = kTable.dry[k];
    for (int w = 0; w < wet; ++w)
      for (int k = 0; k < kTable.count; ++k)
        if (kTable.all[k].port == wetPort_[w]) which[r++] = k;
    factorWithPivoting(which, r);
  }

  template <int R>
  MAGMORA_INLINE bool factorSet() {
    for (int k = 0; k < Shape::kMaxTriodes * devices::kTriodeJacobian + Shape::kMaxPentodes * devices::kPentodeJacobian +
                                Shape::kMaxDiodes; ++k)
      Jf_[k] = J_[k];
    for (int a = 0; a < R; ++a) {
      const detail::Row row = rowOf(a);
      const double* j = Jf_ + row.jacobian;
      for (int b = 0; b < R; ++b) {
        const double* k = K_ + rowOf(b).port * padded(kPorts) + row.first;
        double sum = j[0] * k[0];
        if (row.width > 1) sum += j[1] * k[1];
        if (row.width > 2) sum += j[2] * k[2];
        A_[a * R + b] = (a == b ? 1.0 : 0.0) - sum;
      }
    }
    if constexpr (R == 2) {
      const double determinant = A_[0] * A_[3] - A_[1] * A_[2];
      const double id = 1.0 / determinant, a00 = A_[0];
      A_[0] = A_[3] * id;
      A_[1] = -A_[1] * id;
      A_[2] = -A_[2] * id;
      A_[3] = a00 * id;
      return std::fabs(determinant) >= detail::kSmallPivot;
    } else {
      return detail::factorUnpivoted<R>(A_);
    }
  }

  template <int R>
  MAGMORA_INLINE void stepSet(const double* in, double* out) {
    double y[detail::atLeast(R, 2)];
    for (int a = 0; a < R; ++a) {
      const detail::Row row = rowOf(a);
      const double* j = Jf_ + row.jacobian;
      double sum = j[0] * in[row.first];
      if (row.width > 1) sum += j[1] * in[row.first + 1];
      if (row.width > 2) sum += j[2] * in[row.first + 2];
      y[a] = sum;
    }
    if constexpr (R == 2) {
      const double y0 = A_[0] * y[0] + A_[1] * y[1];
      y[1] = A_[2] * y[0] + A_[3] * y[1];
      y[0] = y0;
    } else {
      detail::solveUnpivoted<R>(A_, y);
    }
    addSetColumns(y, in, out, std::make_index_sequence<static_cast<size_t>(R)>());
  }

  template <std::size_t... B>
  MAGMORA_INLINE void addSetColumns(const double* y, const double* in, double* out, std::index_sequence<B...>) {
    constexpr auto pairs = std::make_index_sequence<static_cast<size_t>(kPairs)>();
    simd::F2 a[detail::atLeast(kPairs, 1)];
    magmora::detail::loadRows(a, in, pairs);
    (magmora::detail::addColumn(a, K_ + rowOf(static_cast<int>(B)).port * padded(kPorts), simd::F2::set(y[B]), pairs), ...);
    magmora::detail::storeRows(a, out, pairs);
  }

  const detail::Row* rowTable() const {
    if constexpr (Shape::kFixed) return kTable.all;
    else return table_.all;
  }

  void factorActive() {
    const detail::Row* all = rowTable();
    const int count = shape_.ports();
    int r = 0;
    for (int k = 0; k < count; ++k) {
      const double* j = J_ + all[k].jacobian;
      double largest = std::fabs(j[0]);
      if (all[k].width > 1) largest = std::fmax(largest, std::fabs(j[1]));
      if (all[k].width > 2) largest = std::fmax(largest, std::fabs(j[2]));
      selected_[r] = k;
      r += static_cast<int>(!(largest < negligible_[all[k].port]));
    }
    mode_ = Rows::Active;
    buildSelected(selected_, r);
    bool sound = r <= kUnrolled;
    if (sound) sound = detail::factorUpTo<kUnrolled>(r, A_);
    if (!sound) factorWithPivoting(selected_, r);
  }

  // The rows in `which`: their Jacobian entries and I - Jr K E.
  void buildSelected(const int* which, int r) {
    const detail::Row* all = rowTable();
    r_ = r;
    for (int a = 0; a < r; ++a) {
      const detail::Row row = all[which[a]];
      port_[a] = row.port;
      first_[a] = row.first;
      width_[a] = row.width;
      for (int w = 0; w < 3; ++w) Jr_[a][w] = w < row.width ? J_[row.jacobian + w] : 0.0;
    }
    for (int a = 0; a < r; ++a)
      for (int b = 0; b < r; ++b) {
        const double* k = K_ + port_[b] * stride_ + first_[a];
        double sum = Jr_[a][0] * k[0];
        if (width_[a] > 1) sum += Jr_[a][1] * k[1];
        if (width_[a] > 2) sum += Jr_[a][2] * k[2];
        A_[a * r + b] = (a == b ? 1.0 : 0.0) - sum;
      }
  }

  MAGMORA_NOINLINE void factorWithPivoting(const int* which, int r) {
    buildSelected(which, r);
    detail::factorAny(A_, pivot_, r);
    mode_ = Rows::Pivoted;
  }

  // out = (I - K J)^-1 in
  MAGMORA_INLINE void step(const double* in, double* out) {
    if constexpr (kPorts == 0) {
      (void)in, (void)out;
    } else if constexpr (Shape::kFixed) {
      if (mode_ == Rows::Fixed) stepWet<kTable.gridCount>(wet_, in, out);
      else stepSelected(in, out);
    } else {
      stepSelected(in, out);
    }
  }

  void stepSelected(const double* in, double* out) {
    const int r = r_;
    double y[detail::atLeast(kPorts, 2)];
    for (int a = 0; a < r; ++a) {
      const double* f = in + first_[a];
      double sum = Jr_[a][0] * f[0];
      if (width_[a] > 1) sum += Jr_[a][1] * f[1];
      if (width_[a] > 2) sum += Jr_[a][2] * f[2];
      y[a] = sum;
    }
    if (mode_ == Rows::Pivoted) detail::solveAny(A_, pivot_, r, y);
    else detail::solveUpTo<kUnrolled>(r, A_, y);
    if constexpr (Shape::kFixed) {
      addColumns<kPairs>(K_, padded(kPorts), port_, y, r, in, out);
    } else {
      for (int k = 0; k < stride_; ++k) {
        double sum = in[k];
        for (int b = 0; b < r; ++b) sum += K_[port_[b] * stride_ + k] * y[b];
        out[k] = sum;
      }
    }
  }

  // Scale that keeps every junction from going more than a step beyond max(its value, its conduction threshold).
  MAGMORA_INLINE double junctionLimit() const {
    using namespace circuit::netdata;
    const int nt = shape_.triodes(), np = shape_.pentodes();
    double scale = 1.0;
    const auto limit = [&scale](double at, double delta, double on, double reach) {
      const double ceiling = (at > on ? at : on) + reach;
      if (at + delta > ceiling && delta > 0.0) {
        const double ratio = (ceiling - at) / delta;
        if (ratio < scale) scale = ratio;
      }
    };
    for (int d = 0; d < nt; ++d) limit(v[2 * d + 1], dv_[2 * d + 1], kGridOn, kGridStep);
    for (int d = 0; d < np; ++d) {
      const int q = 2 * nt + 3 * d;
      limit(v[q + 2], dv_[q + 2], kGridOn, kGridStep);
      // a folded diode's voltage is minus the plate voltage
      if (set_->flyback[static_cast<size_t>(d)]) limit(-v[q], -dv_[q], kDiodeOn, kDiodeStep);
    }
    for (int d = 0; d < shape_.diodes(); ++d) {
      const int q = 2 * nt + 3 * np + d;
      limit(v[q], dv_[q], kDiodeOn, kDiodeStep);
    }
    return scale;
  }

  // The last step updates the currents to first order with the current J, without evaluating the devices again.
  MAGMORA_INLINE void acceptFirstOrder(const double* d) {
    double* i = i_;
    const double* J = J_;
    for (int k = 0; k < shape_.triodes(); ++k) {
      i[0] += J[0] * d[0] + J[1] * d[1];
      i[1] += J[2] * d[1];
      i += 2;
      d += 2;
      J += devices::kTriodeJacobian;
    }
    for (int k = 0; k < shape_.pentodes(); ++k) {
      i[0] += J[0] * d[0] + J[1] * d[1] + J[2] * d[2];
      i[1] += J[3] * d[0] + J[4] * d[1] + J[5] * d[2];
      i[2] += J[6] * d[2];
      i += 3;
      d += 3;
      J += devices::kPentodeJacobian;
    }
    for (int k = 0; k < shape_.diodes(); ++k) i[k] += J[k] * d[k];
  }

  MAGMORA_INLINE double maxAbs(const double* x) const {
    if constexpr (kPorts == 0) {
      (void)x;
      return 0.0;
    } else {
      simd::F2 big = simd::F2::set(0.0);
      for (int a = 0; a < rows(); a += 2) big = max(abs(simd::F2::load(x + a)), big);
      double lane[2];
      big.store(lane);
      return lane[0] > lane[1] ? lane[0] : lane[1];
    }
  }

  Shape shape_;
  Laws<Shape> laws_;
  const devices::DeviceSet* set_ = nullptr;
  const double* K_ = nullptr;
  double* i_ = nullptr;
  int stride_ = 0;
  double dropThreshold_ = 0.0;
  static constexpr int kUnrolled = kPorts < 10 ? kPorts : 10;
  Rows mode_ = Rows::Active;
  int wet_ = 0, wetPort_[kStorage] = {}, wetJacobian_[kStorage] = {};
  int r_ = 0;
  detail::RowTable<Shape::kFixed ? 0 : kPorts> table_;
  int selected_[kStorage] = {}, width_[kStorage] = {};
  alignas(64) double F_[kStorage] = {};
  double dv_[kStorage] = {}, vt_[kStorage] = {}, db_[kStorage] = {}, negligible_[kStorage] = {};
  double J_[detail::atLeast(devices::kTriodeJacobian * Shape::kMaxTriodes + devices::kPentodeJacobian * Shape::kMaxPentodes +
                                Shape::kMaxDiodes, 1)] = {};
  double Jf_[detail::atLeast(devices::kTriodeJacobian * Shape::kMaxTriodes + devices::kPentodeJacobian * Shape::kMaxPentodes +
                                 Shape::kMaxDiodes, 1)] = {};  // the Jacobian the factorisation in force was made of
  double Jr_[kStorage][3] = {};
  double A_[kStorage * kStorage] = {};
  int port_[kStorage] = {}, first_[kStorage] = {}, pivot_[kStorage] = {};
};

}  // namespace magmora::solver
