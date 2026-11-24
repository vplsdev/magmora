// The discretisation against the prototype's matrices (reference/golden, eval/export_golden.py), matched by name.
#include <catch.hpp>

#include <cmath>
#include <map>
#include <random>

#include "circuit/AmpNetlist.h"
#include "graph/AmpGraph.h"
#include "common/MatVec.h"
#include "solver/ClusterSystem.h"
#include "solver/DenseLU.h"
#include "support/Golden.h"

using namespace magmora;

namespace {

using Dense = std::vector<std::vector<double>>;

Dense dense(size_t rows, size_t cols) { return Dense(rows, std::vector<double>(cols, 0.0)); }

Dense fromJson(const test::Json& j, size_t cols) {
  Dense out = dense(j.size(), cols);
  for (size_t r = 0; r < j.size(); ++r)
    for (size_t c = 0; c < cols; ++c) out[r][c] = j[r][c].number;
  return out;
}

Dense fromColumns(const std::vector<double>& m, int stride, int firstRow, int rows, int firstCol, int cols) {
  Dense out = dense(static_cast<size_t>(rows), static_cast<size_t>(cols));
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c)
      out[static_cast<size_t>(r)][static_cast<size_t>(c)] = m[static_cast<size_t>((firstCol + c) * stride + firstRow + r)];
  return out;
}

Dense times(const Dense& a, const Dense& b, size_t bCols) {
  Dense out = dense(a.size(), bCols);
  for (size_t r = 0; r < a.size(); ++r)
    for (size_t k = 0; k < b.size(); ++k)
      for (size_t c = 0; c < bCols; ++c) out[r][c] += a[r][k] * b[k][c];
  return out;
}

// Frobenius norm of the difference, relative to the reference.
double relative(const Dense& got, const Dense& want) {
  double diff = 0.0, norm = 0.0;
  REQUIRE(got.size() == want.size());
  for (size_t r = 0; r < got.size(); ++r)
    for (size_t c = 0; c < got[r].size(); ++c) {
      diff += (got[r][c] - want[r][c]) * (got[r][c] - want[r][c]);
      norm += want[r][c] * want[r][c];
    }
  return norm > 0.0 ? std::sqrt(diff / norm) : std::sqrt(diff);
}

std::vector<std::string> names(const test::Json& list) {
  std::vector<std::string> out;
  for (const auto& item : list.items) out.push_back(item.text);
  return out;
}

size_t indexOf(const std::vector<std::string>& list, const std::string& name) {
  for (size_t k = 0; k < list.size(); ++k)
    if (list[k] == name) return k;
  FAIL("not found: " << name);
  return 0;
}

