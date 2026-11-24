// Stand-alone benchmark of the per-sample cluster kernel (same algorithm as proto/blockamp/partition.py).
// This is the REFERENCE path: generic run-time sized code. The demonstrated fast design is kernel_fast.cpp.
// Options: -DUSE_TABLES (tabulated device laws), -DADAPTIVE_FALLBACK (step-controlled continuation, see below).
// Reads the cluster file written by export_clusters.py and a raw float64 input at the oversampled rate,
// writes the raw float64 speaker voltage and prints the time per sample. Not product code: the product
// engine is dsp/; this file exists to put a measured number on the CPU cost.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using std::vector;
typedef vector<double> vec;
static const double LN_MAX = 40.0;

// Beam-tetrode knee, screen share and Jacobian of model v2 (proto/blockamp/devices.py: pentode), given the space
// current i0 and its derivatives, the rectified plate voltage vp (dvp = d vp / d vpk) and the grid current.
static inline void pentode_tail(double i0, double di0s, double di0g, double vp, double dvp, double ig, double dgg, const double* P, double* i, double* J) {
  double kv = P[4], rs = P[5], lam = P[9], np = P[10], asc = P[11];
  double rt = std::sqrt(vp), a = kv * vp * rt, da = 1.5 * kv * rt * dvp;
  double d3 = i0 * i0 * i0 + a * a * a + 1e-60, c = 1.0 / std::cbrt(d3), m = i0 * a * c;
  double dmi = a * c * (1.0 - i0 * i0 * i0 / d3), dma = i0 * c * (1.0 - a * a * a / d3);
  double lf = 1.0 + lam * vp, den = i0 + a + 1e-20, fsc = rs + asc * i0 / den;
  double dsi = rs + asc * i0 * (i0 + 2.0 * a + 2e-20) / (den * den), dsa = -asc * i0 * i0 / (den * den);
  i[0] = np * m * lf; i[1] = np * i0 * fsc; i[2] = np * ig;
  J[0] = np * (dma * da * lf + m * lam * dvp); J[1] = np * dmi * di0s * lf; J[2] = np * dmi * di0g * lf;
  J[3] = np * dsa * da;                        J[4] = np * dsi * di0s;      J[5] = np * dsi * di0g;
  J[6] = 0.0;                                  J[7] = 0.0;                  J[8] = np * dgg;
}

#ifdef USE_TABLES
// Cubic Hermite tables built at start-up from the same closed forms (value + slope per knot).
struct Table {
  double x0, inv_h; int n; vec f, d;
  template <class Fn> void build(double lo, double hi, int knots, Fn fn) {
    x0 = lo; n = knots; double h = (hi - lo) / (knots - 1); inv_h = 1.0 / h; f.resize(knots); d.resize(knots);
    for (int k = 0; k < knots; ++k) { double v, dv; fn(lo + k * h, v, dv); f[k] = v; d[k] = dv * h; }
  }
  inline void eval(double x, double& y, double& dy) const {   // caller guarantees lo <= x <= hi
    double t = (x - x0) * inv_h; int k = (int)t; if (k > n - 2) k = n - 2; t -= k;
    double f0 = f[k], f1 = f[k + 1], d0 = d[k], d1 = d[k + 1];
    double c2 = 3.0 * (f1 - f0) - 2.0 * d0 - d1, c3 = 2.0 * (f0 - f1) + d0 + d1;
    y = f0 + t * (d0 + t * (c2 + t * c3));
    dy = (d0 + t * (2.0 * c2 + 3.0 * t * c3)) * inv_h;
  }
};
static Table T_SP;                       // softplus(x) on [-24, 24]
static inline void softplus_exact(double x, double& s, double& ds) {
  if (x > LN_MAX) { s = x; ds = 1.0; return; }
  if (x < -LN_MAX) { s = 0.0; ds = 0.0; return; }
  double e = std::exp(x); s = std::log1p(e); ds = e / (1.0 + e);
}
static inline void softplus(double x, double& s, double& ds) {
  if (x >= 24.0) { s = x; ds = 1.0; return; }
  if (x <= -24.0) { s = 0.0; ds = 0.0; return; }
  T_SP.eval(x, s, ds);
}
// u^(2*ex) tabulated against u = sqrt(e): smooth at zero, so the cut-off region is resolved.
struct PowTable { Table t; double ex, umax;
  void build(double exponent, double emax) { ex = exponent; umax = std::sqrt(emax);
    t.build(0.0, umax, 2048, [&](double u, double& v, double& dv) { v = std::pow(u, 2.0 * ex); dv = u > 0 ? 2.0 * ex * v / u : 0.0; }); }
  inline void eval(double e, double& y, double& dy) const {    // y = e^ex, dy = d/de
    double u = std::sqrt(e);
    if (u >= umax || u < 1e-9) { y = std::pow(e, ex); dy = e > 0 ? ex * y / e : 0.0; return; }
    double dydu; t.eval(u, y, dydu); dy = dydu / (2.0 * u);
  } };
