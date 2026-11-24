#include "pedal/PedalCluster.h"

#include <algorithm>
#include <cmath>

#include "solver/DenseLU.h"

namespace magmora::pedal {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kExpMax = 40.0;    // pedal.EXP_MAX
constexpr double kDcLeak = 1e-18;   // dk.DC_CAP_LEAK: a capacitor at DC
constexpr double kPrewarpHz = 20000.0;

inline void guardedExp(double a, double& e, double& d) {
  if (a > kExpMax) {
    const double m = std::exp(kExpMax);
    e = m * (1.0 + a - kExpMax);
    d = m;
  } else {
    e = std::exp(a);
    d = e;
  }
}

// SPICE's pnjlim: a step into forward conduction is damped logarithmically above the critical voltage.
double limitJunction(double next, double old, double nvt, double is) {
  const double vcrit = nvt * std::log(nvt / (1.41421356237309505 * is));
  if (next > vcrit && std::fabs(next - old) > 2.0 * nvt) {
    if (old > 0.0) {
      const double arg = 1.0 + (next - old) / nvt;
      return arg > 0.0 ? old + nvt * std::log(arg) : vcrit;
    }
    return nvt * std::log(next / nvt);
  }
  return next;
}

}  // namespace

void evaluate(int kind, const double* P, double u, double& i, double& g) {
  switch (kind) {
    case 0: {  // diode pair
      const double x = u / P[1];
      if (P[1] == P[3] && std::fabs(x) < kExpMax) {  // one exponential: exp(-x) = 1 / exp(x)
        const double e = std::exp(x), r = 1.0 / e;
        i = P[0] * (e - 1.0) - P[2] * (r - 1.0);
        g = (P[0] * e + P[2] * r) / P[1];
        return;
      }
      double ea, da, eb, db;
      guardedExp(u / P[1], ea, da);
      guardedExp(-u / P[3], eb, db);
      i = P[0] * (ea - 1.0) - P[2] * (eb - 1.0);
      g = P[0] * da / P[1] + P[2] * db / P[3];
      return;
    }
    case 1: {  // the op-amp's input pair
      const double t = std::tanh((u + P[2]) / P[1]);
      i = P[0] * t;
      g = P[0] * (1.0 - t * t) / P[1];
      return;
    }
    case 2: {  // the op-amp's output clamp
      double ea, da, eb, db;
      guardedExp((u - P[2]) / P[1], ea, da);
      guardedExp((P[3] - u) / P[1], eb, db);
      i = P[0] * (ea - eb);
      g = P[0] * (da + db) / P[1];
      return;
    }
    default: {  // a junction
      double e, d;
      guardedExp(u / P[1], e, d);
      i = P[0] * e;
      g = P[0] * d / P[1];
    }
  }
}