void checkCase(const std::string& path) {
  INFO(path);
  const test::Json doc = test::Json::load(path);
  const circuit::Controls controls = test::controlsFrom(doc["setting"]);
  // the partition as the graph builds it: with the companion resistors and remainder outputs when those are in force
  graph::GraphOptions options;
  options.sampleRate = doc["fs"].number;
  options.laws = graph::Laws::Closed;
  graph::AmpGraph built;
  REQUIRE(built.prepare(options, controls));
  const circuit::PartitionedAmp& amp = built.amp();
  const test::Json& clusters = doc["clusters"];
  REQUIRE(clusters.size() == amp.clusters.size());

  for (size_t k = 0; k < clusters.size(); ++k) {
    const test::Json& g = clusters[k];
    const circuit::Circuit& c = amp.clusters[k];
    INFO("cluster " << g["name"].text);
    REQUIRE(amp.names[k] == g["name"].text);

    devices::Library library;
    solver::ClusterSystem system;
    REQUIRE(system.prepare(c, doc["fs"].number, library, false));  // unfolded, as the prototype
    REQUIRE(system.build(controls));
    const solver::ClusterMatrices& m = system.matrices;

    // every element, by name, value and nodes
    std::map<std::string, const circuit::Item*> present;
    for (const circuit::Item& item : c.items)
      if (item.variant(controls.switches)) present[item.name] = &item;
    for (const test::Json& e : g["elements"].items) {
      INFO("element " << e["name"].text);
      REQUIRE(present.count(e["name"].text) == 1);
      const circuit::Variant& v = *present[e["name"].text]->variant(controls.switches);
      const test::Json& nodes = e["nodes"];
      REQUIRE(static_cast<size_t>(v.numNodes) == nodes.size());
      for (size_t n = 0; n < nodes.size(); ++n) {
        const int node = v.nodes[n];
        CHECK((node == circuit::kGround ? std::string("0") : c.nodes[static_cast<size_t>(node)]) == nodes[n].text);
      }
      const std::string kind = e["kind"].text;
      if (kind == "R" || kind == "C" || kind == "L")
        // a printed value exactly; the companion resistor is derived (a conductance solved for), so to 1e-9 as the matrices
        CHECK(circuit::valueOf(v, controls) ==
              Approx(e["value"].number).epsilon(e["name"].text == "Rcomp" ? 1e-9 : 1e-12).margin(0.0));
    }

    const auto sources = names(g["sources"]), states = names(g["states"]), devices = names(g["devices"]);
    const auto outputs = names(g["outputs"]), inputs = names(g["inputs"]);
    REQUIRE(static_cast<size_t>(m.outputs) == outputs.size());
    REQUIRE(static_cast<size_t>(m.inputs) == inputs.size());
    for (size_t j = 0; j < inputs.size(); ++j) CHECK(c.items[static_cast<size_t>(c.inputs[j])].name == inputs[j]);
    for (size_t j = 0; j < outputs.size(); ++j) CHECK(c.outputs[j].label == outputs[j]);

    const size_t ns = static_cast<size_t>(m.states), np = static_cast<size_t>(m.ports);
    const int nin = m.inputs;
    REQUIRE(g["Pv"].size() == np);

    // prototype port of each of our ports
    std::vector<size_t> portMap(np);
    for (size_t p = 0, sub = 0; p < np; ++p) {
      sub = p > 0 && system.portItems()[p] == system.portItems()[p - 1] ? sub + 1 : 0;
      const size_t device = indexOf(devices, c.items[static_cast<size_t>(system.portItems()[p])].name);
      portMap[p] = static_cast<size_t>(g["device_port"][device].number) + sub;
    }
    // s = T w: a capacitor outside the forest is the signed sum around its loop
    Dense T = dense(states.size(), ns);
    int presentStates = 0;
    for (size_t r = 0; r < system.reactiveItems().size(); ++r) {
      if (!system.present(static_cast<int>(r))) continue;
      ++presentStates;
      const size_t row = indexOf(states, c.items[static_cast<size_t>(system.reactiveItems()[r])].name);
      for (size_t t = 0; t < ns; ++t)
        T[row][t] = system.companion(static_cast<int>(r)) * system.expansion(static_cast<int>(r), static_cast<int>(t));
    }
    CHECK(static_cast<size_t>(presentStates) == states.size());

    const auto reorderRows = [&](const Dense& a) {
      Dense out;
      for (size_t p = 0; p < np; ++p) out.push_back(a[portMap[p]]);
      return out;
    };
    const auto reorderCols = [&](const Dense& a) {
      Dense out = dense(a.size(), np);
      for (size_t r = 0; r < a.size(); ++r)
        for (size_t p = 0; p < np; ++p) out[r][p] = a[r][portMap[p]];
      return out;
    };
    // the prototype's source columns: the inputs, then all the constant ones folded into one
    const auto sourceColumns = [&](const Dense& a) {
      Dense out = dense(a.size(), inputs.size() + 1);
      for (size_t r = 0; r < a.size(); ++r)
        for (size_t s = 0; s < sources.size(); ++s) {
          bool input = false;
          for (size_t j = 0; j < inputs.size(); ++j)
            if (inputs[j] == sources[s]) {
              out[r][j] = a[r][s];
              input = true;
            }
          if (!input) out[r][inputs.size()] += a[r][s] * g["source_dc"][s].number;
        }
      return out;
    };
    const auto withConstant = [&](Dense linear, const std::vector<double>& constant, int firstRow) {
      for (size_t r = 0; r < linear.size(); ++r) linear[r].push_back(constant[static_cast<size_t>(firstRow) + r]);
      return linear;
    };
    const auto gold = [&](const char* key, size_t cols) { return fromJson(g[key], cols); };
    const size_t nu = sources.size(), nsFull = states.size();
    const int P = m.portRows, O = m.outRows, NS = m.states, NP = m.ports;

    // 1e-9 relative
    if (np) {
      CHECK(relative(fromColumns(m.K, P, 0, NP, 0, NP), reorderCols(reorderRows(gold("K", np)))) < 1e-9);
      CHECK(relative(fromColumns(m.Pw, P, 0, NP, 0, NS), times(reorderRows(gold("Ps", nsFull)), T, ns)) < 1e-9);
      CHECK(relative(withConstant(fromColumns(m.Pu, P, 0, NP, 0, nin), m.Pc, 0),
                     sourceColumns(reorderRows(gold("Pv", nu)))) < 1e-9);
      CHECK(relative(times(T, fromColumns(m.WL, O, 0, NS, nin, NP), np), reorderCols(gold("Si", np))) < 1e-9);
      CHECK(relative(fromColumns(m.WL, O, NS, m.outputs, nin, NP), reorderCols(gold("Yi", np))) < 1e-9);
    }
    CHECK(relative(times(T, fromColumns(m.WS, O, 0, NS, 0, NS), ns), times(gold("Ss", nsFull), T, ns)) < 1e-9);
    CHECK(relative(times(T, withConstant(fromColumns(m.WL, O, 0, NS, 0, nin), m.Wc, 0), inputs.size() + 1),
                   sourceColumns(gold("Sv", nu))) < 1e-9);
    CHECK(relative(fromColumns(m.WS, O, NS, m.outputs, 0, NS), times(gold("Ys", nsFull), T, ns)) < 1e-9);
    CHECK(relative(withConstant(fromColumns(m.WL, O, NS, m.outputs, 0, nin), m.Wc, NS), sourceColumns(gold("Yv", nu))) <
          1e-9);
  }
}

}  // namespace