struct GridTable { Table t; double gg, cg, xi, vg0, lo, hi;
  void build(double gg_, double cg_, double xi_, double vg0_) { gg = gg_; cg = cg_; xi = xi_; vg0 = vg0_; lo = -4.0 - vg0; hi = 40.0;
    t.build(lo, hi, 4096, [&](double v, double& y, double& dy) { exact(v, y, dy); }); }
  inline void exact(double v, double& y, double& dy) const { double sg, dsg; softplus_exact(cg * (v + vg0), sg, dsg);
    if (sg > 0.0) { double b = sg / cg; y = gg * std::pow(b, xi); dy = xi * y / b * dsg; } else { y = 0; dy = 0; } }
  inline void eval(double v, double& y, double& dy) const {
    if (v <= lo) { y = 0; dy = 0; return; }
    if (v >= hi) { exact(v, y, dy); return; }
    t.eval(v, y, dy); } };
static PowTable T_POW_T, T_POW_P; static GridTable T_GRID_T, T_GRID_P;
static void init_tables(const double* PT, const double* PP) {
  T_SP.build(-24.0, 24.0, 4096, [](double x, double& s, double& ds) { softplus_exact(x, s, ds); });
  if (PT) { T_POW_T.build(PT[1], 400.0); T_GRID_T.build(PT[5], PT[6], PT[7], PT[8]); }
  if (PP) { T_POW_P.build(PP[1], 4000.0); T_GRID_P.build(PP[6], PP[7], PP[8], 0.0); }
}
static inline void triode(double vpk, double vgk, const double* P, double* i, double* J) {
  double mu = P[0], kg1 = P[2], kp = P[3], kvb = P[4];
  double ip = 0, dpp = 0, dpg = 0;
  if (vpk > 0.0) {
    double r = std::sqrt(kvb + vpk * vpk), sp, dsp;
    softplus(kp * (1.0 / mu + (vgk + P[9]) / r), sp, dsp);
    double e1 = vpk / kp * sp;
    if (e1 > 0.0) {
      double pw, dpw; T_POW_T.eval(e1, pw, dpw);
      ip = 2.0 / kg1 * pw; double d = 2.0 / kg1 * dpw;
      dpp = d * (sp / kp + vpk / kp * dsp * (-kp * (vgk + P[9]) * vpk / (r * r * r)));
      dpg = d * (vpk * dsp / r);
    }
  }
  double ig, dgg; T_GRID_T.eval(vgk, ig, dgg);
  i[0] = ip; i[1] = ig; J[0] = dpp; J[1] = dpg; J[2] = 0.0; J[3] = dgg;
}
static inline void pentode(double vpk, double vsk, double vgk, const double* P, double* i, double* J) {
  double mu = P[0], kg1 = P[2], kp = P[3];
  double vs = vsk > 1.0 ? vsk : 1.0, dvs = vsk > 1.0 ? 1.0 : 0.0, sp, dsp;
  softplus(kp * (1.0 / mu + vgk / vs), sp, dsp);
  double e1 = vs / kp * sp, i0 = 0, di0s = 0, di0g = 0;
  if (e1 > 0.0) {
    double pw, dpw; T_POW_P.eval(e1, pw, dpw);
    double k = M_PI / kg1;
    i0 = k * pw; double d = k * dpw;
    di0s = d * (sp / kp + vs / kp * dsp * (-kp * vgk / (vs * vs))) * dvs;
    di0g = d * dsp;
  }
  double vp, dvp; softplus(4.0 * vpk, vp, dvp); vp *= 0.25;
  double ig, dgg; T_GRID_P.eval(vgk, ig, dgg);
  pentode_tail(i0, di0s, di0g, vp, dvp, ig, dgg, P, i, J);
}
#else
static inline void softplus(double x, double& s, double& ds) {
  if (x > LN_MAX) { s = x; ds = 1.0; return; }
  if (x < -LN_MAX) { s = 0.0; ds = 0.0; return; }
  double e = std::exp(x); s = std::log1p(e); ds = e / (1.0 + e);
}