void PedalCluster::prepare(const PedalClusterDef& def, double rate) {
  def_ = &def;
  rate_ = rate;
  n_ = def.nDevices;
  ny_ = def.nOutputs;
  ns_ = nu_ = 0;
  int branches = 0;
  extra_.assign(static_cast<size_t>(def.nElements), -1);
  sourceOf_.assign(static_cast<size_t>(def.nElements), -1);
  u0_.clear();
  gs_.clear();
  in_ = -1;
  for (int e = 0; e < def.nElements; ++e) {
    const PedalElement& el = def.elements[e];
    if (el.kind == 'V') extra_[static_cast<size_t>(e)] = def.nodes + branches++;
    if (el.kind == 'V' || el.kind == 'I') {
      if (el.input) in_ = nu_;
      sourceOf_[static_cast<size_t>(e)] = nu_++;
      u0_.push_back(el.value);
    }
    if (el.kind == 'C') ++ns_;
  }
  nx_ = def.nodes + branches;
  gs_.assign(static_cast<size_t>(ns_), 0.0);
  knobs_.assign(6, 1.0);
  const size_t nx = static_cast<size_t>(nx_), m = static_cast<size_t>(nu_ + ns_ + n_);
  G_.assign(nx * nx, 0.0);
  lu_.assign(nx * nx, 0.0);
  rhs_.assign(nx * m, 0.0);
  X_.assign(nx * m, 0.0);
  corr_.assign(nx * m, 0.0);
  work_.assign(nx * m, 0.0);
  pivot_.assign(nx, 0);
  const size_t n = static_cast<size_t>(n_), ns = static_cast<size_t>(ns_), ny = static_cast<size_t>(ny_);
  K_.assign(n * n, 0.0);
  dcKeep_.assign(n * n, 0.0);
  dcX_.assign(nx, 0.0);
  Pin_.assign(n, 0.0);
  P0_.assign(n, 0.0);
  Ps_.assign(n * ns, 0.0);
  Yin_.assign(ny, 0.0);
  Y0_.assign(ny, 0.0);
  Ys_.assign(ny * ns, 0.0);
  Yi_.assign(ny * n, 0.0);
  Sin_.assign(ns, 0.0);
  S0_.assign(ns, 0.0);
  Ss_.assign(ns * ns, 0.0);
  Si_.assign(ns * n, 0.0);
  y_.assign(ny, 0.0);
  std::vector<double>* sets[12] = {&K_, &Pin_, &P0_, &Ps_, &Yin_, &Y0_, &Ys_, &Yi_, &Sin_, &S0_, &Ss_, &Si_};
  size_t total = 0;
  for (int k = 0; k < 12; ++k) {
    sets_[k] = sets[k];
    total += sets[k]->size();
  }
  from_.assign(total, 0.0);
  to_.assign(total, 0.0);
  for (int a = 0; a < n_; ++a) {
    kind_[a] = def.devices[a].kind;
    par_[a] = def.devices[a].par;
  }
  // a transistor: ports 0 and 1 are its collector and base parts, the same junction law
  transistor_ = n_ >= 2 && kind_[0] == 3 && kind_[1] == 3 && par_[0][1] == par_[1][1];
}

void PedalCluster::assemble(double* G, bool dc) const {
  const int nx = nx_;
  std::fill(G, G + static_cast<size_t>(nx * nx), 0.0);
  const double T = 1.0 / rate_;
  const double warp = (kPi * kPrewarpHz / rate_) / std::tan(kPi * kPrewarpHz / rate_);
  auto stamp = [&](int a, int b, double g) {
    if (a >= 0) G[a * nx + a] += g;
    if (b >= 0) G[b * nx + b] += g;
    if (a >= 0 && b >= 0) {
      G[a * nx + b] -= g;
      G[b * nx + a] -= g;
    }
  };
  for (int e = 0; e < def_->nElements; ++e) {
    const PedalElement& el = def_->elements[e];
    switch (el.kind) {
      case 'R': stamp(el.a, el.b, 1.0 / (el.knob ? knobs_[static_cast<size_t>(el.knob - 1)] : el.value)); break;
      case 'C': stamp(el.a, el.b, dc ? kDcLeak : 2.0 * el.value * (el.prewarp ? warp : 1.0) / T); break;
      case 'V': {
        const int j = extra_[static_cast<size_t>(e)];
        for (auto [node, sign] : {std::pair{el.a, 1.0}, std::pair{el.b, -1.0}})
          if (node >= 0) {
            G[node * nx + j] += sign;
            G[j * nx + node] += sign;
          }
        break;
      }
      case 'G':
        for (auto [node, sign] : {std::pair{el.a, 1.0}, std::pair{el.b, -1.0}}) {
          if (node < 0) continue;
          if (el.cp >= 0) G[node * nx + el.cp] += sign * el.value;
          if (el.cn >= 0) G[node * nx + el.cn] -= sign * el.value;
        }
        break;
      default: break;
    }
  }
}

