// The EQ alone (XP3): its impulse response at a host rate and settings, for eval/check_eq.py (gate G-E1).
//   magmora-eq --rate R [--quality standard|high|ultra] --seconds S --out ir.f64 [--set eq_key=value ...]
// The EQ runs where the chain runs it, at the head's internal rate for that host rate and quality. Writes
// float64 samples (little-endian) at that rate, the response to a unit impulse after the EQ has faded in, and
// prints the internal rate.
#include <algorithm>
#include <magmora/Chain.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "EngineSupport.h"
#include "ToolSupport.h"
#include "engine/Quality.h"
#include "eq/Eq.h"

using namespace magmora;

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    tools::printVersion("magmora-eq");
    return 0;
  }
  tools::Args args(argc, argv);
  double rate = 48000.0;
  double seconds = 1.0;
  size_t quality = 0;
  std::string out;
  bool bench = false;  // --bench: the cost of one core of the EQ, steady and with one control moving
  double slopes = 2.0;  // --slopes 0|1|2: of the high-pass and low-pass in the bench
  std::vector<std::pair<std::string, std::string>> settings;
  while (!args.empty()) {
    const std::string a = args.take("option");
    if (a == "--rate") rate = args.number("--rate");
    else if (a == "--seconds") seconds = args.number("--seconds");
    else if (a == "--quality") {
      const std::string q = args.take("--quality");
      quality = q == "high" ? 1 : (q == "ultra" ? 2 : (q == "standard" ? 0 : 9));
      if (quality > 2) tools::fail("unknown quality " + q);
    }
    else if (a == "--out") out = args.take("--out");
    else if (a == "--set") args.settings(settings);
    else if (a == "--bench") bench = true;
    else if (a == "--slopes") slopes = args.number("--slopes");
    else tools::fail("unknown option " + a);
  }
  if (bench) {
    const double host = rate;
    const double internal = rate * engine::oversamplingFactor(quality, rate);
    double plain[eq::kControls];
    for (int k = 0; k < eq::kControls; ++k) plain[k] = kChainParams[kEqOn - kNumParams + k].def;
    plain[0] = 1.0;
    plain[kEqHighPassSlope - kEqOn] = plain[kEqLowPassSlope - kEqOn] = slopes;  // 2: all eight sections
    for (int moving = 0; moving < 2; ++moving) {
      eq::Eq e;
      e.prepare(internal);
      const int factor = static_cast<int>(internal / host + 0.5), block = 64 * factor;
      std::vector<double> x(static_cast<size_t>(block));
      const long blocks = static_cast<long>(10.0 * host / 64);
      double best = 1e30;
      for (int round = 0; round < 3; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        for (long b = 0; b < blocks; ++b) {
          if (moving) plain[kEqBand2Freq - kEqOn] = 400.0 + 300.0 * std::sin(0.01 * b);
          e.setControls(plain, true);
          for (int i = 0; i < block; ++i) x[static_cast<size_t>(i)] = 1e-3 * ((b * block + i) % 97 - 48);
          e.process(x.data(), nullptr, block);
        }
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
      }
      std::printf("%s at %.0f Hz host, %.0f Hz inside: %.2f %% of a core (best of 3, 10 s)\n", moving ? "moving" : "steady",
                  host, internal, 100.0 * best / 10.0);
    }
    return 0;
  }
  if (out.empty() || !(seconds > 0.0)) tools::fail("usage: magmora-eq --rate R [--quality Q] --seconds S --out ir.f64 [--set key=value ...]");
  rate *= engine::oversamplingFactor(quality, rate);
  const long samples = static_cast<long>(seconds * rate);
  double plain[eq::kControls];
  for (int k = 0; k < eq::kControls; ++k) plain[k] = kChainParams[kEqOn - kNumParams + k].def;
  plain[0] = 1.0;
  for (const auto& [key, value] : settings) {
    const int id = Chain::findParameter(key.c_str());
    if (id < kEqOn || id >= kEqOn + eq::kControls) tools::fail("not an EQ parameter: " + key);
    plain[id - kEqOn] = std::stod(value);
  }
  eq::Eq e;
  e.prepare(rate);
  std::vector<double> x(static_cast<size_t>(rate * 0.05), 0.0);  // silence while it fades in: states stay empty
  e.setControls(plain, true);
  e.process(x.data(), nullptr, static_cast<int>(x.size()));
  std::vector<double> y(static_cast<size_t>(samples), 0.0);
  y[0] = 1.0;
  for (size_t at = 0; at < y.size(); at += 64) {
    e.setControls(plain, true);
    e.process(y.data() + at, nullptr, static_cast<int>(std::min<size_t>(64, y.size() - at)));
  }
  FILE* f = std::fopen(out.c_str(), "wb");
  if (!f || std::fwrite(y.data(), sizeof(double), y.size(), f) != y.size()) tools::fail("cannot write " + out);
  std::fclose(f);
  std::printf("%.1f\n", rate);
  return 0;
}
