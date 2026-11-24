// A complex radix-2 FFT on split real and imaginary arrays (XP10). Tables are made in prepare (main thread);
// forward and inverse do not allocate.
#pragma once
#include <vector>

namespace magmora::cab {

class Fft {
 public:
  void prepare(int size);  // a power of two, 2 or more
  int size() const { return n_; }
  void forward(float* re, float* im) const;
  void inverse(float* re, float* im) const;  // scaled by 1 / size

 private:
  void run(float* re, float* im, bool inverse) const;
  int n_ = 0;
  std::vector<int> reverse_;
  std::vector<float> cos_, sin_;
};

}  // namespace magmora::cab
