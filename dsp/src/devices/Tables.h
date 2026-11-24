// Branch-free cubic Hermite tables, built at prepare() from the closed forms in Tubes.cpp.
// Below a table's range the value is zero (segment 0); what happens above is stated per table.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/Simd.h"

namespace magmora::devices {

namespace detail {
// Power-basis coefficients of the Hermite cubic on one interval, t = 0..1.
inline void hermite(double f0, double d0, double f1, double d1, double h, double* c) {
  c[0] = f0;
  c[1] = d0 * h;
  c[2] = 3.0 * (f1 - f0) - 2.0 * d0 * h - d1 * h;
  c[3] = 2.0 * (f0 - f1) + d0 * h + d1 * h;
}
}  // namespace detail

// Uniform knots on [lo, hi]; linear continuation above hi.
template <int Segments>
class UniformTable {
 public:
  template <class Fn>  // fn(x, y, dy)
  void build(double lo, double hi, Fn fn) {
    const double h = (hi - lo) / Segments;
    x0_ = lo;
    inverseH_ = 1.0 / h;
    c_.assign(4 * (Segments + 2), 0.0);
    double f0, d0;
    fn(lo, f0, d0);
    for (int k = 0; k < Segments; ++k) {
      double f1, d1;
      fn(k + 1 == Segments ? hi : lo + (k + 1) * h, f1, d1);
      detail::hermite(f0, d0, f1, d1, h, &c_[static_cast<size_t>(4 * (k + 1))]);
      f0 = f1;
      d0 = d1;
    }
    c_[4 * (Segments + 1)] = f0;
    c_[4 * (Segments + 1) + 1] = d0 * h;
  }

  template <class V>
  struct View {
    V x0, inverseH;
    const double* data[V::kLanes];

    void bind(const UniformTable* const* tables) {
      double a[V::kLanes], b[V::kLanes];
      for (int l = 0; l < V::kLanes; ++l) {
        a[l] = tables[l]->x0_;
        b[l] = tables[l]->inverseH_;
        data[l] = tables[l]->c_.data();
      }
      x0 = V::load(a);
      inverseH = V::load(b);
    }

    MAGMORA_INLINE void eval(V x, V& y, V& dy) const {
      const V position = madd(V::set(1.0), x - x0, inverseH);
      int64_t k[V::kLanes];
      const V start = position.index(k, Segments + 1);
      const double* p[V::kLanes];
      for (int l = 0; l < V::kLanes; ++l) p[l] = data[l] + 4 * k[l];
      V c0, c1, c2, c3;
      load4(p, c0, c1, c2, c3);
      const V t = position - start;
      const V t3 = c3 * t;
      y = madd(c0, t, madd(c1, t, c2 + t3));
      dy = madd(c1, t, (c2 + c2) + (t3 + t3 + t3)) * inverseH;
    }
  };

 private:
  double x0_ = 0.0, inverseH_ = 0.0;
  std::vector<double> c_;
};

// Knots spaced evenly inside each octave, indexed from the bits of x: constant relative error without a log or root.
// Zero, negative and tiny arguments land in segment 0. At or above max() the caller uses the closed form.
template <int MinExponent, int MaxExponent, int BitsPerOctave>
class OctaveTable {
 public:
  static constexpr int kSegments = (MaxExponent - MinExponent) << BitsPerOctave;

  static double max() { return std::ldexp(1.0, MaxExponent); }

  template <class Fn>
  void build(Fn fn) {
    c_.assign(static_cast<size_t>(kStride * (kSegments + 2)), 0.0);
    double f0, d0;
    fn(knot(0), f0, d0);
    for (int k = 0; k < kSegments; ++k) {
      const double h = knot(k + 1) - knot(k);
      double f1, d1;
      fn(knot(k + 1), f1, d1);
      double* s = &c_[static_cast<size_t>(kStride * (k + 1))];
      detail::hermite(f0, d0, f1, d1, h, s);
      s[4] = 1.0 / h;
      f0 = f1;
      d0 = d1;
    }
  }

  template <class V>
  struct View {
    const double* data[V::kLanes];

    void bind(const OctaveTable* const* tables) {
      for (int l = 0; l < V::kLanes; ++l) data[l] = tables[l]->c_.data();
    }

    MAGMORA_INLINE void eval(V x, V& y, V& dy) const {
      constexpr int shift = 52 - BitsPerOctave;
      constexpr int64_t base = (static_cast<int64_t>(1023 + MinExponent) << BitsPerOctave) - 1;
      int64_t k[V::kLanes];
      const V u = x.template octaveIndex<shift>(k, base, kSegments + 1);  // below base: zero and negative x
      const double* p[V::kLanes];
      double inverseH[V::kLanes];
      for (int l = 0; l < V::kLanes; ++l) {
        p[l] = data[l] + kStride * k[l];
        inverseH[l] = p[l][4];
      }
      V c0, c1, c2, c3;
      load4(p, c0, c1, c2, c3);
      const V u3 = c3 * u;
      y = madd(c0, u, madd(c1, u, c2 + u3));
      dy = madd(c1, u, (c2 + c2) + (u3 + u3 + u3)) * V::load(inverseH);
    }
  };

 private:
  static constexpr int kStride = 6;

  static double knot(int k) {
    constexpr int perOctave = 1 << BitsPerOctave;
    return std::ldexp(1.0 + static_cast<double>(k % perOctave) / perOctave, MinExponent + k / perOctave);
  }

  std::vector<double> c_;
};

}  // namespace magmora::devices