void PedalCluster::build(const double* knobOhms, bool refined) {
  for (int k = 0; k < 6; ++k) knobs_[static_cast<size_t>(k)] = knobOhms[k];
  const int nx = nx_, m = nu_ + ns_ + n_;
  const double T = 1.0 / rate_;
  const double warp = (kPi * kPrewarpHz / rate_) / std::tan(kPi * kPrewarpHz / rate_);
  assemble(G_.data(), false);
  std::copy(G_.begin(), G_.end(), lu_.begin());
  solver::luFactor(lu_.data(), pivot_.data(), nx);
  // right-hand sides [Bv | Bs | Bi]
  std::fill(rhs_.begin(), rhs_.end(), 0.0);
  int state = 0;
  auto put = [&](int row, int col, double v) {
    if (row >= 0) rhs_[static_cast<size_t>(row * m + col)] += v;
  };
  for (int e = 0; e < def_->nElements; ++e) {
    const PedalElement& el = def_->elements[e];
    const int src = sourceOf_[static_cast<size_t>(e)];
    if (el.kind == 'V') put(extra_[static_cast<size_t>(e)], src, 1.0);
    if (el.kind == 'I') {
      put(el.a, src, -1.0);
      put(el.b, src, 1.0);
    }
    if (el.kind == 'C') {
      put(el.a, nu_ + state, 1.0);
      put(el.b, nu_ + state, -1.0);
      gs_[static_cast<size_t>(state)] = 2.0 * el.value * (el.prewarp ? warp : 1.0) / T;
      ++state;
    }
  }
  for (int k = 0; k < n_; ++k)
    for (int j = 0; j < 2; ++j) put(def_->devices[k].injNode[j], nu_ + ns_ + k, def_->devices[k].injCoef[j]);
  std::copy(rhs_.begin(), rhs_.end(), X_.begin());
  solver::luSolve(lu_.data(), pivot_.data(), nx, X_.data(), m, m);
  if (refined) {
    std::copy(rhs_.begin(), rhs_.end(), work_.begin());
    solver::luRefine(G_.data(), lu_.data(), pivot_.data(), nx, X_.data(), work_.data(), corr_.data(), m);
  }
  // rows: device controls, capacitor voltages, outputs; each times X
  auto rowTimes = [&](auto&& row, double* out) {  // out[m] = row . X
    std::fill(out, out + m, 0.0);
    row([&](int node, double coef) {
      if (node < 0 || coef == 0.0) return;
      const double* x = X_.data() + static_cast<size_t>(node * m);
      for (int c = 0; c < m; ++c) out[c] += coef * x[c];
    });
  };
  double line[64];
  auto fold = [&](const double* l, double& c0, double& cin) {
    c0 = 0.0;
    for (int c = 0; c < nu_; ++c)
      if (c != in_) c0 += l[c] * u0_[static_cast<size_t>(c)];
    cin = in_ >= 0 ? l[in_] : 0.0;
  };
  for (int a = 0; a < n_; ++a) {
    const PedalDevice& d = def_->devices[a];
    rowTimes([&](auto&& f) { for (int j = 0; j < 3; ++j) f(d.ctrlNode[j], d.ctrlCoef[j]); }, line);
    fold(line, P0_[static_cast<size_t>(a)], Pin_[static_cast<size_t>(a)]);
    for (int b = 0; b < ns_; ++b) Ps_[static_cast<size_t>(a * ns_ + b)] = line[nu_ + b];
    for (int b = 0; b < n_; ++b) K_[static_cast<size_t>(a * n_ + b)] = line[nu_ + ns_ + b];
  }
  state = 0;
  for (int e = 0; e < def_->nElements; ++e) {
    const PedalElement& el = def_->elements[e];
    if (el.kind != 'C') continue;
    rowTimes([&](auto&& f) { f(el.a, 1.0); f(el.b, -1.0); }, line);
    const double g2 = 2.0 * gs_[static_cast<size_t>(state)];
    for (int c = 0; c < m; ++c) line[c] *= g2;
    fold(line, S0_[static_cast<size_t>(state)], Sin_[static_cast<size_t>(state)]);
    for (int b = 0; b < ns_; ++b) Ss_[static_cast<size_t>(state * ns_ + b)] = line[nu_ + b] - (b == state ? 1.0 : 0.0);
    for (int b = 0; b < n_; ++b) Si_[static_cast<size_t>(state * n_ + b)] = line[nu_ + ns_ + b];
    ++state;
  }
  for (int k = 0; k < ny_; ++k) {
    const PedalOutput& o = def_->outputs[k];
    rowTimes([&](auto&& f) { f(o.plus, 1.0); f(o.minus, -1.0); }, line);
    fold(line, Y0_[static_cast<size_t>(k)], Yin_[static_cast<size_t>(k)]);
    for (int b = 0; b < ns_; ++b) Ys_[static_cast<size_t>(k * ns_ + b)] = line[nu_ + b];
    for (int b = 0; b < n_; ++b) Yi_[static_cast<size_t>(k * n_ + b)] = line[nu_ + ns_ + b];
  }
}

