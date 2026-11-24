// Every section can be switched off. The head's own on/off (amp_on): appended, on by default, saved in its own
// state section; off, the signal passes pedal, EQ and cabinet at unity, at the head's latency, without the amp's cost.
// The click tests of the switch are in test_automation.cpp, with the other blocks' switches.
#include <algorithm>
#include <magmora/Chain.h>

#include <catch.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "support/Signals.h"

using namespace magmora;
using namespace magmora::test;

namespace {

// Mono in, the chain's left output (the right is checked where it matters).
std::vector<double> render(Chain& c, const std::vector<double>& x, int block = 64, std::vector<double>* rightOut = nullptr) {
  std::vector<double> y(x.size()), right(x.size());
  for (size_t at = 0; at < x.size(); at += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min(x.size() - at, static_cast<size_t>(block)));
    const double* in[1] = {x.data() + at};
    double* out[2] = {y.data() + at, right.data() + at};
    c.process(in, out, n);
  }
  if (rightOut) *rightOut = right;
  return y;
}

std::vector<double> tones(double rate, int samples) {
  std::vector<double> x(static_cast<size_t>(samples));
  for (int n = 0; n < samples; ++n) {
    const double t = n / rate;
    x[static_cast<size_t>(n)] = 0.2 * std::sin(2 * kPi * 110.0 * t) + 0.1 * std::sin(2 * kPi * 1000.0 * t) +
                                0.05 * std::sin(2 * kPi * 4000.0 * t);
  }
  return x;
}

// Amplitude of one tone over whole periods near the end of the signal.
double toneAmplitude(const std::vector<double>& y, double rate, double hz) {
  const int periods = static_cast<int>(std::floor(0.1 * hz));
  const int samples = static_cast<int>(std::lround(periods * rate / hz));
  const size_t from = y.size() - static_cast<size_t>(samples);
  std::complex<double> sum = 0.0;
  for (int n = 0; n < samples; ++n) sum += y[from + static_cast<size_t>(n)] * std::polar(1.0, -2 * kPi * hz * n / rate);
  return 2.0 * std::abs(sum) / samples;
}

// The largest difference between the output and the input delayed by `delay`, after `settle` samples.
double alignedError(const std::vector<double>& y, const std::vector<double>& x, int delay, size_t settle) {
  double worst = 0.0;
  for (size_t n = settle; n < y.size(); ++n) worst = std::max(worst, std::fabs(y[n] - x[n - static_cast<size_t>(delay)]));
  return worst;
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool hasSection(const std::vector<uint8_t>& state, const char* tag) {
  return std::search(state.begin(), state.end(), tag, tag + 4) != state.end();
}

}  // namespace

TEST_CASE("sections: amp_on is appended, on by default, automatable, and blockOn(Amp) follows it") {
  // amp_on was appended as id 71; the transposer's two followed it (2026-11-24), so amp_on keeps its id
  CHECK(kAmpOn == 71);
  CHECK(kNumChainParams == 74);
  CHECK(kTranspose == kAmpOn + 1);
  CHECK(kTransposeMode == kNumChainParams - 1);
  CHECK(Chain::findParameter("amp_on") == kAmpOn);
  const ParamInfo& p = *Chain::parameterInfo(kAmpOn);
  CHECK(std::string(p.name) == "Amp");
  CHECK(p.def == 1.0);
  CHECK(p.taper == Taper::Stepped);
  CHECK(p.automatable);
  CHECK_FALSE(Chain::parameterHidden(kAmpOn));
  // the ids before it are where they were (amp_on was appended)
  CHECK(kMic2Source == 70);
  CHECK(Chain::findParameter("mic2_source") == 70);

  Chain c;
  CHECK(c.blockOn(ChainBlock::Amp));
  CHECK(c.engine().ampOn());
  c.setParameter(kAmpOn, 0.0);
  CHECK_FALSE(c.blockOn(ChainBlock::Amp));
  CHECK_FALSE(c.engine().ampOn());
  c.setParameter(kAmpOn, 0.7);  // stepped: rounds
  CHECK(c.blockOn(ChainBlock::Amp));
}

