// Gate G5 (docs/VERIFICATION.md): no zipper noise, no relay clicks.
//
// A 10 ms frame cannot resolve 220 Hz lines, and a sweep legitimately modulates them, so zipper noise is isolated
// two ways:
//  1. against a reference: the same sweep with continuous automation (a new value every sample, delayed by the half
//     block that a staircase lags). Whatever differs is what block-rate automation and the engine's updates added.
//     This sees zipper noise everywhere, also underneath the harmonics. The tone's own line is taken out of the
//     difference (G5: "outside the tone and its harmonics"): a staircase and a continuous ramp round the corners of
//     a sweep differently, which is a level difference of a few thousandths of a dB that follows the ramp.
//  2. blind: above 4 kHz, where the clean tone has nothing above -80 dB, nothing may appear.
#include <algorithm>
#include <magmora/Chain.h>
#include <magmora/Engine.h>

#include <catch.hpp>

#include <cstdio>

#include "solver/DenseLU.h"
#include "support/Signals.h"

using namespace magmora;
using namespace magmora::test;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
constexpr int kFrame = 480;           // 10 ms
constexpr int kStart = 100 * kBlock;  // the sweep starts after the tone has settled
constexpr int kLength = kStart + 24000;
constexpr double kSweepSeconds = 0.050;
constexpr double kTone = 0.005;       // full scale at the input: clean on Rhythm, pre 2

void cleanAmp(Engine& e) {
  e.setParam(kChannel, 0.0);
  e.setParam(kPreRhythm, 2.0);
}

std::vector<double> sweep(ParamId id, double from, double to, int block, double delay) {
  Engine e;
  cleanAmp(e);
  e.setParam(id, from);
  REQUIRE(e.prepare(kRate, kBlock, 1));
  const std::vector<double> x = sine(kRate, 220.0, kTone, kLength);
  std::vector<double> y(x.size());
  for (int n = 0; n < kLength; n += block) {
    const double t = (n - delay - kStart) / (kSweepSeconds * kRate);
    e.setParam(id, from + (to - from) * std::min(1.0, std::max(0.0, t)));
    const double* in[1] = {x.data() + n};
    double* out[1] = {y.data() + n};
    e.process(in, out, std::min(block, kLength - n));
  }
  return y;
}

// The same sweep through the chain with the EQ on and every section doing something (bands at +12 dB, high-pass
// and low-pass at 12 dB per octave), so that a moving frequency or Q changes what the tone meets (XP3, G-E2).
std::vector<double> chainSweep(int id, double from, double to, int block, double delay) {
  Chain c;
  c.setParameter(kChannel, 0.0);
  c.setParameter(kPreRhythm, 2.0);
  c.setParameter(kEqOn, 1.0);
  c.setParameter(kEqHighPassSlope, 1.0);
  c.setParameter(kEqLowPassSlope, 1.0);
  for (int gain : {kEqBand1Gain, kEqBand2Gain, kEqBand3Gain, kEqBand4Gain}) c.setParameter(gain, 12.0);
  c.setParameter(id, from);
  REQUIRE(c.prepare(kRate, kBlock, 1));
  const std::vector<double> x = sine(kRate, 220.0, kTone, kLength);
  std::vector<double> y(x.size()), right(static_cast<size_t>(kBlock));
  for (int n = 0; n < kLength; n += block) {
    const double t = (n - delay - kStart) / (kSweepSeconds * kRate);
    c.setParameter(id, from + (to - from) * std::min(1.0, std::max(0.0, t)));
    const double* in[1] = {x.data() + n};
    double* out[2] = {y.data() + n, right.data()};
    c.process(in, out, std::min(block, kLength - n));
  }
  return y;
}

// Least squares: the tone's line with an envelope that is quadratic across the frame, and a constant.
void removeToneLine(double* frame, size_t at) {
  constexpr int kTerms = 7;
  double basis[kTerms][kFrame], normal[kTerms * kTerms] = {}, weight[kTerms] = {};
  for (int n = 0; n < kFrame; ++n) {
    const double t = 2.0 * n / (kFrame - 1) - 1.0, angle = 2 * kPi * 220.0 * (static_cast<double>(at) + n) / kRate;
    const double c = std::cos(angle), s = std::sin(angle);
    const double row[kTerms] = {c, s, t * c, t * s, t * t * c, t * t * s, 1.0};
    for (int a = 0; a < kTerms; ++a) {
      basis[a][n] = row[a];
      weight[a] += row[a] * frame[n];
      for (int b = 0; b < kTerms; ++b) normal[a * kTerms + b] += row[a] * row[b];
    }
  }
  int pivot[kTerms];
  REQUIRE(solver::luFactor(normal, pivot, kTerms));
  solver::luSolve(normal, pivot, kTerms, weight, 1, 1);
  for (int n = 0; n < kFrame; ++n)
    for (int a = 0; a < kTerms; ++a) frame[n] -= weight[a] * basis[a][n];
}

