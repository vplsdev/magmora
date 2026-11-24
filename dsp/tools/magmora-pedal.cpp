// The pedal alone (XP2), at an internal rate, for the comparison with the prototype (gate G-P2) and for timing.
//   magmora-pedal --in x.f64 --fs F --out prefix [--set drive=D tone=T level=L] [--exact] [--no-cheap]
//       writes prefix.{p1,e1,o1,o2,out,e2}.f64 (float64, little-endian) and prints statistics
//   magmora-pedal --bench --fs F [--set ...] --signal typical|hot|noise3   cost per internal sample, as % of a core at
//       the host rate F / 6 (Standard at 48 kHz: F = 288000)
//   magmora-pedal --host x.f64 --fs HOST --factor K [--ovs 1|2] [--min-phase] --out y.f64 [--set ...]
//       the pedal stage alone as the product runs it (gate G-P3 in isolation): host-rate volts brought up by the
//       head's oversampler (factor K), the pedal stage at K x HOST (x2 inside with --ovs 2), and the head's
//       downsampler only; writes the host-rate output. --ovs 3: only O2 and O3 at 2x.
// --exact: the prototype's tolerance (1e-7 V) with the currents evaluated at the solution; otherwise the product's
// (1e-4 V, first-order currents). --no-cheap: the full form of every cluster, as pedal.SplitSim.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "EngineSupport.h"
#include "ToolSupport.h"
#include "cab/UserIr.h"
#include "pedal/Pedal.h"
#include "pedal/PedalStage.h"
#include "resample/Oversampler.h"

