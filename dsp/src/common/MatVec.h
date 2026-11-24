// The linear kernels: dense products with column-major, zero-padded matrices (even row count).
#pragma once
#include <utility>

#include "common/Simd.h"

namespace magmora {

constexpr int padded(int rows) { return (rows + 1) & ~1; }

namespace detail {

template <std::size_t... I>
MAGMORA_INLINE void loadRows(simd::F2* a, const double* p, std::index_sequence<I...>) {
  ((a[I] = simd::F2::load(p + 2 * I)), ...);
}

template <std::size_t... I>
MAGMORA_INLINE void storeRows(const simd::F2* a, double* p, std::index_sequence<I...>) {
  (a[I].store(p + 2 * I), ...);
}

template <std::size_t... I>
MAGMORA_INLINE void addColumn(simd::F2* a, const double* m, simd::F2 x, std::index_sequence<I...>) {
  ((a[I] = simd::madd(a[I], simd::F2::load(m + 2 * I), x)), ...);
}

}  // namespace detail

// out = c + M x, with Pairs * 2 rows known at compile time. Accumulators live in registers, the FMAs of one column are
// independent, so the chain is as long as the number of columns. With few rows two accumulator sets halve that chain.
template <int Pairs>
MAGMORA_INLINE void mulAdd(const double* MAGMORA_RESTRICT M, int stride, int cols, const double* MAGMORA_RESTRICT x,
                          const double* MAGMORA_RESTRICT c, double* MAGMORA_RESTRICT out) {
  if constexpr (Pairs > 0) {
    using simd::F2;
    constexpr auto rows = std::make_index_sequence<Pairs>();
    F2 a[Pairs];
    detail::loadRows(a, c, rows);
    int col = 0;
    if constexpr (Pairs <= 10) {
      F2 b[Pairs];
      const F2 zero = F2::set(0.0);
      for (F2& v : b) v = zero;
      for (; col + 1 < cols; col += 2) {
        const double* m = M + col * stride;
        detail::addColumn(a, m, F2::set(x[col]), rows);
        detail::addColumn(b, m + stride, F2::set(x[col + 1]), rows);
      }
      for (int k = 0; k < Pairs; ++k) a[k] = a[k] + b[k];
    }
    for (; col < cols; ++col) detail::addColumn(a, M + col * stride, F2::set(x[col]), rows);
    detail::storeRows(a, out, rows);
  } else {
    (void)M, (void)stride, (void)cols, (void)x, (void)c, (void)out;
  }
}

// out = c + the selected columns of M, weighted.
template <int Pairs>
MAGMORA_INLINE void addColumns(const double* MAGMORA_RESTRICT M, int stride, const int* column, const double* weight,
                              int count, const double* MAGMORA_RESTRICT c, double* MAGMORA_RESTRICT out) {
  if constexpr (Pairs > 0) {
    constexpr auto rows = std::make_index_sequence<Pairs>();
    simd::F2 a[Pairs];
    detail::loadRows(a, c, rows);
    for (int k = 0; k < count; ++k) detail::addColumn(a, M + column[k] * stride, simd::F2::set(weight[k]), rows);
    detail::storeRows(a, out, rows);
  } else {
    (void)M, (void)stride, (void)column, (void)weight, (void)count, (void)c, (void)out;
  }
}

using MulAddFn = void (*)(const double*, int, int, const double*, const double*, double*);

// Chooses the fixed-size instance for a row count decided at prepare() time (rows must be even).
MulAddFn selectMulAdd(int rows);

}  // namespace magmora
