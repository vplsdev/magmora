#include "solver/DenseLU.h"

#include <cmath>
#include <utility>

#include "common/Simd.h"

namespace magmora::solver {
namespace {

// y -= f x
MAGMORA_INLINE void subtractScaled(double* MAGMORA_RESTRICT y, const double* MAGMORA_RESTRICT x, double f, int n) {
  const simd::F2 minusF = simd::F2::set(-f);
  int k = 0;
  for (; k + 1 < n; k += 2) simd::madd(simd::F2::load(y + k), simd::F2::load(x + k), minusF).store(y + k);
  if (k < n) y[k] -= f * x[k];
}

// a * b - product
MAGMORA_INLINE simd::F2 fusedError(simd::F2 a, simd::F2 b, simd::F2 product) {
#if MAGMORA_NEON
  return simd::madd(simd::F2::set(0.0) - product, a, b);
#else  // no FMA in the baseline instruction set
  double x[2], y[2], p[2];
  a.store(x);
  b.store(y);
  product.store(p);
  for (int k = 0; k < 2; ++k) p[k] = std::fma(x[k], y[k], -p[k]);
  return simd::F2::load(p);
#endif
}

}  // namespace

bool luFactor(double* a, int* pivot, int n) {
  for (int k = 0; k < n; ++k) {
    int q = k;
    double big = std::fabs(a[k * n + k]);
    for (int r = k + 1; r < n; ++r) {
      const double m = std::fabs(a[r * n + k]);
      if (m > big) {
        big = m;
        q = r;
      }
    }
    if (!(big > 0.0)) return false;
    pivot[k] = q;
    double* pivotRow = a + k * n;
    if (q != k) {
      double* other = a + q * n;
      for (int c = 0; c < n; ++c) std::swap(pivotRow[c], other[c]);
    }
    const double inverse = 1.0 / pivotRow[k];
    for (int r = k + 1; r < n; ++r) {
      double* row = a + r * n;
      if (row[k] == 0.0) continue;
      const double f = row[k] * inverse;
      row[k] = f;
      subtractScaled(row + k + 1, pivotRow + k + 1, f, n - k - 1);
    }
  }
  return true;
}

void luSolve(const double* a, const int* pivot, int n, double* b, int columns, int stride) {
  for (int k = 0; k < n; ++k)
    if (pivot[k] != k)
      for (int c = 0; c < columns; ++c) std::swap(b[k * stride + c], b[pivot[k] * stride + c]);
  for (int r = 1; r < n; ++r)
    for (int k = 0; k < r; ++k) {
      const double f = a[r * n + k];
      if (f != 0.0) subtractScaled(b + r * stride, b + k * stride, f, columns);
    }
  for (int r = n - 1; r >= 0; --r) {
    double* row = b + r * stride;
    for (int k = r + 1; k < n; ++k) {
      const double f = a[r * n + k];
      if (f != 0.0) subtractScaled(row, b + k * stride, f, columns);
    }
    const double inverse = 1.0 / a[r * n + r];
    for (int c = 0; c < columns; ++c) row[c] *= inverse;
  }
}

void luRefine(const double* matrix, const double* lu, const int* pivot, int n, double* x, double* rhs,
              double* correction, int columns) {
  using simd::F2;
  for (int r = 0; r < n; ++r) {
    double* high = rhs + r * columns;
    double* low = correction + r * columns;
    for (int c = 0; c < columns; ++c) low[c] = 0.0;
    for (int k = 0; k < n; ++k) {
      const double g = matrix[r * n + k];
      if (g == 0.0) continue;
      const double* column = x + k * columns;
      const F2 minusG = F2::set(-g);
      int c = 0;
      for (; c + 1 < columns; c += 2) {  // high:low -= g x, exactly (TwoProduct by FMA, TwoSum)
        const F2 value = F2::load(column + c), sum0 = F2::load(high + c);
        const F2 product = minusG * value;
        const F2 productError = fusedError(minusG, value, product);
        const F2 sum = sum0 + product;
        const F2 shadow = sum - sum0;
        const F2 sumError = (sum0 - (sum - shadow)) + (product - shadow);
        (F2::load(low + c) + (sumError + productError)).store(low + c);
        sum.store(high + c);
      }
      if (c < columns) {
        const double product = -g * column[c];
        const double productError = std::fma(-g, column[c], -product);
        const double sum = high[c] + product;
        const double shadow = sum - high[c];
        low[c] += ((high[c] - (sum - shadow)) + (product - shadow)) + productError;
        high[c] = sum;
      }
    }
    for (int c = 0; c < columns; ++c) high[c] += low[c];
  }
  luSolve(lu, pivot, n, rhs, columns, columns);
  for (int k = 0; k < n * columns; ++k) {
    const double refined = x[k] + rhs[k];
    correction[k] = (x[k] - refined) + rhs[k];
    x[k] = refined;
  }
}

}  // namespace magmora::solver
