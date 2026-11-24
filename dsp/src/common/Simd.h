// One- and two-lane double vectors with one interface, so that a kernel is written once.
// NEON on arm64, SSE2 on x86-64, plain doubles elsewhere. No fast-math anywhere: NaN has to propagate.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#define MAGMORA_NEON 1
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
#define MAGMORA_SSE2 1
#include <emmintrin.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define MAGMORA_INLINE __forceinline
#define MAGMORA_NOINLINE __declspec(noinline)
#define MAGMORA_RESTRICT __restrict
#else
#define MAGMORA_INLINE inline __attribute__((always_inline))
#define MAGMORA_NOINLINE __attribute__((noinline))
#define MAGMORA_RESTRICT __restrict__
#endif

namespace magmora::simd {

struct F1 {
  static constexpr int kLanes = 1;
  double x;

  static MAGMORA_INLINE F1 set(double a) { return {a}; }
  static MAGMORA_INLINE F1 load(const double* p) { return {*p}; }
  static MAGMORA_INLINE F1 gather(const double* p, int) { return {*p}; }
  MAGMORA_INLINE void scatter(double* p, int) const { *p = x; }
  MAGMORA_INLINE void store(double* p) const { *p = x; }
  // x truncated and clamped to [0, top]; returns that index as a double. NaN gives 0.
  MAGMORA_INLINE F1 index(int64_t* out, int64_t top) const {
    out[0] = x >= static_cast<double>(top) ? top : (x > 0.0 ? static_cast<int64_t>(x) : 0);
    return {static_cast<double>(out[0])};
  }
  // The exponent and leading mantissa bits as a clamped index; returns the remaining bits as a fraction.
  template <int Shift>
  MAGMORA_INLINE F1 octaveIndex(int64_t* out, int64_t base, int64_t top) const {
    int64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    return {octaveFromBits<Shift>(bits, out, base, top)};
  }
  // octaveIndex() on the bit pattern of a double, already in an integer register
  template <int Shift>
  static MAGMORA_INLINE double octaveFromBits(int64_t bits, int64_t* out, int64_t base, int64_t top) {
    const int64_t k = (bits >> Shift) - base;
    out[0] = k < 0 ? 0 : (k > top ? top : k);
    return static_cast<double>(bits & ((int64_t{1} << Shift) - 1)) * (1.0 / static_cast<double>(int64_t{1} << Shift));
  }
};

MAGMORA_INLINE F1 operator+(F1 a, F1 b) { return {a.x + b.x}; }
MAGMORA_INLINE F1 operator-(F1 a, F1 b) { return {a.x - b.x}; }
MAGMORA_INLINE F1 operator*(F1 a, F1 b) { return {a.x * b.x}; }
MAGMORA_INLINE F1 operator/(F1 a, F1 b) { return {a.x / b.x}; }
MAGMORA_INLINE F1 madd(F1 c, F1 a, F1 b) { return {c.x + a.x * b.x}; }
MAGMORA_INLINE F1 sqrt(F1 a) { return {std::sqrt(a.x)}; }
MAGMORA_INLINE F1 max(F1 a, F1 b) { return {a.x > b.x ? a.x : b.x}; }
MAGMORA_INLINE F1 min(F1 a, F1 b) { return {a.x < b.x ? a.x : b.x}; }
MAGMORA_INLINE F1 abs(F1 a) { return {std::fabs(a.x)}; }
// x > y ? a : b
MAGMORA_INLINE F1 selectGreater(F1 x, F1 y, F1 a, F1 b) { return {x.x > y.x ? a.x : b.x}; }
MAGMORA_INLINE bool anyGreaterEqual(F1 a, F1 b) { return a.x >= b.x; }
MAGMORA_INLINE bool anyLess(F1 a, F1 b) { return a.x < b.x; }
// four coefficients per lane, transposed
MAGMORA_INLINE void load4(const double* const* p, F1& a, F1& b, F1& c, F1& d) {
  a.x = p[0][0];
  b.x = p[0][1];
  c.x = p[0][2];
  d.x = p[0][3];
}

#if MAGMORA_NEON

struct F2 {
  static constexpr int kLanes = 2;
  float64x2_t x;

