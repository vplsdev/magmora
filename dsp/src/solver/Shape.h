// The device mix of a cluster. FixedShape turns every loop of the kernels into straight-line code;
// DynamicShape serves the DC solve and any mix that has no fixed instance (slower, same results).
#pragma once

namespace magmora::solver {

template <int T, int P, int D>
struct FixedShape {
  static constexpr int kMaxTriodes = T, kMaxPentodes = P, kMaxDiodes = D;
  static constexpr int kMaxPorts = 2 * T + 3 * P + D;
  static constexpr bool kFixed = true;

  constexpr FixedShape() = default;
  constexpr FixedShape(int, int, int) {}
  static constexpr int triodes() { return T; }
  static constexpr int pentodes() { return P; }
  static constexpr int diodes() { return D; }
  static constexpr int ports() { return kMaxPorts; }
};

struct DynamicShape {
  static constexpr int kMaxTriodes = 16, kMaxPentodes = 4, kMaxDiodes = 8;
  static constexpr int kMaxPorts = 2 * kMaxTriodes + 3 * kMaxPentodes + kMaxDiodes;
  static constexpr bool kFixed = false;

  constexpr DynamicShape() = default;
  constexpr DynamicShape(int t, int p, int d) : t_(t), p_(p), d_(d) {}
  constexpr int triodes() const { return t_; }
  constexpr int pentodes() const { return p_; }
  constexpr int diodes() const { return d_; }
  constexpr int ports() const { return 2 * t_ + 3 * p_ + d_; }
  static constexpr bool fits(int t, int p, int d) { return t <= kMaxTriodes && p <= kMaxPentodes && d <= kMaxDiodes; }

 private:
  int t_ = 0, p_ = 0, d_ = 0;
};

}  // namespace magmora::solver
