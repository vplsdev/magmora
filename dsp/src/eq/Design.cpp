#include "eq/Design.h"

#include <algorithm>
#include <cmath>

namespace magmora::eq {
namespace {

constexpr double kPi = 3.14159265358979323846;
using C = std::complex<double>;

// Numerator and denominator in s normalised to the section frequency: (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0).
struct Prototype {
  double n2, n1, n0, d2, d1, d0;
};

Prototype prototype(const Section& s) {
  const double q = std::max(s.q, 1e-3);
  const double a = std::pow(10.0, s.gainDb / 40.0), r = std::sqrt(a);
  switch (s.kind) {
    case Kind::HighPass: return {1, 0, 0, 1, 1 / q, 1};
    case Kind::LowPass: return {0, 0, 1, 1, 1 / q, 1};
    case Kind::Bell: return {1, a / q, 1, 1, 1 / (a * q), 1};
    case Kind::LowShelf: return {a, a * r / q, a * a, a, r / q, 1};
    case Kind::HighShelf: return {a * a, a * r / q, a, 1, r / q, a};
  }
  return {1, 0, 1, 1, 0, 1};
}

}  // namespace

std::complex<double> analog(const Section& s, double w) {
  const Prototype p = prototype(s);
  const C x(0.0, w / (2 * kPi * s.hz));
  return (p.n2 * x * x + p.n1 * x + p.n0) / (p.d2 * x * x + p.d1 * x + p.d0);
}

Biquad matched(const Section& s, double rate) {
  const Prototype p = prototype(s);
  const double w0 = 2 * kPi * s.hz, T = 1.0 / rate;
  // poles of d2 s^2 + d1 s + d0, scaled by w0
  const double b = p.d1 / p.d2, c = p.d0 / p.d2, disc = b * b - 4 * c;
  Biquad z;
  if (disc < 0) {
    const double re = -0.5 * b * w0, im = 0.5 * std::sqrt(-disc) * w0;
    z.a1 = -2 * std::exp(re * T) * std::cos(im * T);
    z.a2 = std::exp(2 * re * T);
  } else {
    // real poles: the bilinear map, which keeps a pole far above the audio band (a broad cut puts one near the
    // internal Nyquist) acting as the analogue one does below 20 kHz, where exp(sT) folds it
    const double r1 = 0.5 * (-b - std::sqrt(disc)) * w0, r2 = 0.5 * (-b + std::sqrt(disc)) * w0;
    const double p1 = (1 + 0.5 * r1 * T) / (1 - 0.5 * r1 * T), p2 = (1 + 0.5 * r2 * T) / (1 - 0.5 * r2 * T);
    z.a1 = -(p1 + p2);
    z.a2 = p1 * p2;
  }
  // Numerator by weighted least squares on the complex response (relative error), which matches phase as well as
  // magnitude: 24 points spread in log from 20 Hz to 24 kHz (or 0.45 of the rate; past the gate's 20 kHz, so the
  // fit has no edge there) and 9 across the section's own bandwidth. Basis: (1 - 1/z)^2, 1 - 1/z^2, (1 + 1/z)^2
  // over the denominator (well conditioned for a 30 Hz section at 264.6 kHz). Bells and shelves also get two direct
  // taps c1/z + c2/z^2 beside the section: with three terms a +15 dB bell at Q 0.3 at 8 or 16 kHz missed by 0.14
  // and 0.12 dB at 192 kHz (its analogue zero at 7.8 f0 lies above Nyquist); with five, 0.011 and 0.012 dB. The
  // high-pass keeps an exact double zero at DC: beta (1 - 1/z)^2 / A.
  constexpr int kSpread = 24, kLocal = 9, kPoints = kSpread + kLocal, kMax = 5;
  const double top = std::min(24000.0, 0.45 * rate);
  const bool hp = s.kind == Kind::HighPass, taps = s.kind != Kind::HighPass && s.kind != Kind::LowPass;
  const int terms = hp ? 1 : (taps ? 5 : 3);
  double m[kMax][kMax] = {}, v[kMax] = {}, x[kMax] = {};
  for (int i = 0; i < kPoints; ++i) {
    double hz = i < kSpread ? 20.0 * std::pow(top / 20.0, i / (kSpread - 1.0))
                            : s.hz * std::pow(2.0, (i - kSpread - kLocal / 2) * 0.5 / std::max(s.q, 0.5));
    hz = std::clamp(hz, 10.0, top);
    const double w = 2 * kPi * hz;
    const C z1 = std::polar(1.0, -w * T), den = 1.0 + z.a1 * z1 + z.a2 * z1 * z1;
    const C target = analog(s, w);
    const double weight = 1.0 / std::max(std::norm(target), 1e-300);
    C basis[kMax] = {(1.0 - z1) * (1.0 - z1) / den, (1.0 - z1) * (1.0 + z1) / den, (1.0 + z1) * (1.0 + z1) / den, z1,
                     z1 * z1};
    for (int r = 0; r < terms; ++r) {
      v[r] += weight * std::real(std::conj(basis[r]) * target);
      for (int col = 0; col < terms; ++col) m[r][col] += weight * std::real(std::conj(basis[r]) * basis[col]);
    }
  }
  for (int col = 0; col < terms; ++col)  // Gaussian elimination; the normal matrix is symmetric positive definite
    for (int r = col + 1; r < terms; ++r) {
      const double f = m[r][col] / m[col][col];
      for (int k = col; k < terms; ++k) m[r][k] -= f * m[col][k];
      v[r] -= f * v[col];
    }
  for (int r = terms - 1; r >= 0; --r) {
    double acc = v[r];
    for (int k = r + 1; k < terms; ++k) acc -= m[r][k] * x[k];
    x[r] = acc / m[r][r];
  }
  if (hp) {
    z.b0 = x[0];
    z.b1 = -2 * x[0];
    z.b2 = x[0];
    return z;
  }
  // back to powers of 1/z: c0 (1 - 2/z + 1/z^2) + c1 (1 - 1/z^2) + c2 (1 + 2/z + 1/z^2)
  z.b0 = x[0] + x[1] + x[2];
  z.b1 = 2 * (x[2] - x[0]);
  z.b2 = x[0] - x[1] + x[2];
  z.c1 = x[3];
  z.c2 = x[4];
  return z;
}

SvfCoefficients toSvf(const Biquad& b) {
  // denominator of the trapezoidal SVF: (1 + gk + g^2) z^2 + (2g^2 - 2) z + (1 - gk + g^2)
  SvfCoefficients s;
  const double plus = 1 + b.a1 + b.a2, minus = 1 - b.a1 + b.a2;
  s.g = std::sqrt(std::max(plus, 1e-300) / minus);
  s.k = 2 * (1 - b.a2) / (minus * s.g);
  const double delta = 1 + s.g * s.k + s.g * s.g;
  // numerator: m0 [(z-1)^2 + gk (z^2-1) + g^2 (z+1)^2] + m1 g (z^2-1) + m2 g^2 (z+1)^2 = delta (b0 z^2 + b1 z + b2)
  s.m0 = delta * (b.b0 - b.b1 + b.b2) / 4;
  s.m2 = delta * (b.b0 + b.b1 + b.b2) / (4 * s.g * s.g) - s.m0;
  s.m1 = (delta * b.b0 - s.m0 * delta - s.m2 * s.g * s.g) / s.g;
  s.c1 = b.c1;
  s.c2 = b.c2;
  return s;
}

std::complex<double> response(const Biquad& b, double w, double rate) {
  const C z1 = std::polar(1.0, -w / rate);
  return (b.b0 + b.b1 * z1 + b.b2 * z1 * z1) / (1.0 + b.a1 * z1 + b.a2 * z1 * z1) + b.c1 * z1 + b.c2 * z1 * z1;
}

}  // namespace magmora::eq