// All ports. A transistor's collector part is its base part times exp((u_c - u_b) / nVT), and the two controls
// differ by k (v(vcc) - v(b)), a small argument: one exponential per transistor (spec 8), the small one as a series
// (|d| < 0.15: truncation under 1e-9 relative).
void PedalCluster::evaluatePorts(double* g) {
  int a = 0;
  if (transistor_) {
    const double* Pc = par_[0];
    const double* Pb = par_[1];
    const double nvt = Pb[1], xb = v_[1] / nvt, d = (v_[0] - v_[1]) / nvt;
    if (xb < kExpMax && xb + d < kExpMax && std::fabs(d) < 0.15) {
      const double eb = std::exp(xb);
      const double r = 1.0 + d * (1.0 + d * (0.5 + d * (1.0 / 6 + d * (1.0 / 24 + d * (1.0 / 120 + d / 720)))));
      cur_[1] = Pb[0] * eb;
      g[1] = cur_[1] / nvt;
      cur_[0] = Pc[0] * eb * r;
      g[0] = cur_[0] / nvt;
      a = 2;
    }
  }
  for (; a < n_; ++a) evaluate(kind_[a], par_[a], v_[a], cur_[a], g[a]);
}

int PedalCluster::newton(const double* p, const NewtonSettings& st) {
  switch (n_) {
    case 1: return newtonFixed<1>(p, st);
    case 2: return newtonFixed<2>(p, st);
    case 3: return newtonFixed<3>(p, st);
    default: return 1;
  }
}

template <int N>
int PedalCluster::newtonFixed(const double* p, const NewtonSettings& st) {
  // pedal.newton2: v = p + K i(v), at most three ports
  double g[kMaxPorts], M[kMaxPorts][kMaxPorts], F[kMaxPorts];
  constexpr int n = N;
  const double* K = K_.data();
  for (int it = 0; it < st.maxIterations; ++it) {
    evaluatePorts(g);
    for (int a = 0; a < n; ++a) {
      double acc = p[a] - v_[a];
      for (int b = 0; b < n; ++b) {
        acc += K[a * n + b] * cur_[b];
        M[a][b] = -K[a * n + b] * g[b];
      }
      M[a][a] += 1.0;
      F[a] = acc;
    }
    for (int a = 0; a < n; ++a) {  // Gaussian elimination with partial pivoting
      int q = a;
      double big = std::fabs(M[a][a]);
      for (int b = a + 1; b < n; ++b)
        if (std::fabs(M[b][a]) > big) {
          big = std::fabs(M[b][a]);
          q = b;
        }
      if (q != a) {
        for (int c = 0; c < n; ++c) std::swap(M[a][c], M[q][c]);
        std::swap(F[a], F[q]);
      }
      for (int b = a + 1; b < n; ++b) {
        const double f = M[b][a] / M[a][a];
        if (f != 0.0) {
          for (int c = a; c < n; ++c) M[b][c] -= f * M[a][c];
          F[b] -= f * F[a];
        }
      }
    }
    for (int a = n - 1; a >= 0; --a) {
      double acc = F[a];
      for (int c = a + 1; c < n; ++c) acc -= M[a][c] * F[c];
      F[a] = acc / M[a][a];
    }
    if (st.junctionLimiting) {
      const double relax = it >= 25 ? 0.25 : (it >= 10 ? 0.5 : 1.0);
      double largest = 0.0, applied[kMaxPorts];
      for (int a = 0; a < n; ++a) {
        const double old = v_[a];
        double next = old + relax * F[a];
        const double* P = par_[a];
        if (kind_[a] == 1) {  // tanh: at most three of its own scales
          const double lim = 3.0 * P[1];
          next = old + std::clamp(next - old, -lim, lim);
        } else if (kind_[a] == 3) {
          next = limitJunction(next, old, P[1], P[0]);
        } else if (kind_[a] == 0) {  // the pair: each direction is a junction
          next = next >= 0.0 ? limitJunction(next, old, P[1], P[0]) : -limitJunction(-next, -old, P[3], P[2]);
        } else if (kind_[a] == 2) {  // the clamp: a junction above Vhi and one below Vlo
          if (next > P[2]) next = P[2] + limitJunction(next - P[2], old - P[2], P[1], P[0]);
          else if (next < P[3]) next = P[3] - limitJunction(P[3] - next, P[3] - old, P[1], P[0]);
        }
        applied[a] = next - old;
        v_[a] = next;
        largest = std::max(largest, std::fabs(applied[a]));
      }
      if (largest < st.tolerance) {
        if (st.fastAccept)
          for (int a = 0; a < n; ++a) cur_[a] += g[a] * applied[a];
        else
          for (int a = 0; a < n; ++a) evaluate(kind_[a], par_[a], v_[a], cur_[a], g[a]);
        return it + 1;
      }
      continue;
    }
    double sc = it >= 25 ? 0.25 : (it >= 10 ? 0.5 : 1.0), largest = 0.0;
    for (int a = 0; a < n; ++a) {
      double lim = 1e9;
      if (kind_[a] == 0) lim = 0.15;
      else if (kind_[a] == 3) lim = 0.10;
      else if (kind_[a] == 2) lim = 0.5;
      else if (kind_[a] == 1) lim = 3.0 * par_[a][1];
      if (std::fabs(F[a]) > lim) sc = std::min(sc, lim / std::fabs(F[a]));
    }
    for (int a = 0; a < n; ++a) {
      v_[a] += sc * F[a];
      largest = std::max(largest, std::fabs(sc * F[a]));
    }
    if (largest < st.tolerance) {
      if (st.fastAccept)
        for (int a = 0; a < n; ++a) cur_[a] += g[a] * sc * F[a];
      else
        for (int a = 0; a < n; ++a) evaluate(kind_[a], par_[a], v_[a], cur_[a], g[a]);
      return it + 1;
    }
  }
  return -st.maxIterations;
}

