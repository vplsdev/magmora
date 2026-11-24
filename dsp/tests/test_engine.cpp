// The engine shell.
#include <algorithm>
#include <magmora/Engine.h>

#include <catch.hpp>

#include <chrono>
#include <cstring>
#include <limits>
#include <random>
#include <string>

#include "engine/DenormalGuard.h"
#include "engine/DeveloperOptions.h"
#include "engine/Quality.h"
#include "graph/AmpGraph.h"
#include "resample/Oversampler.h"
#include "support/Signals.h"

using namespace magmora;
using namespace magmora::test;

namespace {

const double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};

std::vector<double> playing(double rate, int samples) {
  std::vector<double> x(static_cast<size_t>(samples));
  for (int n = 0; n < samples; ++n) {
    const double t = n / rate;
    x[static_cast<size_t>(n)] =
        0.15 * std::exp(-std::fmod(t, 0.1) * 25.0) * (std::sin(2 * kPi * 82.4 * t) + 0.4 * std::sin(2 * kPi * 247.0 * t));
  }
  return x;
}

}  // namespace

TEST_CASE("engine: output is identical for any partition into blocks") {
  const std::vector<double> x = playing(48000.0, 8192);
  std::vector<double> reference;
  for (int block : {4096, 1, 7, 64, 512}) {
    INFO("block " << block);
    Engine e;
    REQUIRE(e.prepare(48000.0, 4096, 1));
    // controls already moving and a relay already pending when the first block starts
    e.setParam(kPreLead, 8.0);
    e.setParam(kPresence, 1.0);
    e.setParam(kBright, 1.0);
    e.setParam(kInputTrim, 6.0);
    e.setParam(kGateThreshold, -50.0);
    const std::vector<double> y = run(e, x, block);
    if (reference.empty()) {
      reference = y;
      continue;
    }
    double worst = 0.0;
    for (size_t n = 0; n < y.size(); ++n) worst = std::max(worst, std::fabs(y[n] - reference[n]));
    CHECK(worst <= 1e-9 * peak(reference));
  }
}

TEST_CASE("engine: reported latency equals measured latency at every rate and quality") {
  // A short band-limited pulse through the engine, against the bare circuit at the internal rate fed with the same
  // analytic pulse. The error must be small at the reported delay and clearly larger one sample either side.
  for (double rate : kRates)
    for (int quality = 0; quality < 3; ++quality) {
      INFO(rate << " Hz quality " << quality);
      Engine e;
      e.setParam(kQuality, quality);
      e.setParam(kChannel, 0.0);
      e.setParam(kPreRhythm, 2.0);
      REQUIRE(e.prepare(rate, 512, 1));
      CHECK(e.oversamplingFactor() == engine::oversamplingFactor(static_cast<size_t>(quality), rate));
      const int factor = engine::oversamplingFactor(static_cast<size_t>(quality), rate);
      const int length = static_cast<int>(0.012 * rate);
      const double centre = 0.004, sigma = 60e-6;
      const auto pulse = [&](double t) { return 1e-3 * std::exp(-0.5 * (t - centre) * (t - centre) / (sigma * sigma)); };

      std::vector<double> x;
      for (int n = 0; n < length; ++n) x.push_back(pulse(n / rate) / Calibration().inputVoltsPerFS);
      const std::vector<double> measured = run(e, x, 128);

      graph::GraphOptions options;
      options.sampleRate = rate * factor;
      options.dcCapLeak = 1e-18;
      options.probes = {"sec0"};
      circuit::Controls controls;
      REQUIRE(controls.set("channel", "rhythm"));
      REQUIRE(controls.set("pre_rhythm", "2"));
      REQUIRE(controls.set("post_rhythm", "4"));  // the engine's default, not the prototype's
      REQUIRE(controls.set("post_lead", "4"));
      graph::AmpGraph amp;
      REQUIRE(amp.prepare(options, controls));
      const double* speaker = amp.output("P", "spk");
      std::vector<double> ideal;
      for (int n = 0; n < length; ++n)
        for (int k = 0; k < factor; ++k) {
          amp.processSample(pulse((n + static_cast<double>(k) / factor) / rate));
          if (k == 0) ideal.push_back(*speaker / Calibration().outputVoltsAtFS);
        }

      const int latency = e.latencySamples();
      const auto error = [&](int shift) {
        double sum = 0.0;
        for (int n = std::max(0, shift); n < length + std::min(0, shift); ++n) {
          const double d = measured[static_cast<size_t>(n)] - ideal[static_cast<size_t>(n - shift)];
          sum += d * d;
        }
        return std::sqrt(sum);
      };
      const double at = error(latency);
      double norm = 0.0;
      for (double v : ideal) norm += v * v;
      CHECK(at < 0.02 * std::sqrt(norm));
      CHECK(at < 0.5 * error(latency + 1));
      CHECK(at < 0.5 * error(latency - 1));

      e.setParam(kPhaseMode, 1.0);
      e.applyStructuralParams();
      CHECK(e.latencySamples() == 0);
    }
}