TEST_CASE("sections: with the head off and every block off, the input leaves at unity, at the head's latency") {
  struct Setup {
    double rate;
    Quality quality;
    int block;
  };
  for (const Setup& s : {Setup{48000.0, Quality::Standard, 64}, Setup{48000.0, Quality::High, 100},
                         Setup{44100.0, Quality::Standard, 512}, Setup{96000.0, Quality::Standard, 128},
                         Setup{192000.0, Quality::High, 64}}) {
    INFO("rate " << s.rate << " quality " << static_cast<int>(s.quality));
    Chain on, off;
    for (Chain* c : {&on, &off}) c->setParameter(kQuality, static_cast<double>(s.quality));
    off.setParameter(kAmpOn, 0.0);
    REQUIRE(on.prepare(s.rate, s.block, 1));
    REQUIRE(off.prepare(s.rate, s.block, 1));
    const int latency = off.latencySamples();
    CHECK(latency == on.latencySamples());  // constant: the bypass runs in the same oversampled loop

    // a dirac arrives exactly at the reported latency
    std::vector<double> dirac(static_cast<size_t>(latency + 4096), 0.0);
    dirac[0] = 0.5;
    const std::vector<double> response = render(off, dirac, s.block);
    size_t at = 0;
    for (size_t n = 0; n < response.size(); ++n)
      if (std::fabs(response[n]) > std::fabs(response[at])) at = n;
    CHECK(static_cast<int>(at) == latency);
    double sum = 0.0;  // the resamplers' band-limited dirac: its peak is lower, its area is the dirac's
    for (double v : response) sum += v;
    CHECK(sum == Approx(0.5).margin(0.001));

    // tones at 110 Hz, 1 kHz and 4 kHz: the delayed input, sample for sample, on both sides
    off.reset();
    const std::vector<double> x = tones(s.rate, static_cast<int>(0.5 * s.rate));
    std::vector<double> right;
    const std::vector<double> y = render(off, x, s.block, &right);
    const double error = alignedError(y, x, latency, static_cast<size_t>(latency) + 2048);
    std::printf("head off %6.0f Hz q%d: latency %d, worst error %.2e of 0.35 (%.1f dB)\n", s.rate,
                static_cast<int>(s.quality), latency, error, db(error / 0.35));
    CHECK(error < 0.35 * 1e-3);  // -60 dB: the resamplers' pass band, nothing else
    CHECK(right == y);
    for (double hz : {110.0, 1000.0, 4000.0}) CHECK(db(toneAmplitude(y, s.rate, hz) / toneAmplitude(x, s.rate, hz)) == Approx(0.0).margin(0.01));
  }
}

TEST_CASE("sections: with the head off, the input trim, gate and output level still apply; power and standby do not") {
  const double rate = 48000.0;
  const auto gain = [&](std::initializer_list<std::pair<int, double>> settings, double level = 0.1) {
    Chain c;
    c.setParameter(kAmpOn, 0.0);
    for (const auto& [id, v] : settings) c.setParameter(id, v);
    REQUIRE(c.prepare(rate, 64, 1));
    return db(toneAmplitude(render(c, sine(rate, 1000.0, level, 24000)), rate, 1000.0) / level);
  };
  const std::vector<double> x = sine(rate, 1000.0, 0.1, 24000);
  CHECK(gain({}) == Approx(0.0).margin(0.01));
  CHECK(gain({{kOutputLevel, -6.0}}) == Approx(-6.0).margin(0.02));
  CHECK(gain({{kInputTrim, 6.0}}) == Approx(6.0).margin(0.02));
  CHECK(gain({{kPower, 0.0}}) == Approx(0.0).margin(0.01));    // the head's own switches
  CHECK(gain({{kStandby, 0.0}}) == Approx(0.0).margin(0.01));
  CHECK(gain({{kGateThreshold, -20.0}}, 0.01) < -40.0);         // the gate is the plug-in's: -20 dB shuts a -40 dB tone
  // the head on with its power off is silent, as it always was
  Chain silent;
  silent.setParameter(kPower, 0.0);
  REQUIRE(silent.prepare(rate, 64, 1));
  CHECK(peak(render(silent, x)) == 0.0);
}