bool PedalCluster::operatingPoint(double input, double* s) {
  // pedal.dc2: the DC network (capacitors open), sources stepped up in 12 steps
  const int nx = nx_, m = nu_ + ns_ + n_;
  u0_[static_cast<size_t>(in_)] = input;
  build(knobs_.data(), true);  // fills the right-hand sides rhs_
  assemble(G_.data(), true);
  std::copy(G_.begin(), G_.end(), lu_.begin());
  if (!solver::luFactor(lu_.data(), pivot_.data(), nx)) return false;
  std::copy(rhs_.begin(), rhs_.end(), X_.begin());
  solver::luSolve(lu_.data(), pivot_.data(), nx, X_.data(), m, m);
  std::copy(rhs_.begin(), rhs_.end(), work_.begin());
  solver::luRefine(G_.data(), lu_.data(), pivot_.data(), nx, X_.data(), work_.data(), corr_.data(), m);
  // DC K and p
  double Kdc[kMaxPorts * kMaxPorts] = {}, Pdc[kMaxPorts] = {};
  auto at = [&](int node, int col) { return node < 0 ? 0.0 : X_[static_cast<size_t>(node * m + col)]; };
  for (int a = 0; a < n_; ++a) {
    const PedalDevice& d = def_->devices[a];
    for (int j = 0; j < 3; ++j) {
      for (int b = 0; b < n_; ++b) Kdc[a * n_ + b] += d.ctrlCoef[j] * at(d.ctrlNode[j], nu_ + ns_ + b);
      for (int c = 0; c < nu_; ++c) Pdc[a] += d.ctrlCoef[j] * at(d.ctrlNode[j], c) * u0_[static_cast<size_t>(c)];
    }
  }
  std::copy(K_.begin(), K_.end(), dcKeep_.begin());
  std::copy(Kdc, Kdc + n_ * n_, K_.begin());
  for (int a = 0; a < n_; ++a) v_[a] = 0.0;
  const NewtonSettings dc{1e-11, 400, false};
  bool ok = true;
  for (int k = 1; k <= 12; ++k) {
    double p[kMaxPorts];
    for (int a = 0; a < n_; ++a) p[a] = Pdc[a] * k / 12.0;
    ok = ok && newton(p, dc) > 0;
  }
  if (n_ == 0) ok = true;
  std::copy(dcKeep_.begin(), dcKeep_.end(), K_.begin());
  // the node solution, then the outputs and the states
  std::vector<double>& x = dcX_;
  for (int r = 0; r < nx; ++r) {
    double acc = 0.0;
    for (int c = 0; c < nu_; ++c) acc += X_[static_cast<size_t>(r * m + c)] * u0_[static_cast<size_t>(c)];
    for (int b = 0; b < n_; ++b) acc += X_[static_cast<size_t>(r * m + nu_ + ns_ + b)] * cur_[b];
    x[static_cast<size_t>(r)] = acc;
  }
  auto node = [&](int i) { return i < 0 ? 0.0 : x[static_cast<size_t>(i)]; };
  for (int k = 0; k < ny_; ++k) y_[static_cast<size_t>(k)] = node(def_->outputs[k].plus) - node(def_->outputs[k].minus);
  int state = 0;
  for (int e = 0; e < def_->nElements; ++e) {
    const PedalElement& el = def_->elements[e];
    if (el.kind != 'C') continue;
    s[state] = gs_[static_cast<size_t>(state)] * (node(el.a) - node(el.b));
    ++state;
  }
  for (int a = 0; a < n_; ++a) {
    vPrev_[a] = vLast_[a] = v_[a];
    curLast_[a] = cur_[a];
  }
  build(knobs_.data(), true);  // back to the transient matrices, with this input as the folded DC
  for (int a = 0; a < n_; ++a) {
    double acc = P0_[static_cast<size_t>(a)] + Pin_[static_cast<size_t>(a)] * input;
    for (int b = 0; b < ns_; ++b) acc += Ps_[static_cast<size_t>(a * ns_ + b)] * s[b];
    pLast_[a] = acc;
  }
  return ok;
}