TEST_CASE("solver: LU solves a random dense system") {
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> value(-1.0, 1.0);
  const int n = 23, columns = 5;
  std::vector<double> a(n * n), x(n * columns), b(n * columns);
  for (double& v : a) v = value(rng);
  for (double& v : x) v = value(rng);
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < columns; ++c)
      for (int k = 0; k < n; ++k) b[r * columns + c] += a[r * n + k] * x[k * columns + c];
  int pivot[n];
  REQUIRE(solver::luFactor(a.data(), pivot, n));
  solver::luSolve(a.data(), pivot, n, b.data(), columns, columns);
  for (size_t k = 0; k < x.size(); ++k) CHECK(b[k] == Approx(x[k]).margin(1e-11));

  std::vector<double> singular(9, 0.0);
  CHECK_FALSE(solver::luFactor(singular.data(), pivot, 3));
}

TEST_CASE("solver: the fixed-size products equal a plain loop for every size") {
  for (int rows = 0; rows <= 48; rows += 2)
    for (int cols : {0, 1, 2, 7, 16}) {
      std::vector<double> m(static_cast<size_t>(rows * cols) + 1), x(static_cast<size_t>(cols) + 1);
      std::vector<double> c(static_cast<size_t>(rows) + 1), out(static_cast<size_t>(rows) + 1);
      for (size_t k = 0; k < m.size(); ++k) m[k] = std::sin(static_cast<double>(k));
      for (size_t k = 0; k < x.size(); ++k) x[k] = std::cos(static_cast<double>(k));
      for (size_t k = 0; k < c.size(); ++k) c[k] = 0.1 * static_cast<double>(k);
      selectMulAdd(rows)(m.data(), rows, cols, x.data(), c.data(), out.data());
      for (int r = 0; r < rows; ++r) {
        double want = c[static_cast<size_t>(r)];
        for (int k = 0; k < cols; ++k) want += m[static_cast<size_t>(k * rows + r)] * x[static_cast<size_t>(k)];
        CHECK(out[static_cast<size_t>(r)] == Approx(want).margin(1e-13));
      }
    }
}

TEST_CASE("solver: cluster matrices equal the prototype's to 1e-9") {
  for (const std::string& path : test::goldenCases()) checkCase(path);
}