TEST_CASE("sections: with the head off, the pedal, the EQ and the cabinet act as they do behind the head") {
  const double rate = 48000.0;
  const std::vector<double> x = sine(rate, 400.0, 0.05, 48000);
  SECTION("EQ: a +6 dB bell at 400 Hz gives +6 dB") {
    Chain c;
    c.setParameter(kAmpOn, 0.0);
    c.setParameter(kEqOn, 1.0);
    c.setParameter(kEqBand2Freq, 400.0);
    c.setParameter(kEqBand2Gain, 6.0);
    REQUIRE(c.prepare(rate, 64, 1));
    CHECK(db(toneAmplitude(render(c, x), rate, 400.0) / 0.05) == Approx(6.0).margin(0.1));
  }
  SECTION("cabinet: a one-sample response in slot 1, centred at +3.01 dB, is unity, with no added delay") {
    Chain c;
    c.setParameter(kAmpOn, 0.0);
    c.setParameter(kCabOn, 1.0);
    c.setParameter(kMic1On, 1.0);
    c.setParameter(kMic1Source, 1.0);
    c.setParameter(kMic1Level, 3.0103);  // the slot's constant-power centre is -3.01 dB per side
    REQUIRE(c.prepare(rate, 64, 1));
    const float one = 1.0f;
    const float* channels[1] = {&one};
    REQUIRE(c.loadCabinetIrSamples(0, channels, 1, 1, rate));
    REQUIRE(c.waitForCabinet());
    render(c, std::vector<double>(4800, 0.0));  // the 20 ms crossfade to the response
    c.reset();
    std::vector<double> right;
    const std::vector<double> y = render(c, x, 64, &right);
    const int latency = c.latencySamples();
    CHECK(db(toneAmplitude(y, rate, 400.0) / 0.05) == Approx(0.0).margin(0.02));
    CHECK(db(toneAmplitude(right, rate, 400.0) / 0.05) == Approx(0.0).margin(0.02));
    CHECK(alignedError(y, x, latency, 4800) < 0.05 * 3e-3);
  }
  SECTION("pedal: in the path, no offset in silence, a sensible level") {
    Chain c;
    c.setParameter(kAmpOn, 0.0);
    c.setParameter(kPedalOn, 1.0);
    REQUIRE(c.prepare(rate, 64, 1));
    const std::vector<double> quiet = render(c, std::vector<double>(48000, 0.0));
    CHECK(peak(quiet, 24000) < 1e-4);
    const std::vector<double> y = render(c, x);
    const double level = db(toneAmplitude(y, rate, 400.0) / 0.05);
    std::printf("head off, pedal on (drive, tone, level 5): 400 Hz at %+.1f dB\n", level);
    CHECK(level > -20.0);
    CHECK(level < 20.0);
    CHECK(alignedError(y, x, c.latencySamples(), 4800) > 1e-3);  // the pedal changes the signal
  }
}

TEST_CASE("sections: the latency does not depend on any section being on") {
  for (double rate : {44100.0, 48000.0, 96000.0, 192000.0})
    for (int q = 0; q < 2; ++q)
      for (int phase = 0; phase < 2; ++phase) {
        Chain all;
        all.setParameter(kQuality, q);
        all.setParameter(kPhaseMode, phase);
        REQUIRE(all.prepare(rate, 64, 2));
        const int base = all.latencySamples();
        for (int mask = 0; mask < 16; ++mask) {
          Chain c;
          c.setParameter(kQuality, q);
          c.setParameter(kPhaseMode, phase);
          c.setParameter(kPedalOn, mask & 1);
          c.setParameter(kAmpOn, (mask >> 1) & 1);
          c.setParameter(kEqOn, (mask >> 2) & 1);
          c.setParameter(kCabOn, (mask >> 3) & 1);
          REQUIRE(c.prepare(rate, 64, 2));
          CHECK(c.latencySamples() == base);
          c.setParameter(kAmpOn, 1 - ((mask >> 1) & 1));  // switched while running: no structural change
          CHECK(c.latencySamples() == base);
        }
      }
}