static inline void triode(double vpk, double vgk, const double* P, double* i, double* J) {
  double mu = P[0], ex = P[1], kg1 = P[2], kp = P[3], kvb = P[4], gg = P[5], cg = P[6], xi = P[7], vg0 = P[8];
  double ip = 0, dpp = 0, dpg = 0;
  if (vpk > 0.0) {
    double r = std::sqrt(kvb + vpk * vpk), sp, dsp;
    softplus(kp * (1.0 / mu + (vgk + P[9]) / r), sp, dsp);
    double e1 = vpk / kp * sp;
    if (e1 > 0.0) {
      ip = 2.0 / kg1 * std::pow(e1, ex);
      double d = ex * ip / e1;
      dpp = d * (sp / kp + vpk / kp * dsp * (-kp * (vgk + P[9]) * vpk / (r * r * r)));
      dpg = d * (vpk * dsp / r);
    }
  }
  double sg, dsg, ig = 0, dgg = 0;
  softplus(cg * (vgk + vg0), sg, dsg);
  if (sg > 0.0) { double b = sg / cg; ig = gg * std::pow(b, xi); dgg = xi * ig / b * dsg; }
  i[0] = ip; i[1] = ig; J[0] = dpp; J[1] = dpg; J[2] = 0.0; J[3] = dgg;
}

static inline void pentode(double vpk, double vsk, double vgk, const double* P, double* i, double* J) {
  double mu = P[0], ex = P[1], kg1 = P[2], kp = P[3], gg = P[6], cg = P[7], xi = P[8];
  double vs = vsk > 1.0 ? vsk : 1.0, dvs = vsk > 1.0 ? 1.0 : 0.0, sp, dsp;
  softplus(kp * (1.0 / mu + vgk / vs), sp, dsp);
  double e1 = vs / kp * sp, i0 = 0, di0s = 0, di0g = 0;
  if (e1 > 0.0) {
    i0 = M_PI / kg1 * std::pow(e1, ex);
    double d = ex * i0 / e1;
    di0s = d * (sp / kp + vs / kp * dsp * (-kp * vgk / (vs * vs))) * dvs;
    di0g = d * dsp;
  }
  double vp, dvp; softplus(4.0 * vpk, vp, dvp); vp *= 0.25;
  double sg, dsg, ig = 0, dgg = 0;
  softplus(cg * vgk, sg, dsg);
  if (sg > 0.0) { double b = sg / cg; ig = gg * std::pow(b, xi); dgg = xi * ig / b * dsg; }
  pentode_tail(i0, di0s, di0g, vp, dvp, ig, dgg, P, i, J);
}

#endif

