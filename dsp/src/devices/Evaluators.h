// How a solver evaluates all devices of one cluster: exactly (ClosedLaws) or from tables, two tubes at a time.
//
// Jacobian layout: triode (dpp, dpg, dgg); pentode (dpp, dps, dpg, dsp, dss, dsg, dgg); diode (g). These are all
// the entries that are not identically zero.
#pragma once
#include "circuit/Netlist.h"
#include "devices/Tubes.h"
#include "solver/Shape.h"

namespace magmora::devices {

constexpr int kTriodeJacobian = 3;
constexpr int kPentodeJacobian = 7;

namespace detail {

MAGMORA_INLINE void storeTriode(const double* out, double* i, double* J) {
  i[0] = out[0];
  i[1] = out[1];
  J[0] = out[2];
  J[1] = out[3];
  J[2] = out[5];
}

MAGMORA_INLINE void storePentode(const double* out, double* i, double* J) {
  for (int k = 0; k < 3; ++k) i[k] = out[k];
  for (int k = 0; k < 6; ++k) J[k] = out[3 + k];
  J[6] = out[11];
}

MAGMORA_NOINLINE inline void exactTriode(const TriodeLaw& law, const double* v, double* i, double* J) {
  double out[6];
  triode(law.p, v[0], v[1], out);
  storeTriode(out, i, J);
}

MAGMORA_NOINLINE inline void exactPentode(const PentodeLaw& law, const double* v, double* i, double* J) {
  double out[12];
  pentode(law.p, v[0], v[1], v[2], out);
  storePentode(out, i, J);
}

template <class V, class Law, class Fn>
V perLane(const Law* const* laws, Fn fn) {
  double a[V::kLanes];
  for (int l = 0; l < V::kLanes; ++l) a[l] = fn(laws[l]->p);
  return V::load(a);
}

// One or two triodes, evaluated together.
template <class V>
struct TriodeBlock {
  const TriodeLaw* law[V::kLanes];
  V kp, kvb, vct, inverseMu, inverseKp, scale, gridMax, powerMax;
  SoftplusTable::View<V> softplusView;
  PowerTable::View<V> power;
  GridTable::View<V> grid;

  void bind(const TriodeLaw* const* laws, const Library& library) {
    using P = TriodeParams;
    const SoftplusTable* soft[V::kLanes];
    const PowerTable* powers[V::kLanes];
    const GridTable* grids[V::kLanes];
    for (int l = 0; l < V::kLanes; ++l) {
      law[l] = laws[l];
      soft[l] = &library.softplusTable;
      powers[l] = &laws[l]->power;
      grids[l] = &laws[l]->grid;
    }
    kp = perLane<V>(laws, [](const P& p) { return p.kp; });
    kvb = perLane<V>(laws, [](const P& p) { return p.kvb; });
    vct = perLane<V>(laws, [](const P& p) { return p.vct; });
    inverseMu = perLane<V>(laws, [](const P& p) { return 1.0 / p.mu; });
    inverseKp = perLane<V>(laws, [](const P& p) { return 1.0 / p.kp; });
    scale = perLane<V>(laws, [](const P& p) { return 2.0 / p.kg1; });
    gridMax = V::set(kGridTableMax);
    powerMax = V::set(PowerTable::max());
    softplusView.bind(soft);
    power.bind(powers);
    grid.bind(grids);
  }

  MAGMORA_INLINE void eval(const double* v, double* i, double* J) const {
    const V vgk = V::gather(v + 1, 2);
    const V vp = max(V::gather(v, 2), V::set(0.0));  // no plate current at or below zero: e1 = 0 is segment 0
    const V vp2 = vp * vp;
    const V ir = V::set(1.0) / sqrt(kvb + vp2);
    const V vg = vgk + vct;
    const V vgr = vg * ir;
    V sp, dsp, pw, dpw, ig, dgg;
    softplusView.eval(kp * (inverseMu + vgr), sp, dsp);
    const V e1 = vp * inverseKp * sp;
    power.eval(e1, pw, dpw);
    const V d = scale * dpw;
    (scale * pw).scatter(i, 2);
    (d * (sp * inverseKp - dsp * vgr * (vp2 * (ir * ir)))).scatter(J, kTriodeJacobian);
    (d * (vp * dsp * ir)).scatter(J + 1, kTriodeJacobian);
    grid.eval(vgk, ig, dgg);
    ig.scatter(i + 1, 2);
    dgg.scatter(J + 2, kTriodeJacobian);
    if (anyGreaterEqual(vgk, gridMax) || anyGreaterEqual(e1, powerMax))
      for (int l = 0; l < V::kLanes; ++l) exactTriode(*law[l], v + 2 * l, i + 2 * l, J + kTriodeJacobian * l);
  }
};

// One or two pentodes, evaluated together.
template <class V>
struct PentodeBlock {
  const PentodeLaw* law[V::kLanes];
  V kp, inverseMu, inverseKp, scale, rs, asc, lam, npar, beta, inverseBeta, gridMax, powerMax, kneeMax;
  SoftplusTable::View<V> softplusView;
  MinimumTable::View<V> minimum;
  PowerTable::View<V> power;
  GridTable::View<V> grid;
  KneeTable::View<V> knee;