  static MAGMORA_INLINE F2 set(double a) { return {vdupq_n_f64(a)}; }
  static MAGMORA_INLINE F2 load(const double* p) { return {vld1q_f64(p)}; }
  static MAGMORA_INLINE F2 gather(const double* p, int stride) { return {vcombine_f64(vld1_f64(p), vld1_f64(p + stride))}; }
  MAGMORA_INLINE void scatter(double* p, int stride) const {
    vst1q_lane_f64(p, x, 0);
    vst1q_lane_f64(p + stride, x, 1);
  }
  MAGMORA_INLINE void store(double* p) const { vst1q_f64(p, x); }
  MAGMORA_INLINE F2 index(int64_t* out, int64_t top) const {
    const float64x2_t clamped = vminnmq_f64(vmaxnmq_f64(x, vdupq_n_f64(0.0)), vdupq_n_f64(static_cast<double>(top)));
    const int64x2_t i = vcvtq_s64_f64(clamped);
    out[0] = vgetq_lane_s64(i, 0);
    out[1] = vgetq_lane_s64(i, 1);
    return {vcvtq_f64_s64(i)};
  }
  template <int Shift>
  MAGMORA_INLINE F2 octaveIndex(int64_t* out, int64_t base, int64_t top) const {
    const int64x2_t bits = vreinterpretq_s64_f64(x);
    int64x2_t k = vsubq_s64(vshrq_n_s64(bits, Shift), vdupq_n_s64(base));
    k = vbicq_s64(k, vshrq_n_s64(k, 63));
    const int64x2_t highest = vdupq_n_s64(top);
    k = vbslq_s64(vcgtq_s64(k, highest), highest, k);
    out[0] = vgetq_lane_s64(k, 0);
    out[1] = vgetq_lane_s64(k, 1);
    return {vcvtq_n_f64_s64(vandq_s64(bits, vdupq_n_s64((int64_t{1} << Shift) - 1)), Shift)};
  }
};

MAGMORA_INLINE F2 operator+(F2 a, F2 b) { return {vaddq_f64(a.x, b.x)}; }
MAGMORA_INLINE F2 operator-(F2 a, F2 b) { return {vsubq_f64(a.x, b.x)}; }
MAGMORA_INLINE F2 operator*(F2 a, F2 b) { return {vmulq_f64(a.x, b.x)}; }
MAGMORA_INLINE F2 operator/(F2 a, F2 b) { return {vdivq_f64(a.x, b.x)}; }
MAGMORA_INLINE F2 madd(F2 c, F2 a, F2 b) { return {vfmaq_f64(c.x, a.x, b.x)}; }
MAGMORA_INLINE F2 sqrt(F2 a) { return {vsqrtq_f64(a.x)}; }
MAGMORA_INLINE F2 max(F2 a, F2 b) { return {vbslq_f64(vcgtq_f64(a.x, b.x), a.x, b.x)}; }
MAGMORA_INLINE F2 min(F2 a, F2 b) { return {vbslq_f64(vcltq_f64(a.x, b.x), a.x, b.x)}; }
MAGMORA_INLINE F2 abs(F2 a) { return {vabsq_f64(a.x)}; }
MAGMORA_INLINE F2 selectGreater(F2 x, F2 y, F2 a, F2 b) { return {vbslq_f64(vcgtq_f64(x.x, y.x), a.x, b.x)}; }
MAGMORA_INLINE bool anyGreaterEqual(F2 a, F2 b) { return vmaxvq_u32(vreinterpretq_u32_u64(vcgeq_f64(a.x, b.x))) != 0; }
MAGMORA_INLINE bool anyLess(F2 a, F2 b) { return vmaxvq_u32(vreinterpretq_u32_u64(vcltq_f64(a.x, b.x))) != 0; }
MAGMORA_INLINE void load4(const double* const* p, F2& a, F2& b, F2& c, F2& d) {
  const float64x2_t a0 = vld1q_f64(p[0]), a1 = vld1q_f64(p[0] + 2);
  const float64x2_t b0 = vld1q_f64(p[1]), b1 = vld1q_f64(p[1] + 2);
  a.x = vzip1q_f64(a0, b0);
  b.x = vzip2q_f64(a0, b0);
  c.x = vzip1q_f64(a1, b1);
  d.x = vzip2q_f64(a1, b1);
}

#elif MAGMORA_SSE2

struct F2 {
  static constexpr int kLanes = 2;
  __m128d x;

