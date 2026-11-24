// Dense LU with partial pivoting on caller-owned, row-major storage. No allocation.
#pragma once

namespace magmora::solver {

// Factorises a[n x n] in place; false if it is singular.
bool luFactor(double* a, int* pivot, int n);
// Solves for `columns` right-hand sides held row-major in b[n x stride].
void luSolve(const double* a, const int* pivot, int n, double* b, int columns, int stride);
// One step of iterative refinement with a residual in twice the precision. On entry x is luSolve's answer for rhs;
// on exit x + correction is the solution to better than double precision (x alone is its nearest double) and
// rhs is destroyed. The companion conductances span ten decades, so plain LU loses about eight digits here.
void luRefine(const double* matrix, const double* lu, const int* pivot, int n, double* x, double* rhs,
              double* correction, int columns);

}  // namespace magmora::solver
