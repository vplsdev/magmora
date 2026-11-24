#include "resample/HalfbandFIR.h"

#include <algorithm>
#include <iterator>

namespace magmora::resample {
namespace {

#include "resample/HalfbandCoefficients.inc"

using simd::F2;

MAGMORA_INLINE double sum(F2 a) {
  double lane[2];
  a.store(lane);
  return lane[0] + lane[1];
}

MAGMORA_INLINE double dot(const double* a, const double* b, int n) {
  F2 s0 = F2::set(0.0), s1 = F2::set(0.0);
  int k = 0;
  for (; k + 3 < n; k += 4) {
    s0 = simd::madd(s0, F2::load(a + k), F2::load(b + k));
    s1 = simd::madd(s1, F2::load(a + k + 2), F2::load(b + k + 2));
  }
  double total = sum(s0 + s1);
  for (; k < n; ++k) total += a[k] * b[k];
  return total;
}

// Sum of c[k] (x[k] + x[n-1-k]) over half of a symmetric filter with an even number of taps.
MAGMORA_INLINE double symmetricDot(const double* c, const double* x, int n) {
  const double* tail = x + n - 1;
  double total = 0.0, total1 = 0.0;
  int k = 0;
  for (; k + 1 < n / 2; k += 2) {
    total += c[k] * (x[k] + tail[-k]);
    total1 += c[k + 1] * (x[k + 1] + tail[-(k + 1)]);
  }
  for (; k < n / 2; ++k) total += c[k] * (x[k] + tail[-k]);
  return total + total1;
}

}  // namespace

const ThirdbandDesign& chooseThirdDesign(double lowerRate) {
  const ThirdbandDesign* chosen = &kThirdDesigns[0];
  for (const ThirdbandDesign& d : kThirdDesigns)
    if (lowerRate >= d.lowerRate) chosen = &d;
  return *chosen;
}

void ThirdbandFIR::prepare(const ThirdbandDesign& design, bool minimumPhase) {
  minimum_ = minimumPhase;
  taps_ = design.taps;
  const double* h = minimumPhase ? design.minimum : design.linear;
  h_.assign(h, h + taps_);
  branch_ = (taps_ + kFactor - 1) / kFactor;
  for (int p = 0; p < kFactor; ++p) {
    phase_[p].assign(static_cast<size_t>(branch_), 0.0);
    for (int k = 0; k < branch_; ++k)
      if (k * kFactor + p < taps_) phase_[p][static_cast<size_t>(k)] = kFactor * h[k * kFactor + p];
  }
  upHistory_.assign(static_cast<size_t>(2 * branch_), 0.0);
  downHistory_.assign(static_cast<size_t>(2 * (taps_ + kFactor)), 0.0);
  reset();
}

void ThirdbandFIR::reset() {
  std::fill(upHistory_.begin(), upHistory_.end(), 0.0);
  std::fill(downHistory_.begin(), downHistory_.end(), 0.0);
  upHead_ = downHead_ = 0;
}

void ThirdbandFIR::up(double x, double* out) {
  upHead_ = upHead_ == 0 ? branch_ - 1 : upHead_ - 1;
  upHistory_[static_cast<size_t>(upHead_)] = x;
  upHistory_[static_cast<size_t>(upHead_ + branch_)] = x;
  const double* window = upHistory_.data() + upHead_;  // newest first
  for (int p = 0; p < kFactor; ++p) out[p] = dot(phase_[p].data(), window, branch_);
}

double ThirdbandFIR::down(const double* in) {
  const int length = taps_ + kFactor;
  for (int k = 0; k < kFactor; ++k) {
    downHead_ = downHead_ == 0 ? length - 1 : downHead_ - 1;
    downHistory_[static_cast<size_t>(downHead_)] = in[k];
    downHistory_[static_cast<size_t>(downHead_ + length)] = in[k];
  }
  // newest first; linear phase is anchored at the oldest of the three (a whole-sample round trip)
  return dot(h_.data(), downHistory_.data() + downHead_ + (minimum_ ? 0 : kFactor - 1), taps_);
}

const HalfbandDesign& chooseDesign(double lowerRate, bool firstStage) {
  const HalfbandDesign* chosen = nullptr;
  for (const HalfbandDesign& d : kDesigns) {
    if (d.firstStage != firstStage) continue;
    if (!chosen || lowerRate >= d.lowerRate) chosen = &d;
  }
  return *chosen;
}

void HalfbandFIR::History::prepare(int n) {
  length = n;
  data.assign(static_cast<size_t>(2 * n), 0.0);
  head = 0;
}

void HalfbandFIR::History::reset(double value) {
  std::fill(data.begin(), data.end(), value);
  head = 0;
}

void HalfbandFIR::prepare(const HalfbandDesign& design, bool minimumPhase) {
  minimum_ = minimumPhase;
  taps_ = design.taps;
  centre_ = (taps_ - 1) / 2;
  branch_ = (taps_ + 1) / 2;
  even_.clear();
  odd_.clear();
  full_.clear();
  if (minimum_) {
    for (int k = 0; k < taps_; ++k) {
      full_.push_back(design.minimum[k]);
      (k % 2 ? odd_ : even_).push_back(2.0 * design.minimum[k]);
    }
  } else {
    even_.assign(design.even, design.even + branch_);
  }
  up_.prepare(branch_);
  downEven_.prepare(minimum_ ? taps_ : branch_);
  downOdd_.prepare(minimum_ ? 1 : (centre_ + 1) / 2 + 1);
}

void HalfbandFIR::reset() {
  up_.reset();
  downEven_.reset();
  downOdd_.reset();
}

void HalfbandFIR::reset(double upInput, double downInput) {
  up_.reset(upInput);
  downEven_.reset(downInput);
  downOdd_.reset(downInput);
}

void HalfbandFIR::up(double x, double* out) {
  const double* window = up_.push(x);
  if (minimum_) {
    out[0] = dot(even_.data(), window, branch_);
    out[1] = dot(odd_.data(), window, branch_ - 1);
  } else {
    out[0] = 2.0 * symmetricDot(even_.data(), window, branch_);
    out[1] = window[(centre_ - 1) / 2];
  }
}

double HalfbandFIR::down(double first, double second) {
  if (minimum_) {  // anchored at the newer sample: the least delay
    downEven_.push(first);
    return dot(full_.data(), downEven_.push(second), taps_);
  }
  // anchored at the older sample: the round trip is exactly `centre` samples of the lower rate
  const double* even = downEven_.push(first);
  const double* odd = downOdd_.push(second);
  return symmetricDot(even_.data(), even, branch_) + 0.5 * odd[(centre_ + 1) / 2];
}

}  // namespace magmora::resample