TEST_CASE("sections: with the head off its circuit is not computed") {
  const double rate = 48000.0;
  std::mt19937 random(3);
  std::uniform_real_distribution<double> noise(-0.3, 0.3);
  std::vector<double> x(static_cast<size_t>(2 * rate));
  for (double& v : x) v = noise(random);
  const auto seconds = [&](bool head) {
    Chain c;
    c.setParameter(kChannel, 1.0);
    c.setParameter(kAmpOn, head ? 1.0 : 0.0);
    REQUIRE(c.prepare(rate, 64, 1));
    render(c, std::vector<double>(4800, 0.0));
    const auto t0 = std::chrono::steady_clock::now();
    render(c, x);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!head) CHECK(c.meters().platePowerW.load() == 0.f);
    return t;
  };
  const double on = seconds(true), off = seconds(false);
  std::printf("head off CPU, 2 s of noise at 48 kHz Standard: head on %.3f s, off %.3f s (%.1f %%)\n", on, off, 100.0 * off / on);
  CHECK(off < 0.25 * on);
}

TEST_CASE("sections: the head switched off and on again comes back to its own sound") {
  const double rate = 48000.0;
  const std::vector<double> x = sine(rate, 220.0, 0.05, 24000);
  Chain ref, c;
  ref.setParameter(kChannel, 0.0);
  c.setParameter(kChannel, 0.0);
  REQUIRE(ref.prepare(rate, 64, 1));
  REQUIRE(c.prepare(rate, 64, 1));
  const double steady = toneAmplitude(render(ref, x), rate, 220.0);
  render(c, x);
  c.setParameter(kAmpOn, 0.0);
  const std::vector<double> bypassed = render(c, x);
  CHECK(db(toneAmplitude(bypassed, rate, 220.0) / 0.05) == Approx(0.0).margin(0.01));
  c.setParameter(kAmpOn, 1.0);
  std::vector<double> back = render(c, x);
  back = render(c, x);
  bool finite = true;
  for (double v : back) finite = finite && std::isfinite(v);
  CHECK(finite);
  CHECK(db(toneAmplitude(back, rate, 220.0) / steady) == Approx(0.0).margin(0.1));
}