// The per-sample work, with the sizes known at compile time for the six cluster forms (unrolled loops); any other
// size takes the general path.
template <int N, int NS, int NY>
int PedalCluster::solveFixed(double u, const double* s, const NewtonSettings& st, const double* seed) {
  const double* P0 = P0_.data();
  const double* Pin = Pin_.data();
  const double* Ps = Ps_.data();
  for (int a = 0; a < N; ++a) {
    double acc = P0[a] + Pin[a] * u;
    for (int b = 0; b < NS; ++b) acc += Ps[a * NS + b] * s[b];
    p_[a] = acc;
    if (seed) {
      vPrev_[a] = v_[a];
      v_[a] = seed[a];
    } else {
      const double guess = 2.0 * v_[a] - vPrev_[a];
      vPrev_[a] = v_[a];
      v_[a] = guess;
    }
  }
  const int it = N > 0 ? newton(p_, st) : 1;
  const double* Y0 = Y0_.data();
  const double* Yin = Yin_.data();
  const double* Ys = Ys_.data();
  const double* Yi = Yi_.data();
  double* y = y_.data();
  for (int k = 0; k < NY; ++k) {
    double acc = Y0[k] + Yin[k] * u;
    for (int b = 0; b < NS; ++b) acc += Ys[k * NS + b] * s[b];
    for (int b = 0; b < N; ++b) acc += Yi[k * N + b] * cur_[b];
    y[k] = acc;
  }
  uLast_ = u;
  sLast_ = s;
  return it;
}

template <int N, int NS>
void PedalCluster::commitFixed(double u, double* s) {
  double next[NS > 0 ? NS : 1];
  const double* S0 = S0_.data();
  const double* Sin = Sin_.data();
  const double* Ss = Ss_.data();
  const double* Si = Si_.data();
  for (int k = 0; k < NS; ++k) {
    double acc = S0[k] + Sin[k] * u;
    for (int b = 0; b < NS; ++b) acc += Ss[k * NS + b] * s[b];
    for (int b = 0; b < N; ++b) acc += Si[k * N + b] * cur_[b];
    next[k] = acc;
  }
  for (int k = 0; k < NS; ++k) s[k] = next[k];
  for (int a = 0; a < N; ++a) {
    vLast_[a] = v_[a];
    curLast_[a] = cur_[a];
    pLast_[a] = p_[a];
  }
}

#define MAGMORA_PEDAL_SIZES(X) X(2, 2, 2) X(3, 6, 1) X(1, 6, 3) X(2, 5, 1) X(0, 5, 3) X(2, 3, 2)

