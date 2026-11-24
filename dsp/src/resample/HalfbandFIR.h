// One 2x stage of the resampler. Our own designs: design/design_halfbands.py.
#pragma once
#include <vector>

#include "common/Simd.h"

namespace magmora::resample {

struct HalfbandDesign {
  double lowerRate;  // serves stages whose lower rate is at least this
  const double* even;     // linear phase: taps 0, 2, 4, ... (the centre is 0.5, the other odd ones are zero)
  const double* minimum;  // minimum phase: all taps
  int taps;
  bool firstStage;
};

const HalfbandDesign& chooseDesign(double lowerRate, bool firstStage);

struct ThirdbandDesign {
  double lowerRate;
  const double* linear;   // length 3k + 1, symmetric
  const double* minimum;
  int taps;
};

const ThirdbandDesign& chooseThirdDesign(double lowerRate);

class HalfbandFIR {
 public:
  void prepare(const HalfbandDesign&, bool minimumPhase);  // allocates
  void reset();
  // As if the input had been `upInput` (for up) and `downInput` (for down) forever: the steady state of a constant.
  void reset(double upInput, double downInput);

  void up(double x, double* out);       // one sample in, two out
  double down(double first, double second);  // two samples in, one out

  // Round trip through up() and down(), in samples of the lower rate. Minimum phase reports zero.
  int delay() const { return minimum_ ? 0 : centre_; }
  int taps() const { return taps_; }

 private:
  // newest first, written twice so that a window is always contiguous
  struct History {
    std::vector<double> data;
    int length = 0, head = 0;

    void prepare(int n);
    void reset(double value = 0.0);
    MAGMORA_INLINE const double* push(double x) {
      head = head == 0 ? length - 1 : head - 1;
      double* window = data.data() + head;
      window[0] = x;
      window[length] = x;
      return window;
    }
  };

  bool minimum_ = false;
  int taps_ = 0, centre_ = 0, branch_ = 0;
  std::vector<double> even_, odd_, full_;
  History up_, downEven_, downOdd_;
};

// One 3x stage: polyphase FIR. Linear phase anchors the decimation so that a round trip is exactly (taps - 1) / 3
// samples of the lower rate; minimum phase anchors it at the newest sample and reports zero.
class ThirdbandFIR {
 public:
  static constexpr int kFactor = 3;

  void prepare(const ThirdbandDesign&, bool minimumPhase);  // allocates
  void reset();
  void up(double x, double* out);  // one sample in, three out
  double down(const double* in);   // three samples in, one out
  int delay() const { return minimum_ ? 0 : (taps_ - 1) / kFactor; }
  int taps() const { return taps_; }

 private:
  bool minimum_ = false;
  int taps_ = 0, branch_ = 0;
  std::vector<double> h_, phase_[kFactor];
  std::vector<double> upHistory_, downHistory_;
  int upHead_ = 0, downHead_ = 0;
};

}  // namespace magmora::resample
