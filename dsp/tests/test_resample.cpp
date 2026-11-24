// The half-band cascade: latency, response, images (gate G6).
#include <catch.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "resample/Oversampler.h"

using namespace magmora::resample;

namespace {

constexpr double kPi = 3.14159265358979323846;
const double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
const double kTargets[] = {176400.0, 264600.0, 352800.0, 705600.0};  // the old Standard, Standard, High, Ultra

std::vector<double> roundTrip(Oversampler& os, int length) {
  std::vector<double> out;
  double buffer[kMaxFactor];
  for (int n = 0; n < length; ++n) {
    os.up(n == 0 ? 1.0 : 0.0, buffer);
    out.push_back(os.down(buffer));
  }
  return out;
}

double magnitude(const std::vector<double>& h, double f) {
  std::complex<double> sum = 0.0;
  for (size_t n = 0; n < h.size(); ++n) sum += h[n] * std::polar(1.0, -2.0 * kPi * f * static_cast<double>(n));
  return 20.0 * std::log10(std::abs(sum) + 1e-30);
}

}  // namespace

TEST_CASE("resample: oversampling factors follow the internal-rate targets") {
  CHECK(factorFor(44100.0, 264600.0) == 6);
  CHECK(factorFor(48000.0, 264600.0) == 6);
  CHECK(factorFor(88200.0, 264600.0) == 3);
  CHECK(factorFor(96000.0, 264600.0) == 3);
  CHECK(factorFor(176400.0, 264600.0) == 2);
  CHECK(factorFor(192000.0, 264600.0) == 2);
  CHECK(factorFor(48000.0, 352800.0) == 8);
  CHECK(factorFor(44100.0, 176400.0) == 4);
  CHECK(factorFor(48000.0, 176400.0) == 4);
  CHECK(factorFor(88200.0, 176400.0) == 2);
  CHECK(factorFor(96000.0, 176400.0) == 2);
  CHECK(factorFor(176400.0, 176400.0) == 1);
  CHECK(factorFor(192000.0, 176400.0) == 1);
  CHECK(factorFor(48000.0, 705600.0) == 16);
  CHECK(factorFor(192000.0, 705600.0) == 4);
}

TEST_CASE("resample: linear phase is symmetric about exactly the reported latency") {
  for (double rate : kRates)
    for (double target : kTargets) {
      INFO(rate << " Hz, target " << target);
      Oversampler os;
      os.prepare(rate, factorFor(rate, target), false);
      const int latency = os.latency();
      const std::vector<double> h = roundTrip(os, 2 * latency + 1 + 64);
      size_t peak = 0;
      for (size_t n = 0; n < h.size(); ++n)
        if (std::fabs(h[n]) > std::fabs(h[peak])) peak = n;
      CHECK(static_cast<int>(peak) == latency);
      for (int k = 1; k <= latency; ++k)
        CHECK(std::fabs(h[static_cast<size_t>(latency - k)] - h[static_cast<size_t>(latency + k)]) < 1e-12);
    }
}

TEST_CASE("resample: linear mode at 48 kHz Standard stays within 64 samples, minimum phase reports zero") {
  Oversampler os;
  os.prepare(48000.0, factorFor(48000.0, kTargets[1]), false);
  WARN("latency at 48 kHz Standard, linear: " << os.latency() << " samples");
  CHECK(os.latency() <= 64);
  os.prepare(48000.0, factorFor(48000.0, kTargets[1]), true);
  CHECK(os.latency() == 0);
}

TEST_CASE("resample: the pass band is flat to 20 kHz in both phase modes") {
  for (double rate : kRates)
    for (double target : kTargets)
      for (bool minimum : {false, true}) {
        INFO(rate << " Hz, target " << target << ", minimum phase " << minimum);
        Oversampler os;
        os.prepare(rate, factorFor(rate, target), minimum);
        const std::vector<double> h = roundTrip(os, 700);
        for (double f = 0.0; f <= 20000.0; f += 500.0) CHECK(std::fabs(magnitude(h, f / rate)) < 0.01);
      }
}

TEST_CASE("resample: the 3x stage rejects images by more than 100 dB") {
  for (double rate : {88200.0, 96000.0})
    for (bool minimum : {false, true}) {
      INFO(rate << " Hz, minimum phase " << minimum);
      ThirdbandFIR f;
      f.prepare(chooseThirdDesign(rate), minimum);
      std::vector<double> h;
      for (int n = 0; n < f.taps(); ++n) {
        double three[3];
        f.up(n == 0 ? 1.0 : 0.0, three);
        for (double v : three) h.push_back(v / 3.0);
      }
      double worst = -999.0;  // images of 0 to 28 kHz, at the upper rate
      for (int k = 1; k <= 1; ++k)
        for (double f0 = k * rate - 28200.0; f0 <= 1.5 * rate; f0 += 50.0) worst = std::max(worst, magnitude(h, f0 / (3.0 * rate)));
      CHECK(worst < -100.0);
    }
}

TEST_CASE("resample: every stage rejects images by more than 100 dB") {
  for (double rate : kRates)
    for (bool first : {true, false}) {
      if (!first && rate < 86000.0) continue;
      for (bool minimum : {false, true}) {
        INFO(rate << " Hz, first stage " << first << ", minimum phase " << minimum);
        HalfbandFIR f;
        f.prepare(chooseDesign(rate, first), minimum);
        std::vector<double> h;
        for (int n = 0; n < f.taps(); ++n) {
          double pair[2];
          f.up(n == 0 ? 1.0 : 0.0, pair);
          h.push_back(0.5 * pair[0]);
          h.push_back(0.5 * pair[1]);
        }
        const double edge = 20000.0 / (2.0 * rate);  // what must not land below 20 kHz
        double worst = -999.0;
        for (double w = 0.5 - edge; w <= 0.5; w += edge / 300.0) worst = std::max(worst, magnitude(h, w));
        CHECK(worst < -100.0);
      }
    }
}