double gWorstHz = 0.0;
size_t gWorstAt = 0;
// Largest windowed component at or above `fromHz` in any 10 ms frame. The frame's straight line is removed first:
// that is the ramp itself, below 30 Hz.
double worstComponent(const std::vector<double>& y, double fromHz, bool removeTone = false) {
  double window[kFrame], windowSum = 0.0;
  for (int n = 0; n < kFrame; ++n) {
    window[n] = 0.42 - 0.5 * std::cos(2 * kPi * n / kFrame) + 0.08 * std::cos(4 * kPi * n / kFrame);
    windowSum += window[n];
  }
  static std::complex<double> phase[kFrame];
  for (int n = 0; n < kFrame; ++n) phase[n] = std::polar(1.0, -2 * kPi * n / kFrame);
  const int firstBin = std::max(1, static_cast<int>(fromHz * kFrame / kRate));
  double worst = 0.0;
  for (size_t at = static_cast<size_t>(kStart - 2 * kFrame); at + kFrame <= y.size(); at += kFrame / 2) {
    double mean = 0.0, slope = 0.0, norm = 0.0;
    for (int n = 0; n < kFrame; ++n) mean += y[at + static_cast<size_t>(n)] / kFrame;
    for (int n = 0; n < kFrame; ++n) {
      const double t = n - 0.5 * (kFrame - 1);
      slope += t * y[at + static_cast<size_t>(n)];
      norm += t * t;
    }
    slope /= norm;
    double frame[kFrame];
    for (int n = 0; n < kFrame; ++n) frame[n] = y[at + static_cast<size_t>(n)] - mean - slope * (n - 0.5 * (kFrame - 1));
    if (removeTone) removeToneLine(frame, at);
    for (int n = 0; n < kFrame; ++n) frame[n] *= window[n];
    for (int k = firstBin; k < kFrame / 2; ++k) {
      double re = 0.0, im = 0.0;
      for (int n = 0, index = 0; n < kFrame; ++n) {
        re += frame[n] * phase[index].real();
        im += frame[n] * phase[index].imag();
        index += k;
        if (index >= kFrame) index -= kFrame;
      }
      const double v = 2.0 * std::sqrt(re * re + im * im) / windowSum;
      if (v > worst) gWorstHz = k * kRate / kFrame, gWorstAt = at;
      worst = std::max(worst, v);
    }
  }
  return worst;
}

// The tone's amplitude: the louder of the two ends of the sweep.
double toneLevel(const std::vector<double>& y) {
  const auto at = [&](int from) {
    const int samples = 4800;  // 22 whole periods
    std::complex<double> sum = 0.0;
    for (int n = 0; n < samples; ++n) sum += y[static_cast<size_t>(from + n)] * std::polar(1.0, -2 * kPi * 220.0 * n / kRate);
    return 2.0 * std::abs(sum) / samples;
  };
  return std::max(at(kStart - 4800), at(kLength - 4800));
}

}  // namespace

TEST_CASE("automation: no zipper noise on any smoothed parameter (G5)") {
  for (const ParamInfo& p : kParams) {
    if (!p.smoothed) continue;
    for (int up = 0; up < 2; ++up) {
      const double from = up ? p.min : p.max, to = up ? p.max : p.min;
      const std::vector<double> stepped = sweep(p.id, from, to, kBlock, 0.0);
      const double tone = toneLevel(stepped);
      const double blind = db(worstComponent(stepped, 4000.0) / tone);
      double zipper = -999.0;
      if (p.id != kGateThreshold) {  // a threshold is not a gain: crossing it opens or closes the gate, that is all
        const std::vector<double> smooth = sweep(p.id, from, to, 1, 0.5 * (kBlock - 1));
        std::vector<double> difference(stepped.size());
        for (size_t n = 0; n < difference.size(); ++n) difference[n] = stepped[n] - smooth[n];
        zipper = db(worstComponent(difference, 30.0, true) / tone);
      }
      std::printf("G5 %-15s %s: zipper %7.1f dB, above 4 kHz %7.1f dB re the tone (gate -80)\n", p.key, up ? "up  " : "down",
                  zipper, blind);
      INFO(p.key << (up ? " up" : " down"));
      CHECK(zipper < -80.0);
      CHECK(blind < -80.0);
    }
  }
}

