// The partitioned amplifier: operating point, wiring, knob and relay rebuilds, value sets.
#include <catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

#include "circuit/AmpNetlist.h"
#include "graph/AmpGraph.h"
#include "solver/Newton.h"
#include "support/Golden.h"

using namespace magmora;

namespace model {
#include "circuit/ModelData.inc"
}

namespace {

std::vector<double> tone(double rate, double hz, double volts, int samples) {
  std::vector<double> x(static_cast<size_t>(samples));
  for (int n = 0; n < samples; ++n)
    x[static_cast<size_t>(n)] = volts * std::min(1.0, n / (0.002 * rate)) * std::sin(2.0 * 3.14159265358979 * hz * n / rate);
  return x;
}

std::vector<double> speakerOf(graph::AmpGraph& amp, const std::vector<double>& x) {
  const double* speaker = amp.output("P", "spk");
  std::vector<double> y;
  for (double v : x) {
    amp.processSample(v);
    y.push_back(*speaker);
  }
  return y;
}

double largestDifference(const std::vector<double>& a, const std::vector<double>& b) {
  double big = 0.0;
  for (size_t k = 0; k < a.size(); ++k) big = std::max(big, std::fabs(a[k] - b[k]));
  return big;
}

std::vector<double> readInput(const std::string& name) {
  const std::string path = std::string(MAGMORA_SOURCE_DIR) + "/reference/tran/" + name + ".in.f64";
  std::FILE* f = std::fopen(path.c_str(), "rb");
  REQUIRE(f != nullptr);
  std::vector<double> x;
  double v;
  while (std::fread(&v, sizeof v, 1, f) == 1) x.push_back(v);
  std::fclose(f);
  return x;
}

}  // namespace

TEST_CASE("graph: the generated netlist belongs to the generated model data") {
  CHECK(std::string(circuit::netdata::kValueSets[circuit::kDefaultValueSet].modelHash) == model::kModelHash);
}

TEST_CASE("graph: operating point and wiring equal the prototype's") {
  for (const std::string& path : test::goldenCases()) {
    INFO(path);
    const test::Json doc = test::Json::load(path);
    graph::GraphOptions options;
    options.sampleRate = doc["fs"].number;
    options.laws = graph::Laws::Closed;
    graph::AmpGraph amp;
    const circuit::Controls controls = test::controlsFrom(doc["setting"]);
    REQUIRE(amp.prepare(options, controls));
    const test::Json& clusters = doc["clusters"];
    size_t wired = 0;
    for (size_t k = 0; k < clusters.size(); ++k) {
      const test::Json& g = clusters[k];
      INFO("cluster " << g["name"].text);
      const graph::Cluster& c = amp.cluster(static_cast<int>(k));
      for (size_t o = 0; o < g["y0"].size(); ++o) {
        const double want = g["y0"][o].number;
        CHECK(std::fabs(*c.output(static_cast<int>(o)) - want) <= 1e-7 * std::max(1.0, std::fabs(want)));
      }
      const test::Json& from = g["wiring_src"];
      for (size_t j = 0; j < from.size(); ++j) {
        const int src = static_cast<int>(from[j][0].number);
        if (src < 0) continue;
        ++wired;
        bool found = false;
        for (const circuit::Wire& w : amp.amp().wires)
          found = found || (w.dst == static_cast<int>(k) && w.input == static_cast<int>(j) && w.src == src &&
                            w.output == static_cast<int>(from[j][1].number) && w.scale == g["wiring_scale"][j].number);
        CHECK(found);
      }
    }
    CHECK(wired == amp.amp().wires.size());
  }
}

TEST_CASE("graph: tables agree with the closed forms within 1e-4 V at the speaker on the golden signals") {
  // only the input signals are read here: they do not depend on the model version
  struct Signal {
    const char* name;
    bool crunch;
  };
  for (const Signal s : {Signal{"sine_lead_440Hz_0.001V", false}, {"sine_lead_440Hz_0.1V", false},
                         {"sine_lead_110Hz_0.5V", false}, {"sine_crunch_440Hz_0.1V", true}, {"burst_lead", false}}) {
    INFO(s.name);
    const std::vector<double> x = readInput(s.name);
    circuit::Controls controls;
    if (s.crunch) {
      REQUIRE(controls.set("channel", "rhythm"));
      REQUIRE(controls.set("crunch", "1"));
      REQUIRE(controls.set("pre_rhythm", "7"));
    }
    graph::GraphOptions options;
    options.solver.tolerance = 1e-6;  // the prototype's: only the laws differ
    graph::AmpGraph table, closed;
    options.laws = graph::Laws::Table;
    REQUIRE(table.prepare(options, controls));
    options.laws = graph::Laws::Closed;
    REQUIRE(closed.prepare(options, controls));
    CHECK(largestDifference(speakerOf(table, x), speakerOf(closed, x)) < 1e-4);
    CHECK(table.fallbacks() == 0);
  }
}

