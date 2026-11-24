// magmora-bench: gate G3 (docs/VERIFICATION.md), CPU per quality.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>

#include <magmora/Chain.h>

#include "BenchSignals.h"
#include "EngineSupport.h"
#include "WavFile.h"
#include "engine/Quality.h"
#include "resample/Oversampler.h"

#if defined(__APPLE__)
#include <pthread/qos.h>
#include <time.h>
#endif
#if !defined(_WIN32)
#include <stdlib.h>
#endif

namespace {

using namespace magmora;

const char* kUsage =
    "magmora-bench [--rate R] [--in di.wav --set key=value ...] [--seconds S] [--rounds N] [--quality standard,high,ultra]\n"
    "             [--cases typical,hot,torture,sweep,stress,stress_noise,tone10k,power_moves,power_moves_noise,soakN] [--values NAME] [--in-volts V] [--check] [--chain] [--chain-set key=value ...]\n"
    "  48 kHz host, 64-sample blocks, one thread. typical: the input (default: the committed guitar-like signal) at\n"
    "  the given settings; hot: the same with pre 8, post 7; torture: everything at 10, full-scale noise; sweep: the\n"
    "  typical case while every knob moves; stress: random automation of every parameter, relays and power included,\n"
    "  with the slowest blocks and what happened in them. The share of a core is CPU time; the worst block is per block\n"
    "  the fastest\n"
    "  of the rounds, which takes out the operating system's interruptions. --check: exit 1 if gate G3 fails.\n";

double kRate = 48000.0;  // host rate: --rate
constexpr int kBlock = 64;

// CPU time of this thread, so that a busy machine does not count as a slow engine.
double threadSeconds() {
#if defined(__APPLE__)
  return static_cast<double>(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) * 1e-9;
#else
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

struct Case {
  std::string name;
  tools::Settings settings;
  const std::vector<float>* signal;
  bool sweep;
  bool stress = false;
  bool powerMoves = false;
  int soakSeed = 0;  // > 0: the plug-in soak's automation (plugin/tests/host.mm Soak) with this seed
};

// Random automation that hammers every automatable parameter, relays and power included, as a soak test does: at
// every block one to four parameters jump to random values. Deterministic.
void stressBlock(Engine& engine, size_t block, std::mt19937& rng) {
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  // bursts of 0.4 s of automation, then 1.6 s of rest: every moved knob settles at about the same time, a relay
  // switches while nothing moves, and all knobs jump at once and settle together
  const size_t at = block % 1500;
  if (at >= 300) {
    if (at == 1000) engine.setParam(kChannel, unit(rng) < 0.5 ? 0.0 : 1.0);
    if (at == 1100) {
      for (int id = kPreRhythm; id <= kPresence; ++id) engine.setParam(static_cast<ParamId>(id), unit(rng) * 10.0);
    }
    return;
  }
  const int changes = 1 + static_cast<int>(unit(rng) * 4.0);
  for (int k = 0; k < changes; ++k) {
    const ParamInfo& p = kParams[static_cast<int>(unit(rng) * kStandby)];  // everything up to Standby, power below
    engine.setParam(p.id, p.min + unit(rng) * (p.max - p.min));
  }
  if (at == 200) engine.setParam(kPower, 0.0);  // a power cycle every two seconds
  if (at == 210) engine.setParam(kPower, 1.0);
  if (block % 50 == 0) engine.setParam(kStandby, unit(rng) < 0.5 ? 0.0 : 1.0);
}

// The plugin soak's overruns came right after presence moves: extreme settings, then presence (and with it the other
// power-amp knobs) jumping between random values at random intervals. Deterministic.
// The plug-in test host's soak, replayed on the engine alone: every 8 blocks a random parameter gets a random
// normalised value (power and standby mostly on), on a looping pluck. Same generator and draws as host.mm.
int gSoakLast = -1;
void soakBlock(Engine& engine, size_t block, std::mt19937& random) {
  if (block % 8 != 0) return;
  std::uniform_real_distribution<double> unit(0., 1.);
  const int id = static_cast<int>(random() % kNumParams);
  const ParamInfo& info = kParams[id];
  if (!info.automatable) return;
  gSoakLast = id;
  double v;
  if (id == kPower || id == kStandby) v = (random() % 20 != 0) ? 1.0 : 0.0;
  else {
    v = info.min + unit(random) * (info.max - info.min);
    if (info.taper == Taper::Stepped) v = std::round(v);
  }
  engine.setParam(static_cast<ParamId>(id), v);
}

size_t gLastMove = 0;
void powerMoveBlock(Engine& engine, size_t block, std::mt19937& rng) {
  static size_t next = 0;
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  if (block == 0) next = 30;
  if (block != next) return;
  gLastMove = block;
  next = block + 10 + static_cast<size_t>(unit(rng) * 790.0);  // 13 ms to 1.07 s apart: some moves settle (about 0.5 s), some do not
  auto extreme = [&] { const double u = unit(rng); return u < 0.35 ? 0.0 : (u < 0.7 ? 10.0 : 10.0 * unit(rng)); };
  engine.setParam(kPresence, extreme());
  if (unit(rng) < 0.4) engine.setParam(kResonance, extreme());
  if (unit(rng) < 0.25) engine.setParam(kPostLead, extreme());
  if (unit(rng) < 0.2) engine.setParam(static_cast<ParamId>(kLow + static_cast<int>(unit(rng) * 3.0)), extreme());
}

struct BlockEvents {
  long rebuilds, relays, settles, restarts, recoveries;
};

BlockEvents snapshotCounters() {
  const engine::WorkCounters& c = engine::workCounters();
  return {c.clusterRebuilds.load(), c.relaySwitches.load(), c.settles.load(), c.restarts.load(), c.recoveries.load()};
}

struct Result {
  double share = 0.0, worst = 0.0, nsPerInternal = 0.0;
  size_t worstBlock = 0;     // index of the worst block (fastest of the rounds)
  double secondHalfWorst = 0.0;  // the same over the second half of the signal (past any start-up)
  double firstBlocks[4] = {};    // blocks 0..3 (fastest of the rounds)
  unsigned fallbacks = 0;
  std::vector<std::pair<double, BlockEvents>> slowest;  // stress: the slowest blocks and what happened in them
  // stress: for each kind of event, how many blocks had one and the slowest of those blocks
  long eventBlocks[5] = {0, 0, 0, 0, 0};
  double eventWorst[5] = {0, 0, 0, 0, 0};
  double quietWorst = 0.0;
  // the slowest single block of each round as it happened (no best-of), in thread time and wall time, and its events
  std::vector<std::pair<double, BlockEvents>> rawWorst;
  std::vector<std::pair<double, size_t>> wallWorst;
  long overWall = 0, overThread = 0;  // blocks over 100 % of real time, all rounds
  long blocks = 0;
  double sinceMove[5] = {0, 0, 0, 0, 0};  // power_moves: slowest block as measured, by blocks since the last move
};

int gValues = Engine::defaultValueSet();
bool gChain = false;
tools::Settings gChainSettings;  // --chain-set key=value ...: chain parameters (implies --chain)
std::string gChainCab, gChainCabMapping;  // --chain-cab folder [--chain-cab-mapping file]: the cabinet's pack
std::string gChainIr1;           // --chain-ir1 file.wav: a response in cabinet slot 1
std::string gChainIr;            // --chain-ir file.wav: a response loaded into the user slot (implies --chain)  // --chain: the whole chain (magmora::Chain, every new block off) instead of the engine alone

Result run(const Case& c, const std::string& quality, const Calibration& calibration, int rounds) {
  const size_t blocks = c.signal->size() / kBlock;
  std::vector<double> fastest(blocks, 1e30);
  std::vector<BlockEvents> events(blocks);
  int factorUsed = 1;
  double bestTotal = 1e30;
  Result result;
  for (int round = 0; round < rounds; ++round) {
    Chain chain;
    Engine& engine = chain.engine();
    for (const auto& [key, value] : c.settings) tools::setParameter(engine, key, value);
    for (const auto& [key, value] : gChainSettings) {
      const int id = Chain::findParameter(key.c_str());
      if (id < kNumParams) tools::fail("not a chain parameter: " + key);
      chain.setParameter(id, std::stod(value));
    }
    tools::setParameter(engine, "quality", quality);
    engine.setCalibration(calibration);
    engine.setValueSet(gValues);
    if (!(gChain ? chain.prepare(kRate, kBlock, 1) : engine.prepare(kRate, kBlock, 1))) tools::fail("prepare failed");
    if (!gChainIr.empty()) {
      std::string error;
      if (!chain.loadUserIr(gChainIr.c_str(), &error)) tools::fail(error);
      chain.idle();
    }
    if (!gChainIr1.empty()) {
      std::string error;
      if (!chain.loadCabinetIr(0, gChainIr1.c_str(), &error)) tools::fail(error);
    }
    if (!gChainCab.empty()) {  // the cabinet's pack; its first response is built before the timing starts
      std::string error;
      if (!chain.loadCabinetPack(gChainCab.c_str(), &error, gChainCabMapping.empty() ? nullptr : gChainCabMapping.c_str()))
        tools::fail(error);
      for (int k = 0; k < 3000 && chain.cabinetStatus().builds == 0; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (round == 0) {
        const CabinetStatus st = chain.cabinetStatus();
        std::printf("    cabinet: %d files, first response built in %.1f ms\n", st.files, st.lastBuildMs);
      }
    }
    if (!gChainIr.empty() || !gChainIr1.empty() || !gChainCab.empty()) chain.waitForCabinet(5.0);
    const char* sweep = std::getenv("MAGMORA_BENCH_SWEEP");  // key: that chain parameter swept 0.5 Hz across its range
    const int sweepId = sweep ? Chain::findParameter(sweep) : -1;
    factorUsed = engine.oversamplingFactor();
    float buffer[kBlock];
    float* io[1] = {buffer};
    float right[kBlock];
    float* stereo[2] = {buffer, right};
    double total = 0.0, roundWorst = 0.0, roundWall = 0.0;
    size_t worstAt = 0, wallAt = 0;
    std::mt19937 rng(17);
    std::mt19937 soakRandom(static_cast<unsigned>(c.soakSeed));
    for (size_t b = 0; b < blocks; ++b) {
      if (c.stress && !c.powerMoves) stressBlock(engine, b, rng);
      if (c.powerMoves) powerMoveBlock(engine, b, rng);
      if (c.soakSeed > 0) soakBlock(engine, b, soakRandom);
      if (sweepId >= 0) {
        const ParamInfo* p = Chain::parameterInfo(sweepId);
        const double t = static_cast<double>(b * kBlock) / kRate;
        chain.setParameter(sweepId, p->min + (p->max - p->min) * (0.5 + 0.5 * std::sin(2.0 * tools::kBenchPi * 0.5 * t)));
      }
      if (const char* at = std::getenv("MAGMORA_BENCH_SET_AT")) {  // key=value@block: one chain parameter changed there
        const std::string spec = at;
        const size_t eq = spec.find('='), amp = spec.find('@');
        if (eq != std::string::npos && amp != std::string::npos && b == static_cast<size_t>(std::stol(spec.substr(amp + 1))))
          chain.setParameter(Chain::findParameter(spec.substr(0, eq).c_str()), std::stod(spec.substr(eq + 1, amp - eq - 1)));
      }
      const BlockEvents before = snapshotCounters();
      std::copy_n(c.signal->data() + b * kBlock, kBlock, buffer);
      if (c.sweep) {
        const double t = static_cast<double>(b * kBlock) / kRate;
        for (int id = kPreRhythm; id <= kPresence; ++id)
          engine.setParam(static_cast<ParamId>(id), 5.0 + 4.0 * std::sin(2.0 * tools::kBenchPi * (2.0 + 0.3 * id) * t));
      }
      const double t0 = threadSeconds();
      const auto w0 = std::chrono::steady_clock::now();
      if (gChain)
        chain.process(io, stereo, kBlock);
      else
        engine.process(io, io, kBlock);
      const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
      const double spent = threadSeconds() - t0;
      if (spent > roundWorst) {
        roundWorst = spent;
        worstAt = b;
      }
      if (wall > kBlock / kRate) ++result.overWall;
      if (spent > kBlock / kRate) ++result.overThread;

      if (c.powerMoves && b >= 30) {
        const size_t d = b - gLastMove;
        const int bucket = d == 0 ? 0 : (d < 4 ? 1 : (d < 40 ? 2 : (d < 400 ? 3 : 4)));
        result.sinceMove[bucket] = std::max(result.sinceMove[bucket], 100.0 * spent / (kBlock / kRate));
      }
      if (wall > roundWall) {
        roundWall = wall;
        wallAt = b;
      }
      total += spent;
      fastest[b] = std::min(fastest[b], spent);
      const BlockEvents after = snapshotCounters();
      events[b] = {after.rebuilds - before.rebuilds, after.relays - before.relays, after.settles - before.settles,
                   after.restarts - before.restarts, after.recoveries - before.recoveries};
      if (c.soakSeed > 0 && (spent > 0.9 * kBlock / kRate || (std::getenv("SOAK_FROM") && b >= static_cast<size_t>(std::atol(std::getenv("SOAK_FROM"))) && b < static_cast<size_t>(std::atol(std::getenv("SOAK_FROM"))) + 14)))
        std::printf("    over 90 %%: block %zu, %.0f %%, last moved %s; rebuilds %ld relays %ld settles %ld restarts %ld "
                    "recoveries %ld fallbacks %u\n", b, 100.0 * spent / (kBlock / kRate),
                    gSoakLast >= 0 ? kParams[gSoakLast].key : "-", events[b].rebuilds, events[b].relays, events[b].settles,
                    events[b].restarts, events[b].recoveries, engine.meters().solverFallbacks.load());
    }
    bestTotal = std::min(bestTotal, total);
    result.blocks += static_cast<long>(blocks);
    result.rawWorst.emplace_back(100.0 * roundWorst / (kBlock / kRate), events[worstAt]);
    result.wallWorst.emplace_back(100.0 * roundWall / (kBlock / kRate), wallAt);
    result.fallbacks = engine.meters().solverFallbacks.load();
  }
  const double blockSeconds = kBlock / kRate;
  result.share = 100.0 * bestTotal / (static_cast<double>(blocks) * blockSeconds);
  result.worst = 100.0 * *std::max_element(fastest.begin(), fastest.end()) / blockSeconds;
  result.worstBlock = static_cast<size_t>(std::max_element(fastest.begin(), fastest.end()) - fastest.begin());
  for (size_t k = 0; k < 4 && k < blocks; ++k) result.firstBlocks[k] = 100.0 * fastest[k] / blockSeconds;
  result.secondHalfWorst = 100.0 * *std::max_element(fastest.begin() + static_cast<std::ptrdiff_t>(blocks / 2), fastest.end()) / blockSeconds;
  const double internal = static_cast<double>(blocks) * kBlock * factorUsed;
  result.nsPerInternal = bestTotal / internal * 1e9;
  if (c.stress) {
    std::vector<size_t> order(blocks);
    for (size_t b = 0; b < blocks; ++b) order[b] = b;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return fastest[a] > fastest[b]; });
    for (size_t k = 0; k < std::min<size_t>(8, blocks); ++k)
      result.slowest.emplace_back(100.0 * fastest[order[k]] / blockSeconds, events[order[k]]);
    for (size_t b = 0; b < blocks; ++b) {
      const BlockEvents& e = events[b];
      const long kinds[5] = {e.rebuilds, e.relays, e.settles, e.restarts, e.recoveries};
      const double percent = 100.0 * fastest[b] / blockSeconds;
      bool any = false;
      for (int k = 0; k < 5; ++k)
        if (kinds[k] > 0) {
          any = true;
          ++result.eventBlocks[k];
          result.eventWorst[k] = std::max(result.eventWorst[k], percent);
        }
      if (!any) result.quietWorst = std::max(result.quietWorst, percent);
    }
  }
  return result;
}

std::string loadAverage() {
#if !defined(_WIN32)
  double load[3];
  if (getloadavg(load, 3) == 3) {
    char text[64];
    std::snprintf(text, sizeof text, "%.2f %.2f %.2f", load[0], load[1], load[2]);
    return text;
  }
#endif
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    tools::printVersion("magmora-bench");
    return 0;
  }
  engine::workCounters().on.store(true);  // the stress report lists what happened in its slowest blocks
#if defined(__APPLE__)
  // a performance core; MAGMORA_BENCH_QOS=background measures an efficiency core instead (what an unprioritised
  // thread can get, as in the plug-in test host's soak)
  const char* qos = std::getenv("MAGMORA_BENCH_QOS");
  pthread_set_qos_class_self_np(qos && std::strcmp(qos, "background") == 0 ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  tools::Args args(argc, argv);
  tools::Settings settings;
  std::string input;
  double seconds = 3.0;
  int rounds = 3;
  bool check = false;
  Calibration calibration;
  std::vector<std::string> qualities = {"standard", "high", "ultra"};
  std::vector<std::string> wanted = {"typical", "hot", "torture", "sweep"};
  while (!args.empty()) {
    const std::string a = args.take("option");
    if (a == "--in") input = args.take("--in");
    else if (a == "--set") args.settings(settings);
    else if (a == "--seconds") seconds = args.number("--seconds");
    else if (a == "--rounds") rounds = std::max(1, static_cast<int>(args.number("--rounds")));
    else if (a == "--quality") qualities = tools::split(args.take("--quality"));
    else if (a == "--cases") wanted = tools::split(args.take("--cases"));
    else if (a == "--in-volts") calibration.inputVoltsPerFS = args.number("--in-volts");
    else if (a == "--values") gValues = tools::valueSetIndex(args.take("--values"));
    else if (a == "--rate") kRate = args.number("--rate");
    else if (a == "--chain") gChain = true;
    else if (a == "--chain-cab") {
      gChainCab = args.take("--chain-cab");
      gChain = true;
    } else if (a == "--chain-cab-mapping") gChainCabMapping = args.take("--chain-cab-mapping");
    else if (a == "--chain-ir1") {
      gChainIr1 = args.take("--chain-ir1");
      gChain = true;
    } else if (a == "--chain-ir") {
      gChainIr = args.take("--chain-ir");
      gChain = true;
    } else if (a == "--chain-set") {
      args.settings(gChainSettings);
      gChain = true;
    }
    else if (a == "--check") check = true;
    else if (a == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else {
      tools::fail("unknown option " + a);
    }
  }
  if (!(seconds >= 0.1 && seconds <= 600.0)) tools::fail("--seconds out of range");

  std::vector<float> signal;
  if (input.empty()) {
    signal = tools::guitarLike(kRate, seconds);
  } else {
    const tools::Audio audio = tools::readWav(input);
    if (audio.rate != kRate) tools::fail("--in must be at the --rate (default 48 kHz)");
    const size_t frames = std::min(audio.frames(), static_cast<size_t>(seconds * kRate));
    signal.assign(audio.channels[0].begin(), audio.channels[0].begin() + static_cast<std::ptrdiff_t>(frames));
    if (signal.size() < static_cast<size_t>(kBlock)) tools::fail("--in is too short");
  }
  const std::vector<float> noise = tools::fullScaleNoise(kRate, seconds);
  std::vector<float> pluck(noise.size());
  {
    const double notes[] = {82.41, 110., 98., 146.83};
    std::vector<float> one(static_cast<size_t>(kRate * 4));
    const int step = static_cast<int>(kRate / 4);
    for (size_t i = 0; i < one.size(); ++i) {
      const double t = static_cast<double>(i % step) / kRate, f = notes[(i / step) % 4];
      double x = 0.;
      for (int h = 1; h <= 8; ++h) x += std::sin(2. * tools::kBenchPi * f * h * t) * std::exp(-t * (3. + 2. * h)) / h;
      one[i] = static_cast<float>(0.4 * x * std::min(1., t * 800.));
    }
    for (size_t b = 0; b + kBlock <= pluck.size(); b += kBlock)
      std::copy_n(one.data() + b % (one.size() - kBlock), kBlock, pluck.data() + b);
  }
  std::vector<float> tone(noise.size());
  for (size_t n = 0; n < tone.size(); ++n)
    tone[n] = static_cast<float>(0.25 * std::sin(2.0 * tools::kBenchPi * 9956.1 * static_cast<double>(n) / kRate));

  std::vector<Case> cases;
  for (const std::string& name : wanted) {
    Case c{name, settings, &signal, false};
    if (name == "hot") {
      for (const char* key : {"pre_lead", "pre_rhythm"}) c.settings.emplace_back(key, "8");
      for (const char* key : {"post_lead", "post_rhythm"}) c.settings.emplace_back(key, "7");
    } else if (name == "torture") {
      c.settings.clear();
      c.settings.emplace_back("channel", "lead");
      for (int id = kPreRhythm; id <= kPresence; ++id) c.settings.emplace_back(kParams[id].key, "10");
      c.signal = &noise;
    } else if (name == "sweep") {
      c.sweep = true;
    } else if (name == "stress") {
      c.stress = true;
    } else if (name == "tone10k") {  // gate G4's hardest tone: 9956 Hz at 0.5 V, lead, every knob at 7
      c.settings.clear();
      c.settings.emplace_back("channel", "lead");
      for (int id = kPreRhythm; id <= kPresence; ++id) c.settings.emplace_back(kParams[id].key, "7");
      c.signal = &tone;
    } else if (name == "power_moves" || name == "power_moves_noise") {  // see powerMoveBlock
      c.settings.clear();
      c.settings.emplace_back("channel", "lead");
      for (int id = kPreRhythm; id <= kPresence; ++id) c.settings.emplace_back(kParams[id].key, "10");
      c.powerMoves = c.stress = true;
      if (name == "power_moves_noise") c.signal = &noise;
    } else if (name.rfind("soak", 0) == 0) {  // soak4: the plug-in soak with seed 4, on its pluck at the engine's default level
      c.settings.clear();
      c.soakSeed = std::max(1, std::atoi(name.c_str() + 4));
      c.signal = &pluck;
    } else if (name == "stress_noise") {  // the same automation on full-scale noise: the solver at its hardest
      c.stress = true;
      c.signal = &noise;
    } else if (name != "typical") {
      tools::fail("unknown case " + name);
    }
    cases.push_back(std::move(c));
  }

  std::printf("magmora-bench %s, values %s\n", versionString(), tools::valueSetDescription(gValues).c_str());
  std::printf("%.1f kHz host, %d-sample blocks, %.1f s, %s, best of %d; load average at start %s\n", kRate / 1000.0, kBlock, seconds,
              input.empty() ? "committed bench signals" : "given input", rounds, loadAverage().c_str());
  std::printf("%-9s %-8s %12s %9s %12s %8s %10s\n", "quality", "case", "ns/internal", "core %", "worst block %", "gate %",
              "fallbacks");
  bool failed = false;
  for (const std::string& quality : qualities) {
    if (quality != "standard" && quality != "high" && quality != "ultra") tools::fail("unknown quality " + quality);
    for (const Case& c : cases) {
      const Result r = run(c, quality, calibration, rounds);
      double gate = 0.0, worstGate = 0.0;
      if (quality == "standard") {
        gate = c.name == "typical" ? 40.0 : (c.name == "hot" ? 50.0 : (c.name == "torture" ? 90.0 : 0.0));
        worstGate = c.name == "sweep" || c.stress ? 90.0 : (c.name == "torture" ? 100.0 : 0.0);
      } else if (quality == "high" && c.name == "typical") {
        gate = 85.0;
      }
      const bool bad = (gate > 0.0 && r.share > gate) || (worstGate > 0.0 && r.worst > worstGate);
      failed = failed || bad;
      const double shown = c.name == "sweep" || c.stress ? worstGate : gate;
      char limit[16] = "-";
      if (shown > 0.0) std::snprintf(limit, sizeof limit, "%.0f", shown);
      std::printf("%-9s %-8s %12.1f %9.1f %13.1f %8s %10u %s\n", quality.c_str(), c.name.c_str(), r.nsPerInternal, r.share,
                  r.worst, limit, r.fallbacks, bad ? "FAIL" : "");
      {
        double thread = 0.0, wall = 0.0;
        for (const auto& w : r.rawWorst) thread = std::max(thread, w.first);
        for (const auto& w : r.wallWorst) wall = std::max(wall, w.first);
        std::printf("    as measured, every round: slowest block %.1f %% thread time, %.1f %% wall time; blocks over 100 %%: "
                    "%ld (thread) %ld (wall) of %ld; worst block (fastest of rounds) at block %zu, second half's worst %.1f %%; "
                    "blocks 0-3: %.1f %.1f %.1f %.1f %%\n",
                    thread, wall, r.overThread, r.overWall, r.blocks, r.worstBlock, r.secondHalfWorst, r.firstBlocks[0],
                    r.firstBlocks[1], r.firstBlocks[2], r.firstBlocks[3]);
      }
      if (c.powerMoves)
        std::printf("    slowest block as measured, by blocks since the last move: 0: %.1f %%, 1-3: %.1f %%, 4-39: %.1f %%, "
                    "40-399: %.1f %%, 400+: %.1f %%\n", r.sinceMove[0], r.sinceMove[1], r.sinceMove[2], r.sinceMove[3],
                    r.sinceMove[4]);
      if (c.stress) {
        const char* names[5] = {"cluster rebuilds", "relay switches", "settles", "restarts", "recoveries"};
        for (int k = 0; k < 5; ++k)
          std::printf("    blocks with %-16s %5ld, slowest %5.1f %%\n", names[k], r.eventBlocks[k], r.eventWorst[k]);
        std::printf("    blocks with none of these: slowest %5.1f %%\n", r.quietWorst);
        for (size_t k = 0; k < r.rawWorst.size(); ++k) {
          const BlockEvents& e = r.rawWorst[k].second;
          std::printf("    round %zu, slowest block as measured: %5.1f %% thread time (rebuilds %ld, relays %ld, settles %ld, "
                      "restarts %ld, recoveries %ld); %5.1f %% wall time at block %zu\n",
                      k + 1, r.rawWorst[k].first, e.rebuilds, e.relays, e.settles, e.restarts, e.recoveries,
                      r.wallWorst[k].first, r.wallWorst[k].second);
        }
      }
      if (c.stress) {
      }
      for (const auto& [percent, e] : r.slowest)
        std::printf("    slow block %5.1f %%: cluster rebuilds %ld, relay switches %ld, settles %ld, restarts %ld, recoveries %ld\n",
                    percent, e.rebuilds, e.relays, e.settles, e.restarts, e.recoveries);
      std::fflush(stdout);
    }
  }
  std::printf("load average at end %s\n", loadAverage().c_str());
  return check && failed ? 1 : 0;
}
