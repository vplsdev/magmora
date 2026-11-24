// Interpolation between two recorded microphone positions, and two microphones into one stereo response (the
// virtual microphone; a port of proto/cab/virtualmic.py, which it matches to rounding).
// Worker thread: these allocate. Responses are float64 at one sample rate.
#pragma once
#include <vector>

namespace magmora::cab::morph {

enum class Method { Snap, Morph };

// virtualmic._minimum_phase: minimum-phase response of length n with magnitude `mag` on the n_fft / 2 + 1 bins.
std::vector<double> minimumPhase(const std::vector<double>& mag, size_t n);
// virtualmic._onset: arrival time in samples (sub-sample).
double onset(const std::vector<double>& h, double fs);
// virtualmic._shift: delayed by d samples (any real d), circularly on twice the length.
std::vector<double> shift(const std::vector<double>& h, double d);
// virtualmic.warp_path on two level curves (dB on the 1/96-octave axis): j[i] for each i.
std::vector<int> warpPath(const std::vector<double>& a, const std::vector<double>& b);

// virtualmic.interpolate: the response a fraction t of the way from a to b. Snap: the nearer file. Morph: features
// matched along log frequency and moved as well as faded, minimum phase; for packs that are not minimum phase the
// result is delayed by the interpolated arrival time.
std::vector<double> interpolate(const std::vector<double>& a, const std::vector<double>& b, double t, double fs,
                                Method method, bool minimumPhasePack);

// virtualmic.relative_delay: samples by which a trails b, and the polarity of a against b (upsampled 8x
// cross-correlation, within 10 ms).
void relativeDelay(const std::vector<double>& a, const std::vector<double>& b, double fs, double& delay, int& polarity);

void panGains(double pan, double& left, double& right);  // constant power, -1..1

enum class PackPhase { Minimum = 0, Aligned = 1, Raw = 2 };

struct TwoMics {
  std::vector<double> left, right;
  double measuredDelay = 0.0;  // samples (raw packs), microphone 2 against microphone 1
  int detectedPolarity = 1;    // shown, never applied
  double moved = 0.0;          // samples microphone 2 was moved by
};
struct MicMix {
  double levelDb = 0.0, pan = 0.0;
  bool invert = false;
};
// virtualmic.two_mics. h2 may be empty: microphone 1 alone (level, pan and polarity applied).
TwoMics twoMics(const std::vector<double>& h1, const std::vector<double>& h2, double fs, const MicMix& m1,
                const MicMix& m2, bool align, double offsetMm, PackPhase phase);

}  // namespace magmora::cab::morph