  static MAGMORA_INLINE F2 set(double a) { return {_mm_set1_pd(a)}; }
  static MAGMORA_INLINE F2 load(const double* p) { return {_mm_loadu_pd(p)}; }
  static MAGMORA_INLINE F2 gather(const double* p, int stride) { return {_mm_set_pd(p[stride], p[0])}; }
  MAGMORA_INLINE void scatter(double* p, int stride) const {
    _mm_storel_pd(p, x);
    _mm_storeh_pd(p + stride, x);
  }
  MAGMORA_INLINE void store(double* p) const { _mm_storeu_pd(p, x); }
  MAGMORA_INLINE F2 index(int64_t* out, int64_t top) const {
    const __m128d c = _mm_min_pd(_mm_max_pd(x, _mm_setzero_pd()), _mm_set1_pd(static_cast<double>(top)));
    out[0] = _mm_cvttsd_si64(c);
    out[1] = _mm_cvttsd_si64(_mm_unpackhi_pd(c, c));
    return {_mm_set_pd(static_cast<double>(out[1]), static_cast<double>(out[0]))};
  }
  // The lanes' bits are moved straight to integer registers. Going through a double and memcpy (F1::octaveIndex)
  // makes MSVC 19.44 /O2 reuse that 8-byte spill slot as the whole vector later on: a misaligned, half-wrong
  // memory operand and an access violation (test_devices, PentodeBlock::eval).
  template <int Shift>
  MAGMORA_INLINE F2 octaveIndex(int64_t* out, int64_t base, int64_t top) const {
    const __m128i bits = _mm_castpd_si128(x);
    const double a = F1::octaveFromBits<Shift>(_mm_cvtsi128_si64(bits), out, base, top);
    const double b = F1::octaveFromBits<Shift>(_mm_cvtsi128_si64(_mm_unpackhi_epi64(bits, bits)), out + 1, base, top);
    return {_mm_set_pd(b, a)};
  }
};

MAGMORA_INLINE F2 operator+(F2 a, F2 b) { return {_mm_add_pd(a.x, b.x)}; }
MAGMORA_INLINE F2 operator-(F2 a, F2 b) { return {_mm_sub_pd(a.x, b.x)}; }
MAGMORA_INLINE F2 operator*(F2 a, F2 b) { return {_mm_mul_pd(a.x, b.x)}; }
MAGMORA_INLINE F2 operator/(F2 a, F2 b) { return {_mm_div_pd(a.x, b.x)}; }
MAGMORA_INLINE F2 madd(F2 c, F2 a, F2 b) { return c + a * b; }
MAGMORA_INLINE F2 sqrt(F2 a) { return {_mm_sqrt_pd(a.x)}; }
MAGMORA_INLINE F2 max(F2 a, F2 b) { return {_mm_max_pd(a.x, b.x)}; }
MAGMORA_INLINE F2 min(F2 a, F2 b) { return {_mm_min_pd(a.x, b.x)}; }
MAGMORA_INLINE F2 abs(F2 a) { return {_mm_andnot_pd(_mm_set1_pd(-0.0), a.x)}; }
MAGMORA_INLINE F2 selectGreater(F2 x, F2 y, F2 a, F2 b) {
  const __m128d m = _mm_cmpgt_pd(x.x, y.x);
  return {_mm_or_pd(_mm_and_pd(m, a.x), _mm_andnot_pd(m, b.x))};
}
MAGMORA_INLINE bool anyGreaterEqual(F2 a, F2 b) { return _mm_movemask_pd(_mm_cmpge_pd(a.x, b.x)) != 0; }
MAGMORA_INLINE bool anyLess(F2 a, F2 b) { return _mm_movemask_pd(_mm_cmplt_pd(a.x, b.x)) != 0; }
MAGMORA_INLINE void load4(const double* const* p, F2& a, F2& b, F2& c, F2& d) {
  const __m128d a0 = _mm_loadu_pd(p[0]), a1 = _mm_loadu_pd(p[0] + 2);
  const __m128d b0 = _mm_loadu_pd(p[1]), b1 = _mm_loadu_pd(p[1] + 2);
  a.x = _mm_unpacklo_pd(a0, b0);
  b.x = _mm_unpackhi_pd(a0, b0);
  c.x = _mm_unpacklo_pd(a1, b1);
  d.x = _mm_unpackhi_pd(a1, b1);
}

#else

struct F2 {
  static constexpr int kLanes = 2;
  F1 lo, hi;

