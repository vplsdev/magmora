// Device laws: the closed forms against the prototype, the tables against the closed forms.
#include <catch.hpp>

#include <algorithm>
#include <cmath>
#include <random>

#include "devices/Evaluators.h"
#include "support/Golden.h"

using namespace magmora;

namespace {

std::array<double, 12> paramsOf(const test::Json& j) {
  std::array<double, 12> p{};
  for (size_t k = 0; k < j.size(); ++k) p[k] = j[k].number;
  return p;
}

const circuit::ElementDef& firstOf(circuit::Kind kind) {
  const circuit::ValueSetDef& set = circuit::netdata::kValueSets[circuit::kDefaultValueSet];
  for (int k = 0; k < set.numElements; ++k)
    if (set.elements[k].kind == kind) return set.elements[k];
  std::abort();
}

std::array<double, 12> netlistParams(circuit::Kind kind) {
  std::array<double, 12> p{};
  const auto& e = firstOf(kind);
  std::copy(std::begin(e.params), std::end(e.params), p.begin());
  return p;
}

bool close(double got, double want, double relative, double absolute) {
  return std::fabs(got - want) <= std::max(absolute, relative * std::fabs(want));
}

}  // namespace

TEST_CASE("devices: closed forms equal the prototype to 1e-12") {
  const test::Json g = test::Json::load(test::goldenDir() + "/devices.json");
  const auto tp = devices::TriodeParams::from(paramsOf(g["triode_params"]).data());
  for (const test::Json& row : g["triode"].items) {
    double out[6];
    devices::triode(tp, row[0].number, row[1].number, out);
    INFO("triode vpk " << row[0].number << " vgk " << row[1].number);
    for (size_t k = 0; k < 6; ++k) CHECK(close(out[k], row[2 + k].number, 1e-12, 1e-30));
  }
  const auto pp = devices::PentodeParams::from(paramsOf(g["pentode_params"]).data());
  for (const test::Json& row : g["pentode"].items) {
    double out[12];
    devices::pentode(pp, row[0].number, row[1].number, row[2].number, out);
    INFO("pentode vpk " << row[0].number << " vsk " << row[1].number << " vgk " << row[2].number);
    // the plate-voltage derivatives are differences of nearly equal terms: judge them against their largest term
    for (size_t k = 0; k < 12; ++k) CHECK(close(out[k], row[3 + k].number, 1e-12, 1e-16));
  }
}

TEST_CASE("devices: golden parameters are the netlist's") {
  const test::Json g = test::Json::load(test::goldenDir() + "/devices.json");
  CHECK(paramsOf(g["triode_params"]) == netlistParams(circuit::Kind::Triode));
  CHECK(paramsOf(g["pentode_params"]) == netlistParams(circuit::Kind::Pentode));
}

TEST_CASE("devices: Jacobians agree with finite differences") {
  const auto t = devices::TriodeParams::from(netlistParams(circuit::Kind::Triode).data());
  const auto p = devices::PentodeParams::from(netlistParams(circuit::Kind::Pentode).data());
  const double h = 1e-5;
  for (double vp : {20.0, 150.0, 300.0})
    for (double vg : {-2.0, -1.0, 0.2}) {
      double out[6], plus[6], minus[6];
      devices::triode(t, vp, vg, out);
      devices::triode(t, vp + h, vg, plus);
      devices::triode(t, vp - h, vg, minus);
      CHECK(close(out[2], (plus[0] - minus[0]) / (2 * h), 1e-4, 1e-9));
      devices::triode(t, vp, vg + h, plus);
      devices::triode(t, vp, vg - h, minus);
      CHECK(close(out[3], (plus[0] - minus[0]) / (2 * h), 1e-4, 1e-9));
      CHECK(close(out[5], (plus[1] - minus[1]) / (2 * h), 1e-4, 1e-9));
    }
  for (double vp : {2.0, 30.0, 400.0})
    for (double vg : {-50.0, -20.0, 1.0}) {
      const double at[3] = {vp, 420.0, vg};
      double out[12], plus[12], minus[12];
      devices::pentode(p, at[0], at[1], at[2], out);
      for (int c = 0; c < 3; ++c) {
        double a[3] = {at[0], at[1], at[2]}, b[3] = {at[0], at[1], at[2]};
        a[c] += h;
        b[c] -= h;
        devices::pentode(p, a[0], a[1], a[2], plus);
        devices::pentode(p, b[0], b[1], b[2], minus);
        for (int r = 0; r < 3; ++r) CHECK(close(out[3 + 3 * r + c], (plus[r] - minus[r]) / (2 * h), 2e-4, 1e-8));
      }
    }
}

TEST_CASE("devices: tables follow the closed forms over the whole working range") {
  devices::Library library;
  devices::DeviceSet set;
  set.library = &library;
  set.triodes.assign(2, library.addTriode(netlistParams(circuit::Kind::Triode)));
  set.pentodes.assign(2, library.addPentode(netlistParams(circuit::Kind::Pentode)));
  const devices::DiodeLaw* diode = library.addDiode(netlistParams(circuit::Kind::Diode));
  set.flyback.assign(2, diode);

  using Shape = solver::FixedShape<2, 2, 0>;
  devices::TableLaws<Shape> tables;
  devices::ClosedLaws<Shape> closed;
  tables.bind(set);
  closed.bind(set);

  std::mt19937 rng(11);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const auto between = [&](double lo, double hi) { return lo + (hi - lo) * unit(rng); };
  for (int trial = 0; trial < 200000; ++trial) {
    double v[10], iT[10], iC[10], jT[20], jC[20];
    for (int d = 0; d < 2; ++d) {
      v[2 * d] = trial % 7 ? between(-20.0, 500.0) : between(-1.0, 3.0);
      v[2 * d + 1] = trial % 5 ? between(-6.0, 2.0) : between(-200.0, 60.0);
      v[4 + 3 * d] = trial % 7 ? between(-20.0, 1500.0) : between(-9.0, 4.0);
      v[4 + 3 * d + 1] = trial % 11 ? between(250.0, 500.0) : between(-5.0, 20.0);
      v[4 + 3 * d + 2] = trial % 5 ? between(-80.0, 4.0) : between(-200.0, 30.0);
    }
    tables.eval(Shape(), v, iT, jT);
    closed.eval(Shape(), v, iC, jC);
    for (int k = 0; k < 10; ++k) {
      // a millionth of the current, or a nanoamp
      if (!close(iT[k], iC[k], 2e-6, 1e-9)) {
        CAPTURE(trial, k, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        REQUIRE(iT[k] == Approx(iC[k]).epsilon(2e-6));
      }
    }
    for (int k = 0; k < 20; ++k) {
      if (!close(jT[k], jC[k], 3e-4, 2e-8)) {
        CAPTURE(trial, k, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        REQUIRE(jT[k] == Approx(jC[k]).epsilon(3e-4));
      }
    }
  }
}