TEST_CASE("sections: amp_on survives the state, and sessions from before amp_on load with the head on") {
  Chain a;
  a.setParameter(kAmpOn, 0.0);
  a.setParameter(kPedalOn, 1.0);
  a.setParameter(kPreLead, 3.5);
  std::vector<uint8_t> state;
  a.getState(state);
  int32_t version = 0;
  std::memcpy(&version, state.data() + 4, 4);
  CHECK(version == Chain::kStateVersion);
  CHECK(Chain::kStateVersion == 2);
  CHECK(hasSection(state, "amp "));
  Chain b;
  REQUIRE(b.setState(state.data(), state.size()));
  CHECK(b.getParameter(kAmpOn) == 0.0);
  CHECK_FALSE(b.engine().ampOn());
  std::vector<uint8_t> again;
  b.getState(again);
  CHECK(again == state);
  a.setParameter(kAmpOn, 1.0);
  a.getState(state);
  REQUIRE(b.setState(state.data(), state.size()));
  CHECK(b.getParameter(kAmpOn) == 1.0);

  // A chain state written by Chain::getState before amp_on (version 1, no "amp " section; committed fixture made with
  // the code of that time: pedal on drive 2.5, EQ on band 2 +4.5 dB, cabinet on mic 1 at -3 dB, rhythm pre 6.5, mid 3,
  // output -4 dB, preset "Talus" at 125 %).
  const std::string fixture = readFile(std::filesystem::path(MAGMORA_SOURCE_DIR) / "dsp/tests/data/chain-state-a31.bin");
  REQUIRE(fixture.size() > 100);
  const std::vector<uint8_t> old(fixture.begin(), fixture.end());
  std::memcpy(&version, old.data() + 4, 4);
  CHECK(version == 1);
  CHECK_FALSE(hasSection(old, "amp "));
  Chain c;
  c.setParameter(kAmpOn, 0.0);  // whatever the instance held, an old session plays the head
  REQUIRE(c.setState(old.data(), old.size()));
  CHECK(c.getParameter(kAmpOn) == 1.0);
  CHECK(c.engine().ampOn());
  CHECK(c.getParameter(kPedalOn) == 1.0);
  CHECK(c.getParameter(kPedalDrive) == 2.5);
  CHECK(c.getParameter(kEqOn) == 1.0);
  CHECK(c.getParameter(kEqBand2Gain) == 4.5);
  CHECK(c.getParameter(kCabOn) == 1.0);
  CHECK(c.getParameter(kMic1Level) == -3.0);
  CHECK(c.getParameter(kPreRhythm) == 6.5);
  CHECK(std::string(c.presetName()) == "Talus");
  // and it sounds as the same settings made by hand
  Chain d;
  for (int id = 0; id < kNumChainParams; ++id) d.setParameter(id, c.getParameter(id));
  REQUIRE(c.prepare(48000.0, 64, 1));
  REQUIRE(d.prepare(48000.0, 64, 1));
  const std::vector<double> x = sine(48000.0, 110.0, 0.2, 9600);
  CHECK(render(c, x) == render(d, x));
  // saved again, it is a version 2 state with the head on
  std::vector<uint8_t> resaved;
  c.getState(resaved);
  CHECK(hasSection(resaved, "amp "));
  Chain e;
  e.setParameter(kAmpOn, 0.0);
  REQUIRE(e.setState(resaved.data(), resaved.size()));
  CHECK(e.getParameter(kAmpOn) == 1.0);

  // a 0.1.0 head state (no chain at all) also plays the head
  const std::string head = readFile(std::filesystem::path(MAGMORA_SOURCE_DIR) / "dsp/tests/data/head-state-0.1.0.bin");
  Chain f;
  f.setParameter(kAmpOn, 0.0);
  REQUIRE(f.setState(reinterpret_cast<const uint8_t*>(head.data()), head.size()));
  CHECK(f.getParameter(kAmpOn) == 1.0);
}

TEST_CASE("sections: presets carry amp_on in \"params\"; every shipped preset plays the head") {
  Chain c;
  const std::string off = R"({"params": {"amp_on": 0, "pre_lead": 7}})";
  REQUIRE(c.applyPreset(off.data(), off.size()));
  CHECK(c.getParameter(kAmpOn) == 0.0);
  const std::string blocks = R"({"cab": {"cab_on": 1}})";  // no "params": the head is left as it is
  REQUIRE(c.applyPreset(blocks.data(), blocks.size()));
  CHECK(c.getParameter(kAmpOn) == 0.0);
  const std::string head = R"({"params": {"pre_lead": 7}})";  // "params" without amp_on: its default, on
  REQUIRE(c.applyPreset(head.data(), head.size()));
  CHECK(c.getParameter(kAmpOn) == 1.0);

  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(MAGMORA_SOURCE_DIR) / "presets")) {
    if (entry.path().extension() != ".json") continue;
    ++count;
    INFO(entry.path().filename().string());
    const std::string json = readFile(entry.path());
    Chain p;
    p.setParameter(kAmpOn, 0.0);
    REQUIRE(p.applyPreset(json.data(), json.size()));
    CHECK(p.getParameter(kAmpOn) == 1.0);
  }
  CHECK(count >= 16);
}
