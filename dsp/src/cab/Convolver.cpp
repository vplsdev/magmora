#include "cab/Convolver.h"

#include <algorithm>
#include <cmath>

namespace magmora::cab {
namespace {

// a dot product the compiler can vectorise: eight independent partial sums
inline float dot(const float* a, const float* b, int n) {
  float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int i = 0;
  for (; i + 8 <= n; i += 8)
    for (int k = 0; k < 8; ++k) s[k] += a[i + k] * b[i + k];
  float t = ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
  for (; i < n; ++i) t += a[i] * b[i];
  return t;
}

int nextPow2(size_t v) {
  int p = 1;
  while (static_cast<size_t>(p) < v) p <<= 1;
  return p;
}

}  // namespace

void Convolver::build(const float* h, size_t length, int directMax, int maxPartition) {
  length = std::max<size_t>(length, 1);
  if (length <= static_cast<size_t>(directMax)) {
    n_ = static_cast<int>(length);
    p_ = 0;
  } else {
    // direct cost ~ N per sample, partition cost ~ 4 L / N: balanced near N = sqrt(4 L)
    n_ = std::clamp(nextPow2(static_cast<size_t>(std::sqrt(4.0 * static_cast<double>(length)))), 256,
                    std::max(256, maxPartition));
    p_ = static_cast<int>((length - static_cast<size_t>(n_) + static_cast<size_t>(n_) - 1) / static_cast<size_t>(n_));
  }
  const size_t n = static_cast<size_t>(n_), bins = n + 1;
  reversed_.assign(n, 0.0f);
  for (size_t i = 0; i < n && i < length; ++i) reversed_[n - 1 - i] = h[i];
  history_.assign(2 * n, 0.0f);
  if (p_ > 0) {
    fft_.prepare(2 * n_);
    hRe_.assign(static_cast<size_t>(p_) * bins, 0.0f);
    hIm_.assign(static_cast<size_t>(p_) * bins, 0.0f);
    workRe_.assign(2 * n, 0.0f);
    workIm_.assign(2 * n, 0.0f);
    for (int j = 0; j < p_; ++j) {  // partition j + 1: taps (j + 1) N .. (j + 2) N - 1, zero-padded to 2N
      std::fill(workRe_.begin(), workRe_.end(), 0.0f);
      std::fill(workIm_.begin(), workIm_.end(), 0.0f);
      for (size_t i = 0; i < n; ++i) {
        const size_t tap = (static_cast<size_t>(j) + 1) * n + i;
        if (tap < length) workRe_[i] = h[tap];
      }
      fft_.forward(workRe_.data(), workIm_.data());
      std::copy_n(workRe_.begin(), bins, hRe_.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(j) * bins));
      std::copy_n(workIm_.begin(), bins, hIm_.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(j) * bins));
    }
    xRe_.assign(static_cast<size_t>(p_) * bins, 0.0f);
    xIm_.assign(static_cast<size_t>(p_) * bins, 0.0f);
    accRe_.assign(bins, 0.0f);
    accIm_.assign(bins, 0.0f);
    frame_.assign(2 * n, 0.0f);
    tail_.assign(n, 0.0f);
  }
  reset();
}

void Convolver::reset() {
  std::fill(history_.begin(), history_.end(), 0.0f);
  std::fill(xRe_.begin(), xRe_.end(), 0.0f);
  std::fill(xIm_.begin(), xIm_.end(), 0.0f);
  std::fill(accRe_.begin(), accRe_.end(), 0.0f);
  std::fill(accIm_.begin(), accIm_.end(), 0.0f);
  std::fill(frame_.begin(), frame_.end(), 0.0f);
  std::fill(tail_.begin(), tail_.end(), 0.0f);
  write_ = pos_ = done_ = last_ = 0;
}

void Convolver::unit(int j) {  // j = 2 .. p_: partition j's response times the spectrum of j - 2 frames before the last
  const size_t bins = static_cast<size_t>(n_) + 1;
  const int slot = ((last_ - (j - 2)) % p_ + p_) % p_;
  const float* xr = xRe_.data() + static_cast<size_t>(slot) * bins;
  const float* xi = xIm_.data() + static_cast<size_t>(slot) * bins;
  const float* hr = hRe_.data() + static_cast<size_t>(j - 1) * bins;
  const float* hi = hIm_.data() + static_cast<size_t>(j - 1) * bins;
  float* ar = accRe_.data();
  float* ai = accIm_.data();
  for (size_t k = 0; k < bins; ++k) {
    ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
    ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
  }
}

void Convolver::frameEnd() {
  const size_t n = static_cast<size_t>(n_), bins = n + 1;
  while (done_ < p_ - 1) unit(2 + done_++);  // whatever the spreading has not done yet
  std::copy(frame_.begin(), frame_.end(), workRe_.begin());
  std::fill(workIm_.begin(), workIm_.end(), 0.0f);
  fft_.forward(workRe_.data(), workIm_.data());
  last_ = (last_ + 1) % p_;
  std::copy_n(workRe_.begin(), bins, xRe_.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(last_) * bins));
  std::copy_n(workIm_.begin(), bins, xIm_.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(last_) * bins));
  // partition 1 with the newest spectrum, then back to time: the last N points are the next frame's tail
  for (size_t k = 0; k < bins; ++k) {
    accRe_[k] += workRe_[k] * hRe_[k] - workIm_[k] * hIm_[k];
    accIm_[k] += workRe_[k] * hIm_[k] + workIm_[k] * hRe_[k];
  }
  for (size_t k = 0; k < bins; ++k) {
    workRe_[k] = accRe_[k];
    workIm_[k] = accIm_[k];
  }
  for (size_t k = 1; k < n; ++k) {  // a real signal's spectrum: the upper half mirrors the lower
    workRe_[2 * n - k] = accRe_[k];
    workIm_[2 * n - k] = -accIm_[k];
  }
  fft_.inverse(workRe_.data(), workIm_.data());
  std::copy_n(workRe_.begin() + static_cast<std::ptrdiff_t>(n), n, tail_.begin());
  std::fill(accRe_.begin(), accRe_.end(), 0.0f);
  std::fill(accIm_.begin(), accIm_.end(), 0.0f);
  std::copy_n(frame_.begin() + static_cast<std::ptrdiff_t>(n), n, frame_.begin());
  pos_ = done_ = 0;
}

void Convolver::process(const float* in, float* out, int frames) {
  const int n = n_;
  for (int i = 0; i < frames; ++i) {
    const float x = in[i];
    history_[static_cast<size_t>(write_)] = history_[static_cast<size_t>(write_ + n)] = x;
    float y = dot(reversed_.data(), history_.data() + write_ + 1, n);  // the last n inputs, oldest first
    if (++write_ == n) write_ = 0;
    if (p_ > 0) {
      y += tail_[static_cast<size_t>(pos_)];
      frame_[static_cast<size_t>(n + pos_)] = x;
      ++pos_;
      const int target = static_cast<int>(static_cast<long>(p_ - 1) * pos_ / n);
      while (done_ < target) unit(2 + done_++);
      if (pos_ == n) frameEnd();
    }
    out[i] = y;
  }
}

}  // namespace magmora::cab
