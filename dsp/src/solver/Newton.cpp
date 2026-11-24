#include "solver/Newton.h"

namespace magmora::solver::detail {

void factorAny(double* A, int* pivot, int r) {
  for (int a = 0; a < r; ++a) {
    int q = a;
    double big = std::fabs(A[a * r + a]);
    for (int b = a + 1; b < r; ++b) {
      const double m = std::fabs(A[b * r + a]);
      if (m > big) {
        big = m;
        q = b;
      }
    }
    pivot[a] = q;
    if (q != a)
      for (int c = 0; c < r; ++c) std::swap(A[a * r + c], A[q * r + c]);
    const double inverse = 1.0 / A[a * r + a];
    A[a * r + a] = inverse;
    for (int b = a + 1; b < r; ++b) {
      const double f = A[b * r + a] * inverse;
      A[b * r + a] = f;
      for (int c = a + 1; c < r; ++c) A[b * r + c] -= f * A[a * r + c];
    }
  }
}

void solveAny(const double* A, const int* pivot, int r, double* y) {
  for (int a = 0; a < r; ++a)
    if (pivot[a] != a) std::swap(y[a], y[pivot[a]]);
  for (int a = 0; a < r; ++a)
    for (int b = a + 1; b < r; ++b) y[b] -= A[b * r + a] * y[a];
  for (int a = r - 1; a >= 0; --a) {
    double sum = y[a];
    for (int c = a + 1; c < r; ++c) sum -= A[a * r + c] * y[c];
    y[a] = sum * A[a * r + a];
  }
}

}  // namespace magmora::solver::detail