  static MAGMORA_INLINE F2 set(double a) { return {{a}, {a}}; }
  static MAGMORA_INLINE F2 load(const double* p) { return {{p[0]}, {p[1]}}; }
  static MAGMORA_INLINE F2 gather(const double* p, int stride) { return {{p[0]}, {p[stride]}}; }
  MAGMORA_INLINE void scatter(double* p, int stride) const {
    p[0] = lo.x;
    p[stride] = hi.x;
  }
  MAGMORA_INLINE void store(double* p) const {
    p[0] = lo.x;
    p[1] = hi.x;
  }
  MAGMORA_INLINE F2 index(int64_t* out, int64_t top) const { return {lo.index(out, top), hi.index(out + 1, top)}; }
  template <int Shift>
  MAGMORA_INLINE F2 octaveIndex(int64_t* out, int64_t base, int64_t top) const {
    return {lo.octaveIndex<Shift>(out, base, top), hi.octaveIndex<Shift>(out + 1, base, top)};
  }
};

MAGMORA_INLINE F2 operator+(F2 a, F2 b) { return {a.lo + b.lo, a.hi + b.hi}; }
MAGMORA_INLINE F2 operator-(F2 a, F2 b) { return {a.lo - b.lo, a.hi - b.hi}; }
MAGMORA_INLINE F2 operator*(F2 a, F2 b) { return {a.lo * b.lo, a.hi * b.hi}; }
MAGMORA_INLINE F2 operator/(F2 a, F2 b) { return {a.lo / b.lo, a.hi / b.hi}; }
MAGMORA_INLINE F2 madd(F2 c, F2 a, F2 b) { return {madd(c.lo, a.lo, b.lo), madd(c.hi, a.hi, b.hi)}; }
MAGMORA_INLINE F2 sqrt(F2 a) { return {sqrt(a.lo), sqrt(a.hi)}; }
MAGMORA_INLINE F2 max(F2 a, F2 b) { return {max(a.lo, b.lo), max(a.hi, b.hi)}; }
MAGMORA_INLINE F2 min(F2 a, F2 b) { return {min(a.lo, b.lo), min(a.hi, b.hi)}; }
MAGMORA_INLINE F2 abs(F2 a) { return {abs(a.lo), abs(a.hi)}; }
MAGMORA_INLINE F2 selectGreater(F2 x, F2 y, F2 a, F2 b) {
  return {selectGreater(x.lo, y.lo, a.lo, b.lo), selectGreater(x.hi, y.hi, a.hi, b.hi)};
}
MAGMORA_INLINE bool anyGreaterEqual(F2 a, F2 b) { return anyGreaterEqual(a.lo, b.lo) || anyGreaterEqual(a.hi, b.hi); }
MAGMORA_INLINE bool anyLess(F2 a, F2 b) { return anyLess(a.lo, b.lo) || anyLess(a.hi, b.hi); }
MAGMORA_INLINE void load4(const double* const* p, F2& a, F2& b, F2& c, F2& d) {
  load4(p, a.lo, b.lo, c.lo, d.lo);
  load4(p + 1, a.hi, b.hi, c.hi, d.hi);
}

#endif

}  // namespace magmora::simd