TEST_CASE("graph: the fixed and the dynamic Newton kernels find the same solution") {
  circuit::Controls controls;
  const auto amp = circuit::partitionAmp(circuit::Partition::Split, 0, 0, {});
  for (const char* name : {"A", "C", "T", "P"}) {
    INFO(name);
    devices::Library library;
    solver::ClusterSystem system;
    REQUIRE(system.prepare(amp.clusters[static_cast<size_t>(amp.find(name))], 192000.0, library));
    REQUIRE(system.build(controls));
    const auto& m = system.matrices;
    const auto& set = system.devices();
    const int t = static_cast<int>(set.triodes.size()), p = static_cast<int>(set.pentodes.size());

    const auto compare = [&](auto shape) {
      using Shape = decltype(shape);
      std::vector<double> i(48), j(48), excitation(48);
      auto fixed = std::make_unique<solver::Newton<Shape, devices::ClosedLaws>>();
      auto any = std::make_unique<solver::Newton<solver::DynamicShape, devices::ClosedLaws>>();
      fixed->bind(shape, set, m.K.data(), i.data(), 1e-4);
      any->bind(solver::DynamicShape(t, p, 0), set, m.K.data(), j.data(), 0.0);
      std::mt19937 rng(3);
      std::uniform_real_distribution<double> unit(0.0, 1.0);
      const solver::NewtonOptions options{1e-10, 200, true};
      int solved = 0;
      for (int trial = 0; trial < 300; ++trial) {
        for (int d = 0; d < t; ++d) {
          excitation[static_cast<size_t>(2 * d)] = 120.0 + 180.0 * unit(rng);
          excitation[static_cast<size_t>(2 * d + 1)] = -4.0 + 5.0 * unit(rng);
        }
        for (int d = 0; d < p; ++d) {
          const size_t q = static_cast<size_t>(2 * t + 3 * d);
          excitation[q] = 300.0 + 200.0 * unit(rng);
          excitation[q + 1] = 440.0;
          excitation[q + 2] = -80.0 + 70.0 * unit(rng);
        }
        std::copy(excitation.begin(), excitation.begin() + m.portRows, fixed->v);
        std::copy(excitation.begin(), excitation.begin() + m.portRows, any->v);
        const int a = fixed->solve(excitation.data(), options, false), b = any->solve(excitation.data(), options, false);
        if (a < 0 || b < 0) continue;
        ++solved;
        for (int k = 0; k < m.ports; ++k) CHECK(fixed->v[k] == Approx(any->v[k]).epsilon(1e-7).margin(1e-7));
      }
      CHECK(solved > 150);
    };
    if (p) compare(solver::FixedShape<2, 2, 0>());
    else if (t == 2) compare(solver::FixedShape<2, 0, 0>());
    else compare(solver::FixedShape<1, 0, 0>());
  }
}

TEST_CASE("graph: a moved knob equals an exact rebuild") {
  circuit::Controls at;
  graph::GraphOptions options;
  options.solver.tolerance = 1e-9;
  graph::AmpGraph amp;
  REQUIRE(amp.prepare(options, at));
  const std::vector<double> x = tone(192000.0, 440.0, 0.05, 9600);

  std::mt19937 rng(5);
  std::uniform_real_distribution<double> position(0.0, 10.0);
  for (int trial = 0; trial < 6; ++trial) {
    circuit::Controls moved = at;
    for (int k = 0; k < circuit::kNumKnobs; ++k) {
      const bool everyOne = trial >= 2;
      if (everyOne || k % 2 == trial) moved.knobs[static_cast<size_t>(k)] = trial == 4 ? 0.0 : (trial == 5 ? 10.0 : position(rng));
    }
    INFO("trial " << trial);
    amp.reset(at);
    amp.setKnobs(moved);  // low-rank
    const std::vector<double> updated = speakerOf(amp, x);
    amp.reset(moved);  // rebuilt
    const std::vector<double> rebuilt = speakerOf(amp, x);
    CHECK(largestDifference(updated, rebuilt) < 1e-4);

    amp.reset(at);
    amp.setKnobs(moved);
    amp.settle();
    CHECK(largestDifference(speakerOf(amp, x), rebuilt) < 1e-4);
  }
}

TEST_CASE("graph: settling in the middle of a note makes no step") {
  circuit::Controls at, moved;
  moved.knobs.fill(6.3);
  graph::AmpGraph a, b;
  REQUIRE(a.prepare({}, at));
  REQUIRE(b.prepare({}, at));
  const std::vector<double> x = tone(192000.0, 440.0, 0.01, 9600);
  a.setKnobs(moved);
  b.setKnobs(moved);
  speakerOf(a, x);
  speakerOf(b, x);
  b.settle();
  CHECK(largestDifference(speakerOf(a, x), speakerOf(b, x)) < 1e-3);
}

