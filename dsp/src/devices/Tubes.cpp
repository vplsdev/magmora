#include "devices/Tubes.h"

#include "circuit/Netlist.h"

namespace magmora::devices {
namespace {

using namespace circuit::netdata;

constexpr double kPi = 3.14159265358979323846;

void gridCurrent(double gg, double cg, double xi, double vg0, double vgk, double& ig, double& dgg) {
  double s, ds;
  softplus(cg * (vgk + vg0), s, ds);
  if (s <= 0.0) {
    ig = 0.0;
    dgg = 0.0;
  } else {
    const double base = s / cg;
    ig = gg * std::pow(base, xi);
    dgg = xi * ig / base * ds;
  }
}

void buildPower(PowerTable& table, double ex) {
  table.build([ex](double e, double& y, double& dy) {
    y = std::pow(e, ex);
    dy = ex * y / e;
  });
}

void buildGrid(GridTable& table, double gg, double cg, double xi, double vg0) {
  // starts exactly where the closed form stops conducting
  table.build(-kLnMax / cg - vg0, kGridTableMax, [=](double v, double& y, double& dy) { gridCurrent(gg, cg, xi, vg0, v, y, dy); });
}

template <class Law>
const Law* find(const std::vector<std::unique_ptr<Law>>& laws, const std::array<double, 12>& key) {
  for (const auto& law : laws)
    if (law->key == key) return law.get();
  return nullptr;
}

}  // namespace

TriodeParams TriodeParams::from(const double* p) {
  return {p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]};
}

PentodeParams PentodeParams::from(const double* p) {
  return {p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]};
}

DiodeParams DiodeParams::from(const double* p) { return {p[0], p[1], p[2]}; }

void softplus(double x, double& y, double& dy) {
  if (x > kLnMax) {
    y = x;
    dy = 1.0;
  } else if (x < -kLnMax) {
    y = 0.0;  // below 4e-18: cut off (keeps denormals out of the solver)
    dy = 0.0;
  } else {
    const double e = std::exp(x);
    y = std::log1p(e);
    dy = e / (1.0 + e);
  }
}

void triode(const TriodeParams& p, double vpk, double vgk, double* out) {
  double ip = 0.0, dpp = 0.0, dpg = 0.0;
  if (vpk > 0.0) {
    const double r = std::sqrt(p.kvb + vpk * vpk);
    const double x = p.kp * (1.0 / p.mu + (vgk + p.vct) / r);
    double sp, dsp;
    softplus(x, sp, dsp);
    const double e1 = vpk / p.kp * sp;
    if (e1 > 0.0) {
      ip = 2.0 / p.kg1 * std::pow(e1, p.ex);
      const double dIpDe1 = p.ex * ip / e1;
      const double dxDvpk = -p.kp * (vgk + p.vct) * vpk / (r * r * r);
      dpp = dIpDe1 * (sp / p.kp + vpk / p.kp * dsp * dxDvpk);
      dpg = dIpDe1 * (vpk * dsp / r);
    }
  }
  double ig, dgg;
  gridCurrent(p.gg, p.cg, p.xi, p.vg0, vgk, ig, dgg);
  out[0] = ip;
  out[1] = ig;
  out[2] = dpp;
  out[3] = dpg;
  out[4] = 0.0;
  out[5] = dgg;
}