static inline void diode(double v, const double* P, double* i, double* J) {
  double is = P[0], nvt = P[1], vc = P[2];
#ifdef USE_TABLES
  if (v < -40.0 * nvt) { i[0] = -is; J[0] = 1e-12; return; }   // exp(-40) is below double precision of the sum
#endif
  if (v > vc) { double e = std::exp(vc / nvt); i[0] = is * (e - 1.0) + is * e / nvt * (v - vc); J[0] = is * e / nvt; }
  else { double e = std::exp(v / nvt); i[0] = is * (e - 1.0); J[0] = is * e / nvt + 1e-12; }
}

static void mv(const vec& A, const double* x, double* y, int rows, int cols, bool add);

struct Cluster {
  int np, ns, nu, ny, nd, nin; bool extrap = true;
  vec Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, s, v, u, y, yprev, par, scale;
  vector<int64_t> dtype, dport, in_idx, in_src;
  vec i, J, p, M, F, dv, vt, vprev, sn;   // work
  long iters = 0, fails = 0; double t_newton = 0, t_lin = 0; long evals = 0;

  inline void eval(const double* vv) {
    ++evals;
    for (int d = 0; d < nd; ++d) {
      int q = (int)dport[d]; const double* P = &par[d * 12];
      double ii[3], JJ[9];
      if (dtype[d] == 0) { triode(vv[q], vv[q + 1], P, ii, JJ); i[q] = ii[0]; i[q + 1] = ii[1];
        J[d * 9 + 0] = JJ[0]; J[d * 9 + 1] = JJ[1]; J[d * 9 + 3] = JJ[2]; J[d * 9 + 4] = JJ[3]; }
      else if (dtype[d] == 1) { pentode(vv[q], vv[q + 1], vv[q + 2], P, ii, JJ); for (int a = 0; a < 3; ++a) i[q + a] = ii[a];
        for (int a = 0; a < 9; ++a) J[d * 9 + a] = JJ[a]; }
      else { diode(vv[q], P, ii, JJ); i[q] = ii[0]; J[d * 9] = JJ[0]; }
    }
  }
  inline double residual(const double* vv, double* Fo) {
    double r = 0;
    for (int a = 0; a < np; ++a) { double acc = p[a] - vv[a]; const double* Ka = &K[a * np];
      for (int b = 0; b < np; ++b) acc += Ka[b] * i[b]; Fo[a] = acc; r += acc * acc; }
    return r;
  }
  // Same algorithm as proto/blockamp/dk.py: newton.
  vec db; vector<int> piv; vec plast;
  void lu() {
    for (int a = 0; a < np; ++a) {
      int q = a; double big = std::fabs(M[a * np + a]);
      for (int b = a + 1; b < np; ++b) if (std::fabs(M[b * np + a]) > big) { big = std::fabs(M[b * np + a]); q = b; }
      piv[a] = q;
      if (q != a) for (int c = 0; c < np; ++c) std::swap(M[a * np + c], M[q * np + c]);
      double inv = 1.0 / M[a * np + a];
      for (int b = a + 1; b < np; ++b) { double f = M[b * np + a] * inv; M[b * np + a] = f;
        if (f != 0.0) for (int c = a + 1; c < np; ++c) M[b * np + c] -= f * M[a * np + c]; }
    }
  }
  void lu_solve(double* x) const {
    for (int a = 0; a < np; ++a) if (piv[a] != a) std::swap(x[a], x[piv[a]]);
    for (int a = 0; a < np; ++a) for (int b = a + 1; b < np; ++b) x[b] -= M[b * np + a] * x[a];
    for (int a = np - 1; a >= 0; --a) { double acc = x[a]; for (int c = a + 1; c < np; ++c) acc -= M[a * np + c] * x[c]; x[a] = acc / M[a * np + a]; }
  }
  int newton(const double* pp, double tol, int maxit) {
    if (np == 0) return 1;
    eval(v.data());
    for (int a = 0; a < np; ++a) { double acc = pp[a] - v[a]; const double* Ka = &K[a * np]; for (int b = 0; b < np; ++b) acc += Ka[b] * i[b]; F[a] = acc; }
    for (int it = 0; it < maxit; ++it) {
      for (int a = 0; a < np * np; ++a) M[a] = 0.0;
      for (int a = 0; a < np; ++a) { M[a * np + a] = 1.0; dv[a] = F[a]; }
      for (int d = 0; d < nd; ++d) {
        int q = (int)dport[d], w = dtype[d] == 0 ? 2 : (dtype[d] == 1 ? 3 : 1);
        for (int a = 0; a < np; ++a) for (int b = 0; b < w; ++b) { double acc = 0;
          for (int c = 0; c < w; ++c) acc += K[a * np + q + c] * J[d * 9 + c * 3 + b]; M[a * np + q + b] -= acc; }
      }
      lu(); lu_solve(dv.data());
      double sc = 1.0;
      for (int d = 0; d < nd; ++d) {
        int q = (int)dport[d], jp = dtype[d] == 0 ? q + 1 : (dtype[d] == 1 ? q + 2 : q);
        double lim = dtype[d] != 2 ? 3.0 : 0.3, von = dtype[d] != 2 ? -0.5 : 0.4;
        double base = v[jp] > von ? v[jp] : von;
        if (dv[jp] > 0.0 && v[jp] + dv[jp] > base + lim) { double r = (base + lim - v[jp]) / dv[jp]; if (r < sc) sc = r; }
      }
      double nd_ = 0; for (int a = 0; a < np; ++a) { dv[a] *= sc; nd_ = std::fmax(nd_, std::fabs(dv[a])); }
      if (nd_ < tol) {
        for (int d = 0; d < nd; ++d) { int q = (int)dport[d], w = dtype[d] == 0 ? 2 : (dtype[d] == 1 ? 3 : 1); double t3[3];
          for (int a = 0; a < w; ++a) { double acc = 0; for (int c = 0; c < w; ++c) acc += J[d * 9 + a * 3 + c] * dv[q + c]; t3[a] = acc; }
          for (int a = 0; a < w; ++a) i[q + a] += t3[a]; }
        for (int a = 0; a < np; ++a) v[a] += dv[a];
        return it + 1;
      }
      double lam = 1.0;
      for (;;) {
        for (int a = 0; a < np; ++a) vt[a] = v[a] + lam * dv[a];
        eval(vt.data());
        for (int a = 0; a < np; ++a) { double acc = pp[a] - vt[a]; const double* Ka = &K[a * np]; for (int b = 0; b < np; ++b) acc += Ka[b] * i[b]; F[a] = acc; db[a] = acc; }
        if (sc < 1.0 || lam < 0.02) break;
        lu_solve(db.data());
        double nb = 0; for (int a = 0; a < np; ++a) nb = std::fmax(nb, std::fabs(db[a]));
        if (nb <= (1.0 - 0.25 * lam) * nd_ || nb < tol) break;
        lam *= 0.5;
      }
      for (int a = 0; a < np; ++a) v[a] = vt[a];
    }
    return -maxit;
  }
};