TEST_CASE("engine: structural parameters take effect through applyStructuralParams") {
  Engine e;
  REQUIRE(e.prepare(48000.0, 512, 2));
  const int standard = e.latencySamples();
  CHECK(standard <= 64);
  // Standard (6x) and High (8x) happen to have the same latency at 48 kHz (49 samples); the phase mode does not
  e.setParam(kPhaseMode, 1.0);
  CHECK(e.latencySamples() == standard);
  e.applyStructuralParams();
  CHECK(e.latencySamples() == 0);
  e.setParam(kPhaseMode, 0.0);
  e.setParam(kQuality, 2.0);
  e.applyStructuralParams();
  CHECK(e.latencySamples() != standard);
}

TEST_CASE("engine: every rate, block size and layout runs and stays finite") {
  for (double rate : kRates)
    for (int block : {1, 17, 256, 4096}) {
      Engine e;
      REQUIRE(e.prepare(rate, block, 1));
      const std::vector<double> y = run(e, playing(rate, 4096), block);
      INFO(rate << " Hz block " << block);
      CHECK(peak(y) > 1e-3);
      CHECK(peak(y) < 4.0);
    }
}

TEST_CASE("engine: channel modes") {
  const std::vector<double> left = playing(48000.0, 4800), right = sine(48000.0, 330.0, 0.05, 4800);
  const auto stereo = [&](ChannelMode mode) {
    Engine e;
    e.setParam(kChannelMode, static_cast<double>(mode));
    REQUIRE(e.prepare(48000.0, 4800, 2));
    std::vector<std::vector<double>> io = {left, right};
    double* p[2] = {io[0].data(), io[1].data()};
    e.process(p, p, 4800);  // in place
    return io;
  };
  const auto mono = [&](const std::vector<double>& x) {
    Engine e;
    REQUIRE(e.prepare(48000.0, 4800, 1));
    return run(e, x, 4800);
  };
  const auto l = stereo(ChannelMode::MonoLeft);
  CHECK(l[0] == mono(left));
  CHECK(l[1] == l[0]);

  std::vector<double> sum;
  for (size_t n = 0; n < left.size(); ++n) sum.push_back(0.5 * (left[n] + right[n]));
  const auto s = stereo(ChannelMode::MonoSum);
  CHECK(s[0] == mono(sum));
  CHECK(s[1] == s[0]);

  const auto d = stereo(ChannelMode::DualMono);
  CHECK(d[0] == mono(left));
  CHECK(d[1] == mono(right));
}

TEST_CASE("engine: float and double overloads agree") {
  const std::vector<double> x = playing(48000.0, 2048);
  std::vector<float> f(x.size());
  for (size_t i = 0; i < x.size(); ++i) f[i] = static_cast<float>(x[i]);
  std::vector<double> d(f.begin(), f.end());
  Engine a, b;
  REQUIRE(a.prepare(48000.0, 2048, 1));
  REQUIRE(b.prepare(48000.0, 2048, 1));
  float* fp[1] = {f.data()};
  double* dp[1] = {d.data()};
  a.process(fp, fp, 2048);
  b.process(dp, dp, 2048);
  for (size_t n = 0; n < d.size(); ++n) REQUIRE(f[n] == static_cast<float>(d[n]));
}

TEST_CASE("engine: prepare and reset start at the operating point, without a thump") {
  for (int channel = 0; channel < 2; ++channel) {
    for (double knob : {5.0, 10.0}) {
      Engine e;
      e.setParam(kChannel, channel);
      for (int id = kPreRhythm; id <= kPresence; ++id) e.setParam(static_cast<ParamId>(id), knob);
      REQUIRE(e.prepare(48000.0, 64, 1));
      const std::vector<double> silence(24000, 0.0);
      const double fresh = peak(run(e, silence));
      run(e, sine(48000.0, 110.0, 0.5, 9600));
      e.reset();
      const double again = peak(run(e, silence));
      INFO("channel " << channel << " knobs " << knob << ": " << db(fresh) << " / " << db(again) << " dBFS");
      CHECK(db(fresh) < -80.0);
      CHECK(db(again) < -80.0);
    }
  }
}