  void bind(const PentodeLaw* const* laws, const Library& library) {
    using P = PentodeParams;
    const SoftplusTable* soft[V::kLanes];
    const MinimumTable* minima[V::kLanes];
    const PowerTable* powers[V::kLanes];
    const GridTable* grids[V::kLanes];
    const KneeTable* knees[V::kLanes];
    for (int l = 0; l < V::kLanes; ++l) {
      law[l] = laws[l];
      soft[l] = &library.softplusTable;
      minima[l] = &library.minimumTable;
      powers[l] = &laws[l]->power;
      grids[l] = &laws[l]->grid;
      knees[l] = &laws[l]->knee;
    }
    kp = perLane<V>(laws, [](const P& p) { return p.kp; });
    inverseMu = perLane<V>(laws, [](const P& p) { return 1.0 / p.mu; });
    inverseKp = perLane<V>(laws, [](const P& p) { return 1.0 / p.kp; });
    scale = perLane<V>(laws, [](const P& p) { return 3.14159265358979323846 / p.kg1; });
    rs = perLane<V>(laws, [](const P& p) { return p.rs; });
    asc = perLane<V>(laws, [](const P& p) { return p.asc; });
    lam = perLane<V>(laws, [](const P& p) { return p.lam; });
    npar = perLane<V>(laws, [](const P& p) { return p.npar; });
    beta = V::set(circuit::netdata::kKneeBeta);
    inverseBeta = V::set(1.0 / circuit::netdata::kKneeBeta);
    gridMax = V::set(kGridTableMax);
    powerMax = V::set(PowerTable::max());
    kneeMax = V::set(KneeTable::max());
    softplusView.bind(soft);
    minimum.bind(minima);
    power.bind(powers);
    grid.bind(grids);
    knee.bind(knees);
  }

  MAGMORA_INLINE void eval(const double* v, double* i, double* J) const {
    const V one = V::set(1.0), zero = V::set(0.0), eps = V::set(circuit::netdata::kPentodeEps1);
    const V vpk = V::gather(v, 3), vsk = V::gather(v + 1, 3), vgk = V::gather(v + 2, 3);
    const V vs = max(vsk, one);
    const V dvs = selectGreater(vsk, one, one, zero);
    const V vgs = vgk * (one / vs);
    V sp, dsp, pw, dpw;
    softplusView.eval(kp * (inverseMu + vgs), sp, dsp);
    const V e1 = vs * inverseKp * sp;
    power.eval(e1, pw, dpw);
    const V i0 = scale * pw;
    const V di0 = scale * dpw;
    const V di0s = di0 * (sp * inverseKp - dsp * vgs) * dvs;
    const V di0g = di0 * dsp;

    V vps, dvps, a, da;
    softplusView.eval(beta * vpk, vps, dvps);
    vps = vps * inverseBeta;
    const V shifted = vpk + V::set(kKneeShift);
    knee.eval(shifted, a, da);

    // m = i0 a / (i0^3 + a^3)^(1/3): with r = smaller / bigger that is a one-dimensional function of r, and the two
    // derivatives are exactly the fourth powers of a c and i0 c, c = (i0^3 + a^3)^(-1/3).
    const V inverseBig = one / max(max(i0, a), eps);
    V f, df;
    minimum.eval(min(i0, a) * inverseBig, f, df);
    const V c = f * inverseBig;
    const V ac = a * c, ic = i0 * c;
    const V ac2 = ac * ac, ic2 = ic * ic;
    const V dmDi0 = ac2 * ac2, dmDa = ic2 * ic2;
    const V m = i0 * ac;
    const V lf = madd(one, lam, vps);
    const V inverseDen = one / (i0 + a + eps);
    const V refused = i0 * inverseDen;
    const V share = madd(rs, asc, refused);
    const V disDi0 = madd(rs, asc * refused, (i0 + a + a + eps + eps) * inverseDen);
    const V plateSlope = npar * dmDi0 * lf, screenSlope = npar * disDi0;

    (npar * m * lf).scatter(i, 3);
    (npar * i0 * share).scatter(i + 1, 3);
    (npar * (dmDa * da * lf + m * lam * dvps)).scatter(J, kPentodeJacobian);
    (plateSlope * di0s).scatter(J + 1, kPentodeJacobian);
    (plateSlope * di0g).scatter(J + 2, kPentodeJacobian);
    ((zero - npar) * asc * refused * refused * da).scatter(J + 3, kPentodeJacobian);
    (screenSlope * di0s).scatter(J + 4, kPentodeJacobian);
    (screenSlope * di0g).scatter(J + 5, kPentodeJacobian);
    V ig, dgg;
    grid.eval(vgk, ig, dgg);
    (npar * ig).scatter(i + 2, 3);
    (npar * dgg).scatter(J + 6, kPentodeJacobian);
    if (anyGreaterEqual(vgk, gridMax) || anyGreaterEqual(e1, powerMax) || anyGreaterEqual(shifted, kneeMax))
      for (int l = 0; l < V::kLanes; ++l) exactPentode(*law[l], v + 3 * l, i + 3 * l, J + kPentodeJacobian * l);
  }
};

constexpr int atLeastOne(int n) { return n > 0 ? n : 1; }

}  // namespace detail

template <class Shape>
class ClosedLaws {
 public:
  void bind(const DeviceSet& set) { set_ = &set; }