static void mv(const vec& Av, const double* xin, double* yout, int rows, int cols, bool add) {
  // dot products dominate the linear part: let the compiler reassociate so they vectorise (NEON/AVX)
#if defined(__clang__)
#pragma clang fp reassociate(on) contract(fast)
#endif
  const double* __restrict A = Av.data(); const double* __restrict x = xin; double* __restrict y = yout;
  for (int a = 0; a < rows; ++a) { double acc = add ? y[a] : 0.0; const double* __restrict r = A + (size_t)a * cols;
    for (int b = 0; b < cols; ++b) acc += r[b] * x[b]; y[a] = acc; }
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: kernel clusters.bin in.f64 out.f64 [tol]\n"); return 2; }
  double tol = argc > 4 ? std::atof(argv[4]) : 1e-6;
  FILE* f = std::fopen(argv[1], "rb"); if (!f) return 1;
  auto ri = [&](int64_t* p, size_t n) { if (std::fread(p, 8, n, f) != n) std::exit(3); };
  auto ra = [&](vec& a, size_t n) { a.resize(n); if (n && std::fread(a.data(), 8, n, f) != n) std::exit(3); };
  int64_t hdr[3]; ri(hdr, 3);
  int nc = (int)hdr[0]; vector<Cluster> C(nc);
  for (auto& c : C) {
    int64_t h[7]; ri(h, 7); c.np = h[0]; c.ns = h[1]; c.nu = h[2]; c.ny = h[3]; c.nd = h[4]; c.nin = h[5]; c.extrap = h[6] != 0;
    ra(c.Pv, c.np * c.nu); ra(c.Ps, c.np * c.ns); ra(c.K, c.np * c.np); ra(c.Sv, c.ns * c.nu); ra(c.Ss, c.ns * c.ns);
    ra(c.Si, c.ns * c.np); ra(c.Yv, c.ny * c.nu); ra(c.Ys, c.ny * c.ns); ra(c.Yi, c.ny * c.np);
    ra(c.s, c.ns); ra(c.v, c.np); ra(c.u, c.nu); ra(c.y, c.ny);
    c.dtype.resize(c.nd); ri(c.dtype.data(), c.nd); c.dport.resize(c.nd); ri(c.dport.data(), c.nd); ra(c.par, c.nd * 12);
    c.in_idx.resize(c.nin); ri(c.in_idx.data(), c.nin); c.in_src.resize(c.nin * 2); ri(c.in_src.data(), c.nin * 2); ra(c.scale, c.nin);
    c.yprev = c.y; c.i.assign(c.np, 0); c.J.assign(c.nd * 9, 0); c.p.assign(c.np, 0); c.M.assign(c.np * c.np, 0);
    c.F.assign(c.np, 0); c.dv.assign(c.np, 0); c.vt.assign(c.np, 0); c.vprev = c.v; c.sn.assign(c.ns, 0);
    c.db.assign(c.np, 0); c.piv.assign(c.np, 0); c.plast.assign(c.np, 0);
    mv(c.Pv, c.u.data(), c.plast.data(), c.np, c.nu, false); mv(c.Ps, c.s.data(), c.plast.data(), c.np, c.ns, true);
  }
  std::fclose(f);
#ifdef USE_TABLES
  { const double *PT = nullptr, *PP = nullptr;
    for (auto& c : C) for (int d = 0; d < c.nd; ++d) { if (c.dtype[d] == 0) PT = &c.par[d * 12]; if (c.dtype[d] == 1) PP = &c.par[d * 12]; }
    init_tables(PT, PP); }
#endif
  f = std::fopen(argv[2], "rb"); std::fseek(f, 0, SEEK_END); long N = std::ftell(f) / 8; std::fseek(f, 0, SEEK_SET);
  vec x(N), out(N); if (std::fread(x.data(), 8, N, f) != (size_t)N) return 3; std::fclose(f);
  auto t0 = std::chrono::steady_clock::now();
  for (long t = 0; t < N; ++t) {
    for (int ci = 0; ci < nc; ++ci) {
      Cluster& c = C[ci];
      for (int k = 0; k < c.nin; ++k) {
        int q = (int)c.in_src[2 * k], o = (int)c.in_src[2 * k + 1];
        double val;
        if (q < 0) val = x[t];
        else if (q < ci) val = c.scale[k] * C[q].y[o];
        else val = c.scale[k] * (C[q].extrap ? 2.0 * C[q].y[o] - C[q].yprev[o] : C[q].y[o]);
        c.u[c.in_idx[k]] = val;
      }
#ifdef PROFILE
      auto ta = std::chrono::steady_clock::now();
#endif
      mv(c.Pv, c.u.data(), c.p.data(), c.np, c.nu, false); mv(c.Ps, c.s.data(), c.p.data(), c.np, c.ns, true);
      for (int a = 0; a < c.np; ++a) { double g = 2.0 * c.v[a] - c.vprev[a]; c.vprev[a] = c.v[a]; c.v[a] = g; }
#ifdef PROFILE
      auto tb = std::chrono::steady_clock::now();
#endif
      int it = c.newton(c.p.data(), tol, 40);
#ifdef PROFILE
      auto tc = std::chrono::steady_clock::now();
#endif
#ifdef ADAPTIVE_FALLBACK
      // Continuation in the excitation with step-size control (same rule as kernel_fast.cpp). With the model-v2 output
      // stage the prototype's two fallbacks below leave some samples unconverged (counted as fails) and the output
      // wrong there; this one converges on all bench cases and is what the fast kernel is validated against.
      if (it < 0) {
        c.iters += 40; c.v = c.vprev; vec ph(c.np), vs(c.np); double lam = 0.0, h = 0.25; bool ok = true;
        while (lam < 1.0) {
          double ln = lam + h < 1.0 ? lam + h : 1.0;
          for (int a = 0; a < c.np; ++a) ph[a] = c.plast[a] + (c.p[a] - c.plast[a]) * ln;
          vs = c.v; int k = c.newton(ph.data(), tol, 20); c.iters += k < 0 ? 20 : k;
          if (k < 0) { c.v = vs; h *= 0.5; if (h < 1.0 / 1024) { ok = false; break; } } else { lam = ln; h *= 2.0; }
        }
        if (!ok) { c.fails++; c.v = c.vprev; c.newton(c.p.data(), tol, 40); }
        it = 0;
      }
#endif
      if (it < 0) {                                  // retry from the previous solution
        c.v = c.vprev; it = c.newton(c.p.data(), tol, 160); c.iters += 40;
      }
      if (it < 0) {                                  // continuation in the excitation
        c.v = c.vprev; bool ok = true; vec ph(c.np);
        for (int st = 1; st <= 8; ++st) { for (int a = 0; a < c.np; ++a) ph[a] = c.plast[a] + (c.p[a] - c.plast[a]) * st / 8.0;
          if (c.newton(ph.data(), tol, 80) < 0) ok = false; c.iters += 4; }
        if (!ok) c.fails++;
      } else c.iters += it;
      c.plast = c.p;
      c.yprev = c.y;
      mv(c.Yv, c.u.data(), c.y.data(), c.ny, c.nu, false); mv(c.Ys, c.s.data(), c.y.data(), c.ny, c.ns, true); mv(c.Yi, c.i.data(), c.y.data(), c.ny, c.np, true);
      mv(c.Ss, c.s.data(), c.sn.data(), c.ns, c.ns, false); mv(c.Sv, c.u.data(), c.sn.data(), c.ns, c.nu, true); mv(c.Si, c.i.data(), c.sn.data(), c.ns, c.np, true);
      c.s.swap(c.sn);
#ifdef PROFILE
      auto td = std::chrono::steady_clock::now();
      c.t_newton += std::chrono::duration<double>(tc - tb).count(); c.t_lin += std::chrono::duration<double>(tb - ta).count() + std::chrono::duration<double>(td - tc).count();
#endif
    }
    out[t] = C[hdr[1]].y[hdr[2]];
  }
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("samples %ld  time %.3f s  %.1f ns/sample\n", N, el, el / N * 1e9);
  for (int ci = 0; ci < nc; ++ci) std::printf("  cluster %d ports %d states %d  iters/sample %.2f evals/sample %.2f fails %ld  newton %.0f ns  linear %.0f ns\n", ci, C[ci].np, C[ci].ns, (double)C[ci].iters / N, (double)C[ci].evals / N, C[ci].fails, C[ci].t_newton / N * 1e9, C[ci].t_lin / N * 1e9);
  f = std::fopen(argv[3], "wb"); std::fwrite(out.data(), 8, N, f); std::fclose(f);
  return 0;
}