using namespace magmora;

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    tools::printVersion("magmora-pedal");
    return 0;
  }
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  tools::Args args(argc, argv);
  std::string in, out, signal = "typical";
  double fs = 288000.0, drive = 5, tone = 5, level = 5;
  bool exact = false, cheap = true, bench = false, budget = true;
  double tol = 0.0;
  bool protoLimit = false, minPhase = false;
  std::string host;
  int factor = 6, ovs = 2;
  bool timeReset = false;
  double budgetRate = 0.0;
  std::vector<std::pair<std::string, std::string>> settings;
  while (!args.empty()) {
    const std::string a = args.take("option");
    if (a == "--in") in = args.take("--in");
    else if (a == "--out") out = args.take("--out");
    else if (a == "--fs") fs = args.number("--fs");
    else if (a == "--set") args.settings(settings);
    else if (a == "--exact") exact = true;
    else if (a == "--no-cheap") cheap = false;
    else if (a == "--no-budget") budget = false;
    else if (a == "--tol") tol = args.number("--tol");
    else if (a == "--proto-limit") protoLimit = true;
    else if (a == "--budget") budgetRate = args.number("--budget");
    else if (a == "--bench") bench = true;
    else if (a == "--host") host = args.take("--host");
    else if (a == "--factor") factor = static_cast<int>(args.number("--factor"));
    else if (a == "--ovs") ovs = static_cast<int>(args.number("--ovs"));
    else if (a == "--min-phase") minPhase = true;
    else if (a == "--time-reset") timeReset = true;
    else if (a == "--signal") signal = args.take("--signal");
    else tools::fail("unknown option " + a);
  }
  for (const auto& [k, v] : settings) {
    if (k == "drive") drive = std::stod(v);
    else if (k == "tone") tone = std::stod(v);
    else if (k == "level") level = std::stod(v);
    else tools::fail("unknown setting " + k);
  }
  if (timeReset) {  // the cost of Pedal::reset (the operating point on turning the pedal on), and of a settled rebuild
    pedal::Pedal q;
    q.setKnobs(drive, tone, level);
    q.prepare(fs, fs / 6.0, ovs == 3 ? 2 : 1);
    double best = 1e30;
    for (int r = 0; r < 200; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      q.reset();
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("reset: %.1f us (best of 200)\n", best * 1e6);
    return 0;
  }
  if (!host.empty()) {  // the pedal stage alone, between the head's resamplers
    std::vector<double> xin;
    FILE* f = std::fopen(host.c_str(), "rb");
    if (!f) tools::fail("cannot read " + host);
    for (double v; std::fread(&v, sizeof v, 1, f) == 1;) xin.push_back(v);
    std::fclose(f);
    resample::Oversampler os;
    os.prepare(fs, factor, minPhase);
    pedal::PedalStage stage;
    if (!budget) stage.pedal(0).budgetPerSample = 1e18;
    if (budgetRate > 0.0) stage.pedal(0).budgetPerSample = budgetRate;
    if (tol > 0.0) stage.pedal(0).settings.tolerance = tol;
    stage.prepare(fs * factor, fs, 1, ovs);
    stage.setControls(true, drive, tone, level);
    std::vector<double> y(xin.size());
    double lane[resample::kMaxFactor];
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < xin.size(); ++i) {
      if (i % 64 == 0) stage.setControls(true, drive, tone, level);  // once per 64-sample block, as the chain does
      os.up(xin[i], lane);
      double* lanes[2] = {lane, nullptr};
      stage.process(lanes, factor);
      y[i] = os.down(lane);
    }
    const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    f = std::fopen(out.c_str(), "wb");
    if (!f) tools::fail("cannot write " + out);
    std::fwrite(y.data(), sizeof(double), y.size(), f);
    std::fclose(f);
    const pedal::PedalStats& s = stage.pedal(0).stats();
    const double ns = static_cast<double>(xin.size()) * factor;  // internal samples
    std::printf("%.2f %% of a core (with the head's resamplers); ", 100.0 * spent * fs / static_cast<double>(xin.size()));
    std::printf("latency %d, fallbacks %ld, recoveries %ld, charged %.3f, iterations per internal sample %.3f (O1 %.2f O2 %.2f O3 %.2f O4 %.2f), "
                "full O2 %.1f %%, full O3 %.1f %% of middle samples\n",
                os.latency(), s.fallbacks, s.recoveries, s.charged / ns, s.iterations / ns, s.clusterIterations[0] / ns,
                s.clusterIterations[1] / ns, s.clusterIterations[2] / ns, s.clusterIterations[3] / ns,
                100.0 * s.fullO2 / std::max(1L, s.midSamples), 100.0 * s.fullO3 / std::max(1L, s.midSamples));
    return 0;
  }
  pedal::Pedal p;
  p.cheap = cheap;
  if (!budget) p.budgetPerSample = 1e18;
  if (tol > 0.0) p.settings.tolerance = tol;
  if (protoLimit) p.settings.junctionLimiting = false;
  if (budgetRate > 0.0) p.budgetPerSample = budgetRate;
  if (exact) {  // as the prototype: no work budget
    p.settings = {1e-7, 60, false, false};
    p.budgetPerSample = 1e18;
  }
  p.setKnobs(drive, tone, level);
  p.prepare(fs, fs / 6.0);  // after the settings above: prepare sizes the budget's ceiling from them
  std::vector<double> x;
  if (bench) {
    const size_t n = static_cast<size_t>(fs * 5.0);
    x.resize(n);
    std::mt19937 rng(3);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<float> host(n / 6);  // full-band at the host rate, 3 V peak, brought to the internal rate as the head does
    for (float& v : host) v = static_cast<float>(std::clamp(g(rng), -1.0, 1.0) * 3.0);
    const std::vector<float> up = cab::resample(host, fs / 6.0, fs);
    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / fs;
      if (signal == "noise3") x[i] = i < up.size() ? up[i] : 0.0;
      else {
        const double a = signal == "hot" ? 1.0 : 0.25;  // a played guitar: decaying plucks
        x[i] = a * std::exp(-std::fmod(t, 0.25) * 6.0) * (std::sin(2 * 3.14159265358979 * 82.4 * t) + 0.5 * std::sin(2 * 3.14159265358979 * 247.0 * t));
      }
    }
    const int factor = 6;
    double best = 1e30, sink = 0.0;
    for (int round = 0; round < 3; ++round) {
      p.reset();
      const auto t0 = std::chrono::steady_clock::now();
      for (size_t i = 0; i < n; i += factor) {
        p.hostTick();
        for (int k = 0; k < factor && i + static_cast<size_t>(k) < n; ++k) sink += p.process(x[i + static_cast<size_t>(k)]);
      }
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    if (!std::isfinite(sink)) tools::fail("non-finite output");
    const pedal::PedalStats& s = p.stats();
    std::printf("%s at %.0f Hz inside: %.2f %% of a core (best of 3, 5 s); per sample: %.2f iterations, full O2 %.1f %%, "
                "full O3 %.1f %%, recoveries %ld, fallbacks %ld\n",
                signal.c_str(), fs, 100.0 * best / 5.0, static_cast<double>(s.iterations) / s.samples,
                100.0 * s.fullO2 / s.samples, 100.0 * s.fullO3 / s.samples, s.recoveries, s.fallbacks);
    return 0;
  }
  {
    FILE* f = std::fopen(in.c_str(), "rb");
    if (!f) tools::fail("cannot read " + in);
    double v;
    while (std::fread(&v, sizeof v, 1, f) == 1) x.push_back(v);
    std::fclose(f);
  }
  const char* names[6] = {"p1", "e1", "o1", "o2", "out", "e2"};
  std::vector<std::vector<double>> y(6, std::vector<double>(x.size()));
  for (size_t i = 0; i < x.size(); ++i) {
    p.process(x[i]);
    for (int k = 0; k < 6; ++k) y[static_cast<size_t>(k)][i] = p.probe(k);
  }
  for (int k = 0; k < 6; ++k) {
    FILE* f = std::fopen((out + "." + names[k] + ".f64").c_str(), "wb");
    if (!f) tools::fail("cannot write " + out);
    std::fwrite(y[static_cast<size_t>(k)].data(), sizeof(double), x.size(), f);
    std::fclose(f);
  }
  const pedal::PedalStats& s = p.stats();
  std::printf("iterations %.3f per sample, full O2 %ld, full O3 %ld, recoveries %ld, fallbacks %ld, model %s\n",
              static_cast<double>(s.iterations) / std::max(1L, s.samples), s.fullO2, s.fullO3, s.recoveries, s.fallbacks,
              pedal::modelHash());
  return 0;
}
