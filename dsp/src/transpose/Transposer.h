// Magmora DSP - a low-latency pitch transposer for a guitar DI (added 2026-11-24).
//
// A phase vocoder with Laroche-Dolson peak shifting ("New phase-vocoder techniques for pitch-shifting, harmonizing
// and other exotic effects", WASPAA 1999): each spectral peak's region of influence is moved, as a whole, to the
// peak's new frequency, and the region's bins are rotated together (identity phase locking) by a phase that
// accumulates hop to hop, so the moved partial stays continuous. The delay is not the analysis length: the analysis
// window is long (2048 samples at 44.1/48 kHz, for the frequency resolution) but asymmetric, rising slowly and
// falling over the newest L/2 samples, and the synthesis window covers only the newest L samples, so the output is
// final L samples after the input arrived (L = latencySamples()). Analysis x synthesis is a Hann of length L, which
// overlap-adds to a constant at the hop L / overlap.
//
//   mode       latency (all rates)   48 kHz: N / L / hop     44.1 kHz          96 kHz            FFT
//   Tight      5.3 ms                2048 / 255 / 85          2048 / 234 / 78   4096 / 513 / 171  2 N (zero-padded)
//   Standard   10.7 ms               2048 / 512 / 128         2048 / 472 / 118  4096 / 1024 / 256 N
//
// A peak is a bin larger than its neighbours and no more than 80 dB below the frame's largest bin; there is no
// absolute floor, so the result does not depend on the input's level (quiet high partials of a distorted or
// compressed guitar keep regions of their own and move to their own frequencies).
//
// Additions over the textbook method, each measured on a guitar DI and on test tones:
//  - the synthesis windows overlap three times (Tight) and four times (Standard) instead of twice: the frame-rate
//    sidebands of a pure tone drop from about -42 to -54 dB (Tight) and from -35 to -55 dB (Standard);
//  - Tight zero-pads its FFT to twice the analysis length, so that a region moves by whole steps of 11.7 Hz at 48 kHz
//    instead of 23.4 Hz (the same delay and analysis window, a finer grid for the moves);
//  - the integer-bin move of a region is referred to the synthesis window's centre, so a partial keeps its phase
//    when its move changes by a bin;
//  - a partial's frequency is measured on the same bin from hop to hop while it stays within a bin of it;
//  - low partials (below 560 Hz in Tight, 280 Hz in Standard): the negative-frequency image that the short window end spills into
//    the lowest bins is fitted with the window's own transform and removed, and the partial's skirt below 0 Hz is
//    moved with it (otherwise a mirror tone at f - df, -14 dB on a 41 Hz note); their frequency comes from the same
//    fit, on this hop and the previous one;
//  - below 300 Hz peaks may be one bin apart, so the partials of a low B or drop tuning get regions of their own;
//  - a region moved below 0 Hz folds back, as a real signal does.
//
// Threads: prepare(), setMode(), setOptions() and reset() on the main thread, never while process() runs.
// setSemitones() from any thread (one relaxed atomic store); the audio thread picks the value up at its next hop and
// crossfades from the old shift to the new one (equal power, kFadeMs). process() allocates nothing, takes no lock and
// does bounded work per sample and per hop (at most Options::maxPeaks regions and maxLowPartials fits per hop).
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "cab/Fft.h"

namespace magmora {

class Transposer {
public:
  enum class Mode { Tight, Standard };  // about 5.3 ms and 10.7 ms of delay, at any rate

  static constexpr int kMinSemitones = -12;
  static constexpr int kMaxSemitones = 12;
  static constexpr double kFadeMs = 15.0;  // crossfade between two shifts (or the shift and the dry delay)

  // Developer knobs (experiments and tests); the defaults are the product. Take effect at the next prepare().
  struct Options {
    int analysisScale = 0;     // analysis 2048 * scale samples at 44.1/48 kHz, doubled per octave of rate; 0: the mode's
    int pad = 0;               // FFT size = analysis length * pad (zero padding: a finer grid for the moves); 0: the mode's
    int overlap = 0;           // synthesis length / hop; 0: the mode's
    double fallExponent = 0.5; // the analysis window falls as hann^e over the newest L/2, synthesis takes hann^(1-e)
    int riseShape = 0;         // analysis window's slow rise: 0 Hann, 1 Blackman, 2 sine
    int peakRadius = 2;        // a peak is the largest bin within +-radius bins (1 or 2)
    double bassRadiusHz = 300.0;  // below this, radius 1 (0: off)
    double peakFloorDb = -80.0;   // peaks below the frame's largest bin by more than this are not peaks
    double leakageBins = 0.0;     // drop a peak whose frequency is further than this from its bin as leakage (0: off)
    int maxLowPartials = 8;       // image cancellation for at most this many of the lowest partials per hop
    int maxPeaks = 128;           // regions per hop, per 2048-point FFT (bounded work; a guitar shows 30-100)
    bool centreCorrection = true; // phase of the integer-bin move referred to the synthesis window's centre
    bool imageCancel = true;      // low partials: remove the negative-frequency image, restore the skirt below 0 Hz
  };

  Transposer();
  ~Transposer();
  Transposer(const Transposer&) = delete;
  Transposer& operator=(const Transposer&) = delete;

  // Main thread. Allocates everything both modes need; maxBlock is not a limit (process() takes any length).
  void prepare(double sampleRate, int maxBlock);
  // Main thread, not while process() runs. Allocates nothing; resets the state.
  void setMode(Mode mode);
  Mode mode() const noexcept { return mode_; }
  void setOptions(const Options& options) { options_ = options; }
  const Options& options() const noexcept { return options_; }

