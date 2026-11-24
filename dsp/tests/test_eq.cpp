// The EQ (XP3): design, realisation, behaviour under modulation. The full gate G-E1 is eval/check_eq.py.
#include <algorithm>
#include <magmora/Chain.h>

#include <catch.hpp>

#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "eq/Design.h"
#include "eq/Eq.h"

using namespace magmora;
using namespace magmora::eq;

namespace {
constexpr double kPi = 3.14159265358979323846;

double defaults(int k) { return kChainParams[kEqOn - kNumParams + k].def; }
}  // namespace

TEST_CASE("eq: the state-variable realisation is the designed biquad") {
  const Section sections[] = {{Kind::Bell, 1000.0, 12.0, 2.0},     {Kind::Bell, 30.0, -15.0, 10.0},
                              {Kind::LowShelf, 200.0, 9.0, 0.7},   {Kind::HighShelf, 16000.0, -15.0, 0.3},
                              {Kind::HighPass, 80.0, 0.0, 0.7071}, {Kind::LowPass, 20000.0, 0.0, 1.3066}};
  for (double rate : {176400.0, 264600.0, 288000.0, 384000.0}) {
    for (const Section& s : sections) {
      const Biquad b = matched(s, rate);
      const SvfCoefficients c = toSvf(b);
      REQUIRE(c.g > 0.0);
      REQUIRE(c.k > 0.0);
      // run the SVF on an impulse, the biquad in direct form, and compare
      double ic1 = 0, ic2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0, worst = 0, peak = 0;
      for (int n = 0; n < 4000; ++n) {
        const double x = n == 0 ? 1.0 : 0.0;
        const double a1 = 1.0 / (1.0 + c.g * (c.g + c.k)), a2 = c.g * a1, a3 = c.g * a2;
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        const double svf = c.m0 * x + c.m1 * v1 + c.m2 * v2;
        const double df = b.b0 * x + b.b1 * x1 + b.b2 * x2 - b.a1 * y1 - b.a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = df;
        worst = std::max(worst, std::fabs(svf - df));
        peak = std::max(peak, std::fabs(df));
      }
      INFO("rate " << rate << " kind " << static_cast<int>(s.kind) << " hz " << s.hz);
      CHECK(worst <= 1e-9 * peak);
    }
  }
}

TEST_CASE("eq: matched sections follow the analogue prototype at the internal rates (G-E1 spot check)") {
  const Section sections[] = {{Kind::Bell, 500.0, 15.0, 0.3},       {Kind::Bell, 2000.0, -15.0, 10.0},
                              {Kind::LowShelf, 30.0, -15.0, 10.0},   {Kind::HighShelf, 16000.0, 15.0, 10.0},
                              {Kind::HighShelf, 1000.0, -15.0, 0.3}, {Kind::HighPass, 1000.0, 0.0, 0.5412},
                              {Kind::LowPass, 1000.0, 0.0, 1.3066},  {Kind::LowPass, 20000.0, 0.0, 0.7071}};
  for (double rate : {264600.0, 288000.0}) {  // Standard at 44.1 / 88.2 and 48 / 96 kHz
    for (const Section& s : sections) {
      const Biquad b = matched(s, rate);
      double mag = 0, phase = 0;
      for (double hz = 20.0; hz <= 20000.0; hz *= 1.02) {
        const std::complex<double> r = response(b, 2 * kPi * hz, rate) / analog(s, 2 * kPi * hz);
        mag = std::max(mag, std::fabs(20 * std::log10(std::abs(r))));
        phase = std::max(phase, std::fabs(std::arg(r)) * 180 / kPi);
      }
      INFO("rate " << rate << " kind " << static_cast<int>(s.kind) << " hz " << s.hz << " gain " << s.gainDb << " q " << s.q);
      CHECK(mag < 0.1);
      CHECK(phase < 2.0);
    }
  }
}

TEST_CASE("eq: audio-rate random modulation of every control stays finite and bounded (G-E2)") {
  Eq e;
  e.prepare(288000.0);
  std::mt19937 rng(5);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::vector<double> left(6), right(6);
  double controls[kControls];
  double peak = 0.0;
  bool finite = true;
  for (int n = 0; n < 48000 * 3; ++n) {  // three seconds at 48 kHz, six internal samples each
    for (int k = 0; k < kControls; ++k) {
      const ParamInfo& p = kChainParams[kEqOn - kNumParams + k];
      controls[k] = p.min + unit(rng) * (p.max - p.min);
    }
    controls[0] = 1.0;
    e.setControls(controls, true);
    for (int i = 0; i < 6; ++i) left[i] = right[i] = 2.0 * unit(rng) - 1.0;
    e.process(left.data(), right.data(), 6);
    for (int i = 0; i < 6; ++i) {
      finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
      peak = std::max(peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
    }
  }
  CHECK(finite);
  // four bands at most +15 dB each and two Q 10 resonances can multiply a full-scale input by a few thousand at most
  CHECK(peak < 1e4);
}

TEST_CASE("eq: off, it does not touch the samples; on at neutral settings it is transparent to 1e-9") {
  Eq e;
  e.prepare(288000.0);
  double controls[kControls];
  for (int k = 0; k < kControls; ++k) controls[k] = defaults(k);
  std::vector<double> x(4096), y;
  for (size_t n = 0; n < x.size(); ++n) x[n] = std::sin(0.013 * n) * 0.7;
  y = x;
  controls[0] = 0.0;
  e.setControls(controls, false);
  CHECK(e.idle());
  e.process(y.data(), nullptr, static_cast<int>(y.size()));
  CHECK(y == x);
  controls[0] = 1.0;  // bands at 0 dB, slopes off: an identity up to rounding
  y = x;
  for (size_t at = 0; at < y.size(); at += 64) {
    e.setControls(controls, true);
    e.process(y.data() + at, nullptr, 64);
  }
  double worst = 0.0;
  for (size_t n = 0; n < x.size(); ++n) worst = std::max(worst, std::fabs(y[n] - x[n]));
  CHECK(worst < 1e-9);
}