TEST_CASE("automation: relays do not click (G5)") {
  for (ParamId id : {kInputJack, kChannel, kBright, kCrunch, kStandby, kPower})
    for (int direction = 0; direction < 2; ++direction)
      for (double knob : {5.0, 10.0}) {
        Engine e;
        for (int k = kPreRhythm; k <= kPresence; ++k) e.setParam(static_cast<ParamId>(k), knob);
        e.setParam(id, direction);
        REQUIRE(e.prepare(kRate, kBlock, 1));
        const std::vector<double> silence(4800, 0.0);
        run(e, silence);
        e.setParam(id, 1 - direction);
        const double click = db(peak(run(e, silence), 0, 960));
        if (knob == 10.0) std::printf("G5 relay %-11s %d: %7.1f dBFS in silence (gate -60)\n", kParams[id].key, direction, click);
        INFO(kParams[id].key << " from " << direction << " knobs " << knob << ": " << click << " dBFS");
        CHECK(click < -60.0);
      }
}

TEST_CASE("automation: relays switched under a note leave no edge") {
  // what a click adds is high frequencies: compare them with the louder steady state on either side
  for (ParamId id : {kInputJack, kChannel, kBright, kCrunch}) {
    Engine e;
    cleanAmp(e);
    e.setParam(id, 0.0);
    REQUIRE(e.prepare(kRate, kBlock, 1));
    const std::vector<double> x = sine(kRate, 220.0, kTone, 24000);
    std::vector<double> y = run(e, x);
    e.setParam(id, 1.0);
    const std::vector<double> after = run(e, x);
    y.insert(y.end(), after.begin(), after.end());

    const double c = std::exp(-2 * kPi * 6000.0 / kRate);
    std::vector<double> high(y);
    for (int pass = 0; pass < 4; ++pass) {
      double previous = 0.0, state = 0.0;
      for (double& v : high) {
        state = c * (state + v - previous);
        previous = v;
        v = state;
      }
    }
    const double steady = std::max(peak(high, 19200, 24000), peak(high, 43200, 48000));
    INFO(kParams[id].key);
    CHECK(peak(high, 24000, 24960) < steady + 1e-3);
  }
}

TEST_CASE("automation: no zipper noise on any EQ control (G-E2, the G5 test through the chain)") {
  for (int id = kEqOn; id <= kEqBand4Type; ++id) {
    const ParamInfo& p = *Chain::parameterInfo(id);
    if (!p.smoothed) continue;
    for (int up = 0; up < 2; ++up) {
      const double from = up ? p.min : p.max, to = up ? p.max : p.min;
      const std::vector<double> stepped = chainSweep(id, from, to, kBlock, 0.0);
      const double tone = toneLevel(stepped);
      const double blind = db(worstComponent(stepped, 4000.0) / tone);
      const std::vector<double> smooth = chainSweep(id, from, to, 1, 0.5 * (kBlock - 1));
      std::vector<double> difference(stepped.size());
      for (size_t n = 0; n < difference.size(); ++n) difference[n] = stepped[n] - smooth[n];
      const double zipper = db(worstComponent(difference, 30.0, true) / tone);
      std::printf("G-E2 %-12s %s: zipper %7.1f dB (at %.0f Hz, sample %zu), above 4 kHz %7.1f dB re the tone (gate -80)\n",
                  p.key, up ? "up  " : "down", zipper, gWorstHz, gWorstAt, blind);
      INFO(p.key << (up ? " up" : " down"));
      CHECK(zipper < -80.0);
      CHECK(blind < -80.0);
    }
  }
}

TEST_CASE("automation: EQ switches and the pedal's bypass crossfade without an edge (G-E2, XP2)") {
  // on, a slope and a band type flipped under the tone: nothing may appear above 4 kHz. Also the head's own
  // on/off (amp_on), with the EQ on: the head crossfades with its bypass, and comes back from its operating point.
  for (int id : {int(kEqOn), int(kEqHighPassSlope), int(kEqLowPassSlope), int(kEqBand1Type), int(kEqBand4Type),
                 int(kPedalOn), int(kAmpOn)}) {
    const ParamInfo& p = *Chain::parameterInfo(id);
    for (int up = 0; up < 2; ++up) {
      Chain c;
      c.setParameter(kChannel, 0.0);
      c.setParameter(kPreRhythm, 2.0);
      c.setParameter(kEqOn, id == kEqOn ? (up ? 0.0 : 1.0) : 1.0);
      c.setParameter(kEqHighPassFreq, 400.0);  // so that the high-pass reaches the tone
      for (int gain : {kEqBand1Gain, kEqBand4Gain}) c.setParameter(gain, 12.0);
      c.setParameter(kEqBand1Freq, 250.0);
      if (id != kEqOn) c.setParameter(id, up ? p.min : p.max);
      REQUIRE(c.prepare(kRate, kBlock, 1));
      const std::vector<double> x = sine(kRate, 220.0, kTone, kLength);
      std::vector<double> y(x.size()), right(static_cast<size_t>(kBlock));
      for (int n = 0; n < kLength; n += kBlock) {
        if (n == kStart + 12000) c.setParameter(id, up ? p.max : p.min);
        const double* in[1] = {x.data() + n};
        double* out[2] = {y.data() + n, right.data()};
        c.process(in, out, kBlock);
      }
      const double blind = db(worstComponent(y, 4000.0) / toneLevel(y));
      std::printf("G-E2 switch %-12s %s: above 4 kHz %7.1f dB re the tone (gate -80)\n", p.key, up ? "up  " : "down", blind);
      INFO(p.key << (up ? " up" : " down"));
      CHECK(blind < -80.0);
    }
  }
}

