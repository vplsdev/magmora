// Zero-latency convolution of one input with one response (XP10). A response up to `directMax` taps is a plain
// time-domain FIR. A longer one is uniformly partitioned: its first N taps stay a time-domain FIR (no
// latency), the rest is N-tap partitions convolved by overlap-save FFTs of 2N points on a frequency-domain delay
// line. The products for partitions 2 and up only need spectra that are known at the start of a frame, so they
// are spread evenly over the frame's samples; the frame's end then does one FFT, one product and one inverse FFT.
// build() allocates (main thread); reset() and process() do not.
#pragma once
#include <cstddef>
#include <vector>

#include "cab/Fft.h"

namespace magmora::cab {

class Convolver {
 public:
  void build(const float* response, size_t length, int directMax, int maxPartition = 2048);
  void reset();
  void process(const float* in, float* out, int frames);
  int partitionSize() const { return n_; }
  int partitions() const { return p_; }

 private:
  void frameEnd();
  void unit(int j);  // accumulate partition j's product for the next frame

  int n_ = 0, p_ = 0;  // direct taps (= partition size) and the number of FFT partitions after them
  std::vector<float> reversed_, history_;  // direct part: taps in reverse, history twice over
  int write_ = 0;
  Fft fft_;
  std::vector<float> hRe_, hIm_;           // p_ partitions x (n_ + 1) bins
  std::vector<float> xRe_, xIm_;           // the delay line: p_ input spectra
  std::vector<float> accRe_, accIm_;       // next frame's tail, being accumulated
  std::vector<float> frame_;               // previous and current input frame (2 n_)
  std::vector<float> workRe_, workIm_;     // 2 n_
  std::vector<float> tail_;                // this frame's output from the partitions
  int pos_ = 0, done_ = 0, last_ = 0;
};

}  // namespace magmora::cab