TEST_CASE("graph: every relay combination, from prepare and from switching, stays at rest in silence") {
  graph::GraphOptions options;
  options.dcCapLeak = 1e-18;  // the engine's
  for (unsigned load : {0u, circuit::kStructuralSwitches}) {
    circuit::Controls c;
    c.switches = load;
    graph::AmpGraph amp;
    REQUIRE(amp.prepare(options, c));
    const double* speaker = amp.output("P", "spk");
    const double rest = *speaker;
    for (unsigned bits = 0; bits < 32; ++bits) {
      if ((bits & circuit::kStructuralSwitches) != 0) continue;
      c.switches = bits | load;
      amp.setSwitches(c);
      double big = 0.0;
      for (int n = 0; n < 4000; ++n) {
        amp.processSample(0.0);
        big = std::max(big, std::fabs(*speaker - rest));
      }
      INFO("switches " << bits << " load " << load);
      CHECK(big < 1e-2);  // -80 dB re full scale
    }
    CHECK(amp.fallbacks() == 0);
    CHECK(amp.nonFinite() == 0);
  }
}

TEST_CASE("graph: a switched relay keeps the charge of the capacitors") {
  // switching there and back between two samples must be inaudible
  circuit::Controls lead, rhythm;
  REQUIRE(rhythm.set("channel", "rhythm"));
  graph::AmpGraph a, b;
  REQUIRE(a.prepare({}, lead));
  REQUIRE(b.prepare({}, lead));
  const std::vector<double> x = tone(192000.0, 440.0, 0.002, 9600);
  speakerOf(a, x);
  speakerOf(b, x);
  b.setSwitches(rhythm);
  b.setSwitches(lead);
  CHECK(largestDifference(speakerOf(a, x), speakerOf(b, x)) < 0.05);
}

TEST_CASE("graph: value sets differ only in what value_sets.json says, and all run") {
  REQUIRE(circuit::kNumValueSets >= 2);
  const int drawn = circuit::findValueSet("as-drawn");
  REQUIRE(drawn >= 0);
  const auto model = circuit::monolithic(0, circuit::kDefaultValueSet), asDrawn = circuit::monolithic(0, drawn);
  circuit::Controls c;
  REQUIRE(model.items.size() == asDrawn.items.size());
  std::vector<std::string> changed;
  for (size_t k = 0; k < model.items.size(); ++k) {
    CHECK(model.items[k].params == asDrawn.items[k].params);
    const auto *a = model.items[k].variant(c.switches), *b = asDrawn.items[k].variant(c.switches);
    if (a && b && circuit::valueOf(*a, c) != circuit::valueOf(*b, c)) changed.push_back(model.items[k].name);
  }
  std::sort(changed.begin(), changed.end());
  CHECK(changed == std::vector<std::string>{"C17", "C2", "R29"});

  for (int set = 0; set < circuit::kNumValueSets; ++set) {
    graph::GraphOptions options;
    options.valueSet = set;
    graph::AmpGraph amp;
    REQUIRE(amp.prepare(options, c));
    speakerOf(amp, tone(192000.0, 440.0, 0.05, 9600));
    CHECK(amp.fallbacks() == 0);
  }
}

TEST_CASE("graph: a non-finite input sends clusters back to the operating point") {
  graph::AmpGraph amp;
  REQUIRE(amp.prepare({}, circuit::Controls()));
  const double* speaker = amp.output("P", "spk");
  amp.processSample(std::numeric_limits<double>::quiet_NaN());
  CHECK(amp.nonFinite() > 0);
  for (int n = 0; n < 5000; ++n) {
    amp.processSample(0.0);
    REQUIRE(std::isfinite(*speaker));
  }
}

TEST_CASE("graph: one failed solve and its recovery never spend more than the recovery budget holds") {
  // The review's storm: lead, every knob at 10, bursts of full-scale noise and a 5 kHz square, at 48 kHz x 6.
  const double rate = 48000.0 * 6;
  circuit::Controls controls;
  controls.switches |= circuit::netdata::kSwitch_channel;
  for (auto& k : controls.knobs) k = 10.0;
  graph::GraphOptions o;
  o.sampleRate = rate;
  o.dcCapLeak = 1e-18;
  o.companionAtDefaults = true;
  o.recoveryPerSample = 20.0 / 6.0;
  o.recoveryCap = 400.0;
  graph::AmpGraph g;
  REQUIRE(g.prepare(o, controls));
  std::mt19937 rng(5);
  std::uniform_real_distribution<double> u(-1, 1);
  const long samples = static_cast<long>(3.0 * rate);  // before the in-loop check: 477 evaluations by 3 s
  for (long n = 0; n < samples; ++n) {
    const double t = static_cast<double>(n) / rate;
    g.processSample(std::fmod(t, 0.2) < 0.1 ? 32.0 * u(rng) : (std::fmod(t * 5000.0, 1.0) < 0.5 ? 32.0 : -32.0));
  }
  long worst = 0, retries = 0;
  for (int k = 0; k < g.size(); ++k) {
    worst = std::max(worst, g.cluster(k).stats().worstRecovery);
    retries += g.cluster(k).stats().retries;
  }
  INFO("recoveries " << retries << ", worst failed solve and recovery " << worst << " device evaluations");
  REQUIRE(retries > 0);
  // the budget is checked before every continuation attempt: at most one attempt's worth past what it held
  CHECK(worst <= 400 + 2 * 20);
}