TEST_CASE("automation: the head's own on/off does not click in silence (G5's relay test through the chain)") {
  for (int direction = 0; direction < 2; ++direction)
    for (double knob : {5.0, 10.0})
      for (int pedal = 0; pedal < 2; ++pedal) {
        Chain c;
        for (int k = kPreRhythm; k <= kPresence; ++k) c.setParameter(k, knob);
        c.setParameter(kPedalOn, pedal);
        c.setParameter(kAmpOn, direction);
        REQUIRE(c.prepare(kRate, kBlock, 1));
        std::vector<double> silence(4800, 0.0), y(4800), right(4800);
        const auto play = [&] {
          for (int n = 0; n < 4800; n += kBlock) {
            const double* in[1] = {silence.data() + n};
            double* out[2] = {y.data() + n, right.data() + n};
            c.process(in, out, kBlock);
          }
        };
        play();
        c.setParameter(kAmpOn, 1 - direction);
        play();
        const double click = db(peak(y, 0, 960));
        if (knob == 10.0) std::printf("G5 amp_on %d, pedal %d: %7.1f dBFS in silence (gate -60)\n", direction, pedal, click);
        INFO("amp_on from " << direction << " knobs " << knob << " pedal " << pedal << ": " << click << " dBFS");
        CHECK(click < -60.0);
      }
}

TEST_CASE("automation: the pedal's knobs and bypass (G5 through the chain, XP2)") {
  // the pedal on at moderate settings, each knob swept as the head's are; the bypass flipped under the tone
  auto run = [](int id, double from, double to, int block, double delay) {
    Chain c;
    c.setParameter(kChannel, 0.0);
    c.setParameter(kPreRhythm, 2.0);
    c.setParameter(kPedalOn, 1.0);
    c.setParameter(id, from);
    REQUIRE(c.prepare(kRate, kBlock, 1));
    const std::vector<double> x = sine(kRate, 220.0, kTone, kLength);
    std::vector<double> y(x.size()), right(static_cast<size_t>(kBlock));
    for (int n = 0; n < kLength; n += block) {
      const double t = (n - delay - kStart) / (kSweepSeconds * kRate);
      c.setParameter(id, from + (to - from) * std::min(1.0, std::max(0.0, t)));
      const double* in[1] = {x.data() + n};
      double* out[2] = {y.data() + n, right.data()};
      c.process(in, out, std::min(block, kLength - n));
    }
    return y;
  };
  for (int id : {int(kPedalDrive), int(kPedalTone), int(kPedalLevel)}) {
    const ParamInfo& p = *Chain::parameterInfo(id);
    for (int up = 0; up < 2; ++up) {
      const double from = up ? p.min : p.max, to = up ? p.max : p.min;
      const std::vector<double> stepped = run(id, from, to, kBlock, 0.0);
      const double tone = toneLevel(stepped);
      const double blind = db(worstComponent(stepped, 4000.0) / tone);
      const std::vector<double> smooth = run(id, from, to, 1, 0.5 * (kBlock - 1));
      std::vector<double> difference(stepped.size());
      for (size_t n = 0; n < difference.size(); ++n) difference[n] = stepped[n] - smooth[n];
      const double zipper = db(worstComponent(difference, 30.0, true) / tone);
      std::printf("G5 pedal %-12s %s: zipper %7.1f dB (at %.0f Hz), above 4 kHz %7.1f dB re the tone (gate -80)\n", p.key,
                  up ? "up  " : "down", zipper, gWorstHz, blind);
      INFO(p.key << (up ? " up" : " down"));
      CHECK(zipper < -80.0);
      CHECK(blind < -80.0);
    }
  }
}
