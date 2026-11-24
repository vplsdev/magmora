// Magmora DSP - a chromatic tuner (added 2026-11-24).
//
// The audio thread hands its input to a TunerFeed, which low-passes and decimates it to about 11 kHz into a lock-free
// single-producer ring (no allocation, no lock, a few multiplies per input sample). A reader on another thread (the
// editor's timer) copies the newest window out of the ring and runs PitchDetector on it: YIN's cumulative mean
// normalised difference for the period, parabolic interpolation, then a refinement on a multiple of the period with
// the normalised square difference, which makes clean tones accurate to well under a cent from A0 (27.5 Hz) to E6.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace magmora {

class TunerFeed {
public:
  static constexpr int kCapacity = 8192;  // samples at the decimated rate (about 0.7 s)
  static constexpr double kTargetRate = 11025.0;

  TunerFeed();
  ~TunerFeed();
  TunerFeed(const TunerFeed&) = delete;
  TunerFeed& operator=(const TunerFeed&) = delete;

  // Not while push() runs (the plug-in calls it with the audio thread held out). Allocates nothing.
  void prepare(double hostRate);
  // Audio thread: wait-free. `gain` scales what is stored (the input trim, so that the level reads as metered).
  void push(const double* x, int n, double gain = 1.0) noexcept;

  // Reader (one thread): the rate of the stored samples, 0 before prepare.
  double rate() const noexcept { return rate_.load(std::memory_order_acquire); }
  // Copies the newest n samples (n <= kCapacity) into out. False if fewer have arrived since prepare, or if the
  // writer overwrote part of them while they were copied.
  bool latest(float* out, int n) const noexcept;
  uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }

private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) noexcept {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  std::unique_ptr<std::atomic<float>[]> ring_;
  std::atomic<uint64_t> written_{0};  // samples published
  std::atomic<uint64_t> claimed_{0};  // samples the writer may be writing (published or not)
  std::atomic<double> rate_{0.0};
  // Audio thread only (and prepare, with the audio thread out).
  Biquad lowpass_[2];
  int factor_ = 1, phase_ = 0;
};

struct Pitch {
  bool found = false;
  double hz = 0.0;
  double clarity = 0.0;  // 0..1: how periodic the window is
  double rms = 0.0;      // of the window, full-scale units
};

class PitchDetector {
public:
  explicit PitchDetector(double minHz = 25.0, double maxHz = 1500.0);
  // The window length detect() wants at this rate.
  int windowSamples(double rate) const;
  // Any one thread at a time. Allocates only when the window grows.
  Pitch detect(const float* x, int n, double rate);
  // Below this RMS (about -66 dBFS) there is no note.
  static constexpr double kSilence = 5e-4;

private:
  double minHz_, maxHz_;
  std::vector<double> x_, d_, cmnd_;
};

struct NoteReading {
  int midi = 69;       // 69 = A4
  double cents = 0.0;  // -50 .. +50 from that note
};
NoteReading nearestNote(double hz, double a4Hz = 440.0);
const char* noteName(int midi);  // "C", "C#", ... "B"
int noteOctave(int midi);        // scientific pitch notation: A4 = 69

}  // namespace magmora
