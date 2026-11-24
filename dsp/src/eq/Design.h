// EQ section design (XP3): analogue prototypes, matched digital second-order sections, and their
// realisation as a trapezoidal state-variable filter. Closed form; no allocation; any thread.
//
// Poles: matched (z = exp(sT) of the analogue poles), so the resonance sits where the analogue one does at every
// rate. Numerator: weighted least squares on the complex response (relative error) over the audio band and
// across the section's bandwidth, so phase is matched as well as magnitude (a three-point magnitude match was
// measured and left 0.4 to 7.8 dB on broad and high shelves at 44.1 kHz); bells and shelves also get two direct taps
// beside the section. The pair is then mapped onto the state-variable structure exactly; any stable pole pair and
// any numerator can be realised.
#pragma once
#include <complex>

namespace magmora::eq {

enum class Kind : int { HighPass, LowPass, Bell, LowShelf, HighShelf };

struct Section {
  Kind kind = Kind::Bell;
  double hz = 1000.0, gainDb = 0.0, q = 0.7071067811865476;
};

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // H(z) = (b0 + b1/z + b2/z^2) / (1 + a1/z + a2/z^2)
  double c1 = 0, c2 = 0;                           //        + c1/z + c2/z^2 (direct taps beside the section)
};

// Trapezoidal SVF: v1 = band, v2 = low; y = m0 x + m1 v1 + m2 v2 + c1 x[n-1] + c2 x[n-2]. g > 0 and k > 0 for every
// stable pair.
struct SvfCoefficients {
  double g = 0, k = 2, m0 = 1, m1 = 0, m2 = 0, c1 = 0, c2 = 0;
};

// The analogue prototype's response at angular frequency w (rad/s).
std::complex<double> analog(const Section& s, double w);
Biquad matched(const Section& s, double sampleRate);
SvfCoefficients toSvf(const Biquad& b);
std::complex<double> response(const Biquad& b, double w, double sampleRate);

}  // namespace magmora::eq
