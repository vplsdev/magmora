#include "cab/Fft.h"

#include <cmath>
#include <utility>

namespace magmora::cab {

void Fft::prepare(int size) {
  n_ = size;
  int bits = 0;
  while ((1 << bits) < size) ++bits;
  reverse_.assign(static_cast<size_t>(size), 0);
  for (int i = 0; i < size; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
    reverse_[static_cast<size_t>(i)] = r;
  }
  cos_.resize(static_cast<size_t>(size / 2));
  sin_.resize(static_cast<size_t>(size / 2));
  for (int k = 0; k < size / 2; ++k) {
    const double a = -2.0 * 3.14159265358979323846 * k / size;
    cos_[static_cast<size_t>(k)] = static_cast<float>(std::cos(a));
    sin_[static_cast<size_t>(k)] = static_cast<float>(std::sin(a));
  }
}

void Fft::run(float* re, float* im, bool inverse) const {
  const int n = n_;
  for (int i = 0; i < n; ++i) {
    const int r = reverse_[static_cast<size_t>(i)];
    if (r > i) {
      std::swap(re[i], re[r]);
      std::swap(im[i], im[r]);
    }
  }
  const float sign = inverse ? -1.0f : 1.0f;
  for (int len = 2; len <= n; len <<= 1) {
    const int half = len >> 1, step = n / len;
    for (int start = 0; start < n; start += len)
      for (int k = 0; k < half; ++k) {
        const float wr = cos_[static_cast<size_t>(k * step)], wi = sign * sin_[static_cast<size_t>(k * step)];
        const int a = start + k, b = a + half;
        const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
      }
  }
  if (inverse) {
    const float scale = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      re[i] *= scale;
      im[i] *= scale;
    }
  }
}

void Fft::forward(float* re, float* im) const { run(re, im, false); }
void Fft::inverse(float* re, float* im) const { run(re, im, true); }

}  // namespace magmora::cab