int PedalCluster::solve(double u, const double* s, const NewtonSettings& st, const double* seed) {
#define MAGMORA_CASE(n, ns, ny) \
  if (n_ == n && ns_ == ns && ny_ == ny) return solveFixed<n, ns, ny>(u, s, st, seed);
  MAGMORA_PEDAL_SIZES(MAGMORA_CASE)
#undef MAGMORA_CASE
  for (int a = 0; a < n_; ++a) {
    double acc = P0_[static_cast<size_t>(a)] + Pin_[static_cast<size_t>(a)] * u;
    for (int b = 0; b < ns_; ++b) acc += Ps_[static_cast<size_t>(a * ns_ + b)] * s[b];
    p_[a] = acc;
    if (seed) {
      vPrev_[a] = v_[a];
      v_[a] = seed[a];
    } else {
      const double guess = 2.0 * v_[a] - vPrev_[a];
      vPrev_[a] = v_[a];
      v_[a] = guess;
    }
  }
  const int it = n_ > 0 ? newton(p_, st) : 1;
  for (int k = 0; k < ny_; ++k) {
    double acc = Y0_[static_cast<size_t>(k)] + Yin_[static_cast<size_t>(k)] * u;
    for (int b = 0; b < ns_; ++b) acc += Ys_[static_cast<size_t>(k * ns_ + b)] * s[b];
    for (int b = 0; b < n_; ++b) acc += Yi_[static_cast<size_t>(k * n_ + b)] * cur_[b];
    y_[static_cast<size_t>(k)] = acc;
  }
  uLast_ = u;
  sLast_ = s;
  return it;
}

bool PedalCluster::recover(const NewtonSettings& st, int& iterations, int steps) {
  bool ok = true;
  for (int a = 0; a < n_; ++a) v_[a] = vLast_[a];
  for (int k = 1; k <= steps; ++k) {
    double pw[kMaxPorts];
    for (int a = 0; a < n_; ++a) pw[a] = pLast_[a] + (p_[a] - pLast_[a]) * k / steps;
    const int it = newton(pw, st);
    iterations += std::abs(it);
    if (it < 0) ok = false;
  }
  refreshOutputs();
  return ok;
}

void PedalCluster::keepLast() {
  for (int a = 0; a < n_; ++a) {
    v_[a] = vLast_[a];
    cur_[a] = curLast_[a];
  }
  refreshOutputs();
}

void PedalCluster::pack(double* out) const {
  for (const std::vector<double>* v : sets_) out = std::copy(v->begin(), v->end(), out);
}

void PedalCluster::blend(double t) {
  const double* a = from_.data();
  const double* b = to_.data();
  for (std::vector<double>* v : sets_)
    for (double& x : *v) {
      x = *a + t * (*b - *a);
      ++a;
      ++b;
    }
}

void PedalCluster::acceptIterate() {
  for (int a = 0; a < n_; ++a) {
    double g;
    evaluate(kind_[a], par_[a], v_[a], cur_[a], g);
  }
  refreshOutputs();
}

void PedalCluster::refreshOutputs() {
  for (int k = 0; k < ny_; ++k) {
    double acc = Y0_[static_cast<size_t>(k)] + Yin_[static_cast<size_t>(k)] * uLast_;
    for (int b = 0; b < ns_; ++b) acc += Ys_[static_cast<size_t>(k * ns_ + b)] * sLast_[b];
    for (int b = 0; b < n_; ++b) acc += Yi_[static_cast<size_t>(k * n_ + b)] * cur_[b];
    y_[static_cast<size_t>(k)] = acc;
  }
}

void PedalCluster::commit(double u, double* s) {
#define MAGMORA_CASE(n, ns, ny) \
  if (n_ == n && ns_ == ns) return commitFixed<n, ns>(u, s);
  MAGMORA_PEDAL_SIZES(MAGMORA_CASE)
#undef MAGMORA_CASE
  double next[16];
  for (int k = 0; k < ns_; ++k) {
    double acc = S0_[static_cast<size_t>(k)] + Sin_[static_cast<size_t>(k)] * u;
    for (int b = 0; b < ns_; ++b) acc += Ss_[static_cast<size_t>(k * ns_ + b)] * s[b];
    for (int b = 0; b < n_; ++b) acc += Si_[static_cast<size_t>(k * n_ + b)] * cur_[b];
    next[k] = acc;
  }
  for (int k = 0; k < ns_; ++k) s[k] = next[k];
  for (int a = 0; a < n_; ++a) {
    vLast_[a] = v_[a];
    curLast_[a] = cur_[a];
    pLast_[a] = p_[a];
  }
}

}  // namespace magmora::pedal
