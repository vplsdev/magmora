// The parametric EQ (XP3): high-pass and low-pass (off, 12 or 24 dB per octave), four bands (the
// outer two bell or shelf). Minimum phase, zero latency. It runs at the head's internal rate (176.4 kHz or more),
// inside the oversampled loop, where a matched second-order section follows its analogue prototype to well within
// gate G-E1 up to 20 kHz; at the host rate it could not.
//
// Every continuous control is smoothed per sample in plain units (50 ms, with 9 ms rounding poles), section
// coefficients are recomputed every kControlSeconds while anything moves and interpolated per
// sample in between (g and k of a state-variable filter stay positive on the way, so it is stable under any
// modulation). Switches (on, slopes, band types) crossfade over 20 ms. While off and faded out, process() does
// not touch the samples.
#pragma once

#include "engine/ParamSmoother.h"
#include "eq/Design.h"

namespace magmora::eq {

constexpr int kControls = 20;    // ChainParams kEqOn .. kEqBand4Type
constexpr int kSections = 8;     // high-pass x2, low-pass x2, four bands
constexpr double kControlSeconds = 0.0004;  // between coefficient updates while a control moves (115 samples at 288 kHz)
constexpr double kFadeSeconds = 0.020;
// The smoothers (engine/ParamSmoother.h): slower than the head's knobs (30 ms, 3 ms). Measured G-E2, worst case a
// band at +12 dB swept across the tone: 30 / 3 ms -79.2 dB, 30 / 6 ms -80.9 dB, 50 / 6 ms -82.1 dB, 50 / 9 ms -84.4 dB.
// Smoothing frequency in log was worse (-69.2 dB), and so was ramping each block's new value across the block.
constexpr double kSlewEqSeconds = 0.050;
constexpr double kRoundingSeconds = 0.009;

// One SVF step with its coefficients in running form (the division done once per coefficient set).
struct Step {
  double a1, a2, a3, m0, m1, m2, c1, c2;
  static Step of(const SvfCoefficients& c) {
    const double a1 = 1.0 / (1.0 + c.g * (c.g + c.k)), a2 = c.g * a1;
    return {a1, a2, c.g * a2, c.m0, c.m1, c.m2, c.c1, c.c2};
  }
  inline double run(double x, double& ic1, double& ic2, double& x1, double& x2) const {
    const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    const double y = m0 * x + m1 * v1 + m2 * v2 + c1 * x1 + c2 * x2;
    x2 = x1;
    x1 = x;
    return y;
  }
};

class Eq {
 public:
  void prepare(double sampleRate);
  // Audio thread, once per block: the plain values of kEqOn .. kEqBand4Type, and whether this instance is wanted (on).
  void setControls(const double* plain, bool wanted);
  bool idle() const { return !wanted_ && wet_ == 0.0 && !fading_; }
  void reset();
  // In place; `right` may be null (mono).
  void process(float* left, float* right, int frames);
  void process(double* left, double* right, int frames);

  // Main thread / tests: the digital response of the current settings, all sections, as designed.
  std::complex<double> designedResponse(double hz) const;

 private:
  struct State {
    double ic1[kSections][2] = {}, ic2[kSections][2] = {};
    double x1[kSections][2] = {}, x2[kSections][2] = {};  // each section's last two inputs (its direct taps)
  };
  struct Config {  // the switches: which sections run and of what kind
    int hpStages = 0, lpStages = 0;
    Kind outer[2] = {Kind::Bell, Kind::Bell};
    bool operator!=(const Config& o) const {
      return hpStages != o.hpStages || lpStages != o.lpStages || outer[0] != o.outer[0] || outer[1] != o.outer[1];
    }
  };
  struct Core {
    Config config;
    State state;
    SvfCoefficients from[kSections], to[kSections];
    bool on[kSections] = {};
    Step fixed[kSections] = {};  // `to` in running form, for samples outside an interpolation leg
  };

  Config configOf(const double* plain) const;
  static void refresh(Core& c);
  void design(const Config& c, const double* smoothed, SvfCoefficients* out, bool* on, unsigned mask = ~0u) const;
  void snapControls(const double* plain);
  void smoothedValues(double* out) const;
  template <class Sample>
  void run(Sample* left, Sample* right, int frames);

  double rate_ = 48000.0;
  int control_ = 16;
  engine::ParamSmoother smooth_[kControls];
  double plain_[kControls] = {};
  bool wanted_ = false, fading_ = false, moving_ = false;
  double wet_ = 0.0, fadeStep_ = 0.0;      // on/off
  Core core_[2];
  int active_ = 0;
  double mix_ = 1.0;                       // weight of core_[active_] while a switch crossfades from the other
  bool switching_ = false;
  int phase_ = 0;                          // samples into the current interpolation leg
  bool leg_ = false;                       // a leg from `from` to `to` is under way
  unsigned dirty_ = 0;                     // sections whose controls moved since the last design
};

}  // namespace magmora::eq