  // Any thread. Clamped to [kMinSemitones, kMaxSemitones].
  void setSemitones(int semitones) noexcept;
  int targetSemitones() const noexcept { return target_.load(std::memory_order_relaxed); }

  // The delay for the current mode and rate, the same at every shift including 0 (and during crossfades).
  int latencySamples() const noexcept { return config_.L; }
  // Analysis length, FFT size and hop of the current mode (for reports and tests).
  int analysisSamples() const noexcept { return config_.N; }
  int fftSize() const noexcept { return config_.M; }
  int hopSamples() const noexcept { return config_.H; }
  // Audio thread (or after process() returned): true while the output is exactly the input delayed by
  // latencySamples(): 0 semitones and no crossfade under way. A host-side bypass may then skip this block entirely.
  bool isBypassable() const noexcept { return cur_ == kDry && !fading_ && pending_ < 0; }

  // Main thread, not while process() runs. Silence in the delay, no crossfade: the shift is the target at once.
  void reset();

  // Audio thread. Mono, any frames >= 0, in and out may be the same buffer. Non-finite input is taken as 0 and the
  // input is limited to +-1000 (and below 1e-20 taken as 0), so the output is finite whatever comes in. At 0 semitones
  // (and no crossfade) the output is the input delayed by latencySamples(), bit for bit, within those limits.
  void process(const float* in, float* out, int frames) noexcept;

private:
  static constexpr int kDry = -1;
  static constexpr int kVoices = 2;

  struct Config {
    int N = 0;  // analysis length
    int M = 0;  // FFT size (N * pad)
    int K = 0;  // bins below Nyquist: M / 2
    int L = 0;  // synthesis length = latency
    int H = 0;  // hop
  };
  struct Voice {
    int semitones = 0;
    double ratio = 1.0;
    int frames = 0;  // frames synthesised since it started; ready to be heard after `overlap`
    std::vector<float> theta, nextTheta;  // accumulated rotation per bin (radians, wrapped)
    std::vector<float> acc;               // overlap-add, L samples
    std::vector<float> ready;             // the hop's finished output, H samples
  };

  Config configFor(Mode mode) const;
  void configure();
  void runHop();
  void analyse();
  void synthesise(Voice& v);
  void startVoice(int slot, int semitones);
  void forwardReal(const float* x);
  void inverseReal(float* x);
  int skirtFor(const Config& c) const;
  void tableAt(double nu, float& re, float& im) const noexcept;
  struct TableRun;
  void cancelImages(int count);
  bool fitPartial(const float* re, const float* im, int k, double kappa, double& u, double& v) const noexcept;
  float readDry(uint64_t at) const noexcept { return ring_[static_cast<size_t>(at) & ringMask_]; }

  Options options_;
  Mode mode_ = Mode::Tight;
  double rate_ = 48000.0;
  Config config_;
  int fadeLength_ = 1;
  float peakRatio_ = 1e-8f;
  int maxPeaks_ = 128, bassBin_ = 0;

  std::atomic<int> target_{0};

  // ---- audio thread state
  std::vector<float> ring_;  // sanitised input; the dry path reads it L samples back
  size_t ringMask_ = 0;
  uint64_t written_ = 0;     // samples written into ring_
  int hopPos_ = 0;           // samples since the last hop
  int cur_ = kDry;           // what is heard: kDry or a voice slot
  int pending_ = -1;         // a voice warming up for the next shift, or -1
  bool fading_ = false;
  int fadeFrom_ = kDry, fadeTo_ = kDry, fadePos_ = 0;
  bool primed_ = false;      // prevRe_/prevIm_ hold the previous hop's spectrum
  Voice voices_[kVoices];

  // ---- per-hop scratch and tables (sized in prepare)
  cab::Fft ffts_[2];         // complex, M / 2 points, for Tight and Standard
  const cab::Fft* fft_ = nullptr;
  std::vector<float> analysisWindow_, synthesisWindow_;  // N and L samples (synthesis includes the OLA gain)
  std::vector<float> fadeIn_, fadeOut_;
  std::vector<float> frame_, zr_, zi_;
  std::vector<float> twiddleRe_, twiddleIm_;  // e^{-2 pi i k / M}, k < M / 2
  std::vector<float> re_, im_, prevRe_, prevIm_, mag_, prevMag_;
  std::vector<float> yr_, yi_;
  std::vector<int> peaks_, regionEnd_;
  std::vector<uint8_t> peakFlag_;
  std::vector<float> omega_;   // per peak: rad/sample
  int peakCount_ = 0;
  std::vector<int> estPeak_;              // per peak: the bin its frequency was measured on
  std::vector<int> estBin_, nextEstBin_;  // per bin: the bin whose phase advance measured its region's frequency
  std::vector<float> cre_, cim_;  // the spectrum the voices move: re_/im_ with the low partials' images removed
  // image cancellation: the analysis window's transform A(nu) for nu in [-V, V] bins, kTableStep points per bin
  static constexpr int kTableStep = 32;
  int skirtBins_ = 0;            // V
  std::vector<float> tableRe_, tableIm_;
  std::vector<float> kappaMemo_, nextKappaMemo_;  // per bin: the last hop's frequency of its region (bins)
  std::vector<float> fitKappa_, lowRe_, lowIm_, lowKappa_;  // per peak: complex amplitude and position (bins); < 0: none
};

}  // namespace magmora