  MAGMORA_INLINE void eval(const Shape shape, const double* v, double* i, double* J) const {
    for (int d = 0; d < shape.triodes(); ++d) {
      detail::exactTriode(*set_->triodes[static_cast<size_t>(d)], v, i, J);
      v += 2;
      i += 2;
      J += kTriodeJacobian;
    }
    for (int d = 0; d < shape.pentodes(); ++d) {
      detail::exactPentode(*set_->pentodes[static_cast<size_t>(d)], v, i, J);
      if (const DiodeLaw* diode = set_->flyback[static_cast<size_t>(d)]) {
        double id, gd;
        diode->closed(-v[0], id, gd);
        i[0] -= id;
        J[0] += gd;
      }
      v += 3;
      i += 3;
      J += kPentodeJacobian;
    }
    for (int d = 0; d < shape.diodes(); ++d) set_->diodes[static_cast<size_t>(d)]->closed(v[d], i[d], J[d]);
  }

 private:
  const DeviceSet* set_ = nullptr;
};

template <class Shape>
class TableLaws {
 public:
  void bind(const DeviceSet& set) {
    const Library& library = *set.library;
    const int nt = static_cast<int>(set.triodes.size()), np = static_cast<int>(set.pentodes.size());
    for (int d = 0; d < nt / 2; ++d) triodePairs_[d].bind(set.triodes.data() + 2 * d, library);
    if (nt & 1) lastTriode_.bind(set.triodes.data() + nt - 1, library);
    for (int d = 0; d < np / 2; ++d) pentodePairs_[d].bind(set.pentodes.data() + 2 * d, library);
    if (np & 1) lastPentode_.bind(set.pentodes.data() + np - 1, library);
    flyback_ = set.flyback.data();
    diodes_ = set.diodes.data();
  }

  MAGMORA_INLINE void eval(const Shape shape, const double* v, double* i, double* J) const {
    const int nt = shape.triodes(), np = shape.pentodes();
    for (int d = 0; d + 1 < nt; d += 2) triodePairs_[d / 2].eval(v + 2 * d, i + 2 * d, J + kTriodeJacobian * d);
    if (nt & 1) lastTriode_.eval(v + 2 * (nt - 1), i + 2 * (nt - 1), J + kTriodeJacobian * (nt - 1));
    v += 2 * nt;
    i += 2 * nt;
    J += kTriodeJacobian * nt;
    for (int d = 0; d + 1 < np; d += 2) pentodePairs_[d / 2].eval(v + 3 * d, i + 3 * d, J + kPentodeJacobian * d);
    if (np & 1) lastPentode_.eval(v + 3 * (np - 1), i + 3 * (np - 1), J + kPentodeJacobian * (np - 1));
    for (int d = 0; d < np; ++d)
      if (const DiodeLaw* diode = flyback_[d]) {
        double id, gd;
        diode->fast(-v[3 * d], id, gd);
        i[3 * d] -= id;
        J[kPentodeJacobian * d] += gd;
      }
    v += 3 * np;
    i += 3 * np;
    J += kPentodeJacobian * np;
    for (int d = 0; d < shape.diodes(); ++d) diodes_[d]->fast(v[d], i[d], J[d]);
  }

 private:
  detail::TriodeBlock<simd::F2> triodePairs_[detail::atLeastOne(Shape::kMaxTriodes / 2)];
  detail::TriodeBlock<simd::F1> lastTriode_;
  detail::PentodeBlock<simd::F2> pentodePairs_[detail::atLeastOne(Shape::kMaxPentodes / 2)];
  detail::PentodeBlock<simd::F1> lastPentode_;
  const DiodeLaw* const* flyback_ = nullptr;
  const DiodeLaw* const* diodes_ = nullptr;
};

}  // namespace magmora::devices