TEST_CASE("engine: non-finite input counts as zero") {
  std::vector<double> clean = playing(48000.0, 4800), dirty = clean;
  const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
  for (size_t n = 100; n < 4000; n += 97) {
    dirty[n] = n % 3 == 0 ? nan : (n % 3 == 1 ? inf : -inf);
    clean[n] = 0.0;
  }
  Engine a, b;
  REQUIRE(a.prepare(48000.0, 64, 1));
  REQUIRE(b.prepare(48000.0, 64, 1));
  CHECK(run(a, dirty) == run(b, clean));
}

TEST_CASE("engine: absurd input stays inside the last line of defence") {
  Engine e;
  for (int id = kPreRhythm; id <= kPresence; ++id) e.setParam(static_cast<ParamId>(id), 10.0);
  e.setParam(kInputTrim, 24.0);
  REQUIRE(e.prepare(48000.0, 64, 1));
  std::vector<double> x;
  std::mt19937 rng(2);
  std::uniform_real_distribution<double> noise(-1.0, 1.0);
  for (int n = 0; n < 4800; ++n) x.push_back(n < 2400 ? noise(rng) : noise(rng) * 1e30);
  const std::vector<double> y = run(e, x);
  for (double v : y) REQUIRE(std::isfinite(v));
  CHECK(peak(y) <= 4.0);
}

TEST_CASE("engine: power, standby and gate") {
  const std::vector<double> x = playing(48000.0, 9600), silence(4800, 0.0);
  Engine e;
  REQUIRE(e.prepare(48000.0, 64, 1));
  CHECK(peak(run(e, x)) > 0.01);

  e.setParam(kStandby, 0.0);
  std::vector<double> y = run(e, x);
  CHECK(peak(y, 600) == 0.0);
  e.setParam(kStandby, 1.0);
  CHECK(peak(run(e, x), 600) > 0.01);

  e.setParam(kPower, 0.0);
  y = run(e, x);
  CHECK(peak(y, 600) == 0.0);
  CHECK(e.meters().platePowerW.load() == 0.f);
  e.setParam(kPower, 1.0);
  CHECK(peak(run(e, x), 600) > 0.01);

  e.setParam(kGateThreshold, -30.0);
  run(e, silence);
  run(e, silence);
  run(e, silence);
  CHECK(e.meters().gateGain.load() < 0.01f);
  run(e, sine(48000.0, 220.0, 0.2, 4800));
  CHECK(e.meters().gateGain.load() > 0.99f);
  e.setParam(kGateThreshold, -96.0);
  run(e, silence);
  CHECK(e.meters().gateGain.load() > 0.99f);
}

TEST_CASE("engine: meters") {
  Engine e;
  for (ParamId id : {kPreLead, kPostLead}) e.setParam(id, 9.0);
  REQUIRE(e.prepare(48000.0, 4800, 1));
  run(e, std::vector<double>(4800, 0.0));
  CHECK(e.meters().platePowerW.load() < 0.01f);
  CHECK(e.meters().supplySag.load() < 0.02f);
  run(e, sine(48000.0, 110.0, 0.25, 24000), 4800);  // the peaks are per block: use whole periods
  INFO("power " << e.meters().platePowerW.load() << " W, sag " << e.meters().supplySag.load());
  CHECK(e.meters().inputPeak.load() == Approx(0.25).epsilon(0.05));
  CHECK(e.meters().outputPeak.load() > 0.05f);
  CHECK(e.meters().platePowerW.load() > 10.f);
  CHECK(e.meters().platePowerW.load() < 250.f);
  CHECK(e.meters().supplySag.load() > 0.02f);
  CHECK(e.meters().solverFallbacks.load() == 0u);
}

TEST_CASE("engine: FTZ is set inside process and the caller's mode is restored") {
  volatile double tiny = std::numeric_limits<double>::min();
  {
    const engine::DenormalGuard guard;
    CHECK(tiny * 0.25 == 0.0);
  }
  CHECK(tiny * 0.25 != 0.0);
  Engine e;
  REQUIRE(e.prepare(48000.0, 64, 1));
  run(e, std::vector<double>(64, 0.0));
  CHECK(tiny * 0.25 != 0.0);
}