void pentode(const PentodeParams& p, double vpk, double vsk, double vgk, double* out) {
  const double vs = vsk > 1.0 ? vsk : 1.0;
  const double dvs = vsk > 1.0 ? 1.0 : 0.0;
  double sp, dsp;
  softplus(p.kp * (1.0 / p.mu + vgk / vs), sp, dsp);
  const double e1 = vs / p.kp * sp;
  double i0 = 0.0, di0s = 0.0, di0g = 0.0;
  if (e1 > 0.0) {
    i0 = kPi / p.kg1 * std::pow(e1, p.ex);
    const double di0De1 = p.ex * i0 / e1;
    di0s = di0De1 * ((sp / p.kp + vs / p.kp * dsp * (-p.kp * vgk / (vs * vs))) * dvs);
    di0g = di0De1 * dsp;
  }
  double vps, dvps;
  softplus(kKneeBeta * vpk, vps, dvps);
  vps /= kKneeBeta;
  const double root = std::sqrt(vps);
  const double a = p.kv * vps * root;
  const double da = 1.5 * p.kv * root * dvps;
  const double d3 = i0 * i0 * i0 + a * a * a + kPentodeEps3;
  const double c = std::pow(d3, -1.0 / 3.0);
  const double m = i0 * a * c;
  const double dmDi0 = a * c * (1.0 - i0 * i0 * i0 / d3);
  const double dmDa = i0 * c * (1.0 - a * a * a / d3);
  const double lf = 1.0 + p.lam * vps;
  const double den = i0 + a + kPentodeEps1;
  const double share = p.rs + p.asc * i0 / den;
  const double disDi0 = p.rs + p.asc * i0 * (i0 + 2.0 * a + 2.0 * kPentodeEps1) / (den * den);
  const double disDa = -p.asc * i0 * i0 / (den * den);
  double ig, dgg;
  gridCurrent(p.gg, p.cg, p.xi, 0.0, vgk, ig, dgg);
  const double n = p.npar;
  out[0] = n * m * lf;
  out[1] = n * i0 * share;
  out[2] = n * ig;
  out[3] = n * (dmDa * da * lf + m * p.lam * dvps);
  out[4] = n * dmDi0 * di0s * lf;
  out[5] = n * dmDi0 * di0g * lf;
  out[6] = n * disDa * da;
  out[7] = n * disDi0 * di0s;
  out[8] = n * disDi0 * di0g;
  out[9] = 0.0;
  out[10] = 0.0;
  out[11] = n * dgg;
}

Library::Library() {
  softplusTable.build(-24.0, 24.0, softplus);
  minimumTable.build(0.0, 1.0, [](double r, double& y, double& dy) {
    const double q = 1.0 + r * r * r;
    y = std::pow(q, -1.0 / 3.0);
    dy = -r * r * y / q;
  });
}

const TriodeLaw* Library::addTriode(const std::array<double, 12>& params) {
  if (const TriodeLaw* law = find(triodes_, params)) return law;
  auto law = std::make_unique<TriodeLaw>();
  law->key = params;
  law->p = TriodeParams::from(params.data());
  buildPower(law->power, law->p.ex);
  buildGrid(law->grid, law->p.gg, law->p.cg, law->p.xi, law->p.vg0);
  triodes_.push_back(std::move(law));
  return triodes_.back().get();
}

const PentodeLaw* Library::addPentode(const std::array<double, 12>& params) {
  if (const PentodeLaw* law = find(pentodes_, params)) return law;
  auto law = std::make_unique<PentodeLaw>();
  law->key = params;
  law->p = PentodeParams::from(params.data());
  buildPower(law->power, law->p.ex);
  buildGrid(law->grid, law->p.gg, law->p.cg, law->p.xi, 0.0);
  const double kv = law->p.kv;
  law->knee.build([kv](double shifted, double& y, double& dy) {
    double s, ds;
    softplus(kKneeBeta * (shifted - kKneeShift), s, ds);
    s /= kKneeBeta;
    const double root = std::sqrt(s);
    y = kv * s * root;
    dy = 1.5 * kv * root * ds;
  });
  pentodes_.push_back(std::move(law));
  return pentodes_.back().get();
}

const DiodeLaw* Library::addDiode(const std::array<double, 12>& params) {
  if (const DiodeLaw* law = find(diodes_, params)) return law;
  auto law = std::make_unique<DiodeLaw>();
  law->key = params;
  law->p = DiodeParams::from(params.data());
  law->exponentialAtVcrit = std::exp(law->p.vcrit / law->p.nvt);
  law->reverseLimit = -kLnMax * law->p.nvt;
  diodes_.push_back(std::move(law));
  return diodes_.back().get();
}

}  // namespace magmora::devices
