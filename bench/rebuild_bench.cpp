// Cost of rebuilding one cluster's matrices after a knob move: dense LU of the nodal matrix G,
// forward/back substitution for every source, state and device column, then the selector rows (which are sparse:
// a node-pair difference or a scaled one). Reads the file written by export_rebuild.py, checks the result against the
// prototype's matrices and prints the time per rebuild. All of [Pv Ps K; Sv Ss Si; Yv Ys Yi] comes out of one N G^-1 B.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
typedef std::vector<double> vec;
int main(int argc, char** argv) {
  if (argc < 2) return 2;
  FILE* f = std::fopen(argv[1], "rb"); if (!f) return 1;
  int reps = argc > 2 ? std::atoi(argv[2]) : 2000;
  int64_t nc; if (std::fread(&nc, 8, 1, f) != 1) return 3;
  double total = 0;
  for (int c = 0; c < nc; ++c) {
    int64_t h[9]; if (std::fread(h, 8, 9, f) != 9) return 3;
    int nx = (int)h[0], m = (int)h[1], nr = (int)h[2];
    vec G(nx * nx), B(nx * m), N(nr * nx), ref(nr * m), M(nx * nx), X(nx * m), R(nr * m); std::vector<int> piv(nx);
    auto rd = [&](vec& a) { if (std::fread(a.data(), 8, a.size(), f) != a.size()) std::exit(3); };
    rd(G); rd(B); rd(N); rd(ref);
    std::vector<int> rs(nr + 1, 0), ci; vec cv;                      // selector rows in compressed form
    for (int a = 0; a < nr; ++a) { for (int b = 0; b < nx; ++b) if (N[a * nx + b] != 0.0) { ci.push_back(b); cv.push_back(N[a * nx + b]); } rs[a + 1] = (int)ci.size(); }
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
      auto t0 = std::chrono::steady_clock::now();
      for (int it = 0; it < reps; ++it) {
        M = G; X = B;
        for (int a = 0; a < nx; ++a) {                               // LU with partial pivoting, right-hand sides carried along
          int q = a; double big = std::fabs(M[a * nx + a]);
          for (int b = a + 1; b < nx; ++b) if (std::fabs(M[b * nx + a]) > big) { big = std::fabs(M[b * nx + a]); q = b; }
          if (q != a) { for (int k = 0; k < nx; ++k) std::swap(M[a * nx + k], M[q * nx + k]); for (int k = 0; k < m; ++k) std::swap(X[a * m + k], X[q * m + k]); }
          double inv = 1.0 / M[a * nx + a];
          for (int b = a + 1; b < nx; ++b) { double fct = M[b * nx + a] * inv; if (fct == 0.0) continue;
            double* mb = &M[b * nx]; const double* ma = &M[a * nx]; for (int k = a + 1; k < nx; ++k) mb[k] -= fct * ma[k];
            double* xb = &X[b * m]; const double* xa = &X[a * m]; for (int k = 0; k < m; ++k) xb[k] -= fct * xa[k]; }
        }
        for (int a = nx - 1; a >= 0; --a) { double* xa = &X[a * m]; double inv = 1.0 / M[a * nx + a];
          for (int b = a + 1; b < nx; ++b) { double u = M[a * nx + b]; if (u == 0.0) continue; const double* xb = &X[b * m]; for (int k = 0; k < m; ++k) xa[k] -= u * xb[k]; }
          for (int k = 0; k < m; ++k) xa[k] *= inv; }
        for (int a = 0; a < nr; ++a) { double* ra = &R[a * m]; for (int k = 0; k < m; ++k) ra[k] = 0.0;
          for (int j = rs[a]; j < rs[a + 1]; ++j) { const double* xb = &X[ci[j] * m]; double w = cv[j]; for (int k = 0; k < m; ++k) ra[k] += w * xb[k]; } }
      }
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / reps; if (el < best) best = el;
    }
    double err = 0, mx = 0; for (size_t k = 0; k < R.size(); ++k) { err = std::fmax(err, std::fabs(R[k] - ref[k])); mx = std::fmax(mx, std::fabs(ref[k])); }
    std::printf("  cluster %d: %d unknowns, %d columns, %d rows: %.1f us per rebuild (relative error %.1e)\n", c, nx, m, nr, best * 1e6, err / mx);
    total += best;
  }
  std::printf("all clusters: %.1f us\n", total * 1e6);
  return 0;
}