TEST_CASE("engine: silence does not get more expensive as it decays") {
  Engine e;
  REQUIRE(e.prepare(48000.0, 64, 1));
  run(e, sine(48000.0, 440.0, 0.2, 4800));
  const std::vector<double> silence(9600, 0.0);
  const auto seconds = [&] {
    double best = 1e9;
    for (int repeat = 0; repeat < 3; ++repeat) {
      const auto t0 = std::chrono::steady_clock::now();
      run(e, silence);
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
  };
  const double early = seconds();
  for (int k = 0; k < 10; ++k) run(e, silence);
  CHECK(seconds() < 1.5 * early);
}

TEST_CASE("engine: randomised automation at 64-sample blocks stays sound") {
  Engine e;
  REQUIRE(e.prepare(48000.0, 64, 1));
  std::mt19937 rng(9);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const std::vector<double> x = playing(48000.0, 64);
  for (int block = 0; block < 4000; ++block) {
    if (block % 3 == 0) {
      const ParamInfo& p = kParams[static_cast<int>(unit(rng) * kStandby)];
      e.setParam(p.id, p.min + unit(rng) * (p.max - p.min));
    }
    const std::vector<double> y = run(e, x);
    for (double v : y) REQUIRE((std::isfinite(v) && std::fabs(v) <= 4.0));
  }
}

TEST_CASE("engine: value sets are per instance and structural") {
  REQUIRE(Engine::valueSetCount() >= 2);
  CHECK(std::string(Engine::valueSetName(Engine::defaultValueSet())) == "prototype");
  CHECK(std::string(Engine::valueSetModelHash(Engine::defaultValueSet())) == modelHash());
  const int drawn = Engine::findValueSet("as-drawn");
  REQUIRE(drawn >= 0);
  CHECK(Engine::findValueSet("no such set") == -1);
  CHECK(Engine::valueSetName(-1) == nullptr);
  CHECK(std::string(Engine::valueSetModelHash(drawn)) != modelHash());

  const std::vector<double> x = playing(48000.0, 9600);
  Engine a, b, c;
  CHECK_FALSE(b.setValueSet(Engine::valueSetCount()));
  CHECK(b.setValueSet(drawn));
  CHECK(b.valueSet() == drawn);
  REQUIRE(a.prepare(48000.0, 64, 1));
  REQUIRE(b.prepare(48000.0, 64, 1));
  REQUIRE(c.prepare(48000.0, 64, 1));
  const auto ya = run(a, x), yb = run(b, x);
  CHECK(ya != yb);                      // two instances, two value sets, side by side
  CHECK(run(c, x) == ya);
  c.setValueSet(drawn);                 // structural: nothing changes until applyStructuralParams()
  c.reset();
  Engine d;
  REQUIRE(d.prepare(48000.0, 64, 1));
  CHECK(run(c, x) == run(d, x));
}

TEST_CASE("engine: Ultra is reported as offline only") {
  CHECK(Engine::qualityIsRealtime(Quality::Standard));
  CHECK(Engine::qualityIsRealtime(Quality::High));
  CHECK_FALSE(Engine::qualityIsRealtime(Quality::Ultra));
  Engine e;
  REQUIRE(e.prepare(48000.0, 64, 1));
  CHECK(e.oversamplingFactor() == 6);
  CHECK(e.internalSampleRate() == 288000.0);
  REQUIRE(e.prepare(192000.0, 64, 1));
  CHECK(e.oversamplingFactor() == 1);
}

TEST_CASE("engine: the developer work counters count only while a tool has switched them on") {
  engine::WorkCounters& counters = engine::workCounters();
  auto total = [&] {
    return counters.clusterRebuilds.load() + counters.relaySwitches.load() + counters.settles.load() +
           counters.restarts.load() + counters.recoveries.load();
  };
  auto play = [](Engine& e) {
    e.reset();
    e.setParam(kPreRhythm, 9.0);  // a knob movement that ends: a settle and a rebuild
    run(e, playing(48000.0, 24000));
  };
  REQUIRE_FALSE(counters.on.load());  // off by default: the product never writes them
  Engine e;
  REQUIRE(e.prepare(48000.0, 64, 1));
  const long before = total();
  play(e);
  CHECK(total() == before);
  counters.on.store(true);
  e.setParam(kPreRhythm, 2.0);
  play(e);
  counters.on.store(false);
  CHECK(total() > before);
}
