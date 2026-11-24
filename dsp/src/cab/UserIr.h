// The user response-file slot (XP10): a WAV of the user's, resampled to the host rate, optionally normalised,
// convolved with zero added latency (cab/Convolver.h). Loading, resampling and building happen on the main thread;
// a finished kernel is handed to the audio thread through one atomic pointer and comes back through another to be
// freed on the main thread, so the audio thread never allocates, frees or waits. A new response crossfades from
// the old one over 20 ms; switching the slot on or off crossfades with the dry signal; while off and faded out the
// samples are not touched.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "cab/Convolver.h"
#include "cab/Wav.h"
#include "engine/ParamSmoother.h"

namespace magmora::cab {

constexpr double kMaxResponseSeconds = 10.0;
constexpr double kSwapSeconds = 0.020;

// True when every sample is finite (a response with NaN or infinity in it would poison every output sample).
bool finiteSamples(const std::vector<float>& x);
// A double narrowed to float without overflow: values beyond float range are clamped, non-finite ones become 0.
float narrowSample(double v);

// Offline band-limited resampling (Kaiser-windowed sinc, 64 zero crossings, cut-off at 0.95 of the lower Nyquist).
std::vector<float> resample(const std::vector<float>& x, double from, double to);

class UserIr {
 public:
  UserIr() = default;
  ~UserIr();
  UserIr(const UserIr&) = delete;
  UserIr& operator=(const UserIr&) = delete;

  // --- main thread ---
  // Audio stopped. stereoInput: the two channels reaching the slot differ (dual-mono routing).
  void prepare(double hostRate, int maxBlockSize, bool stereoInput);
  bool load(const Audio& source, std::string* error = nullptr);
  bool loadFile(const char* path, std::string* error = nullptr);
  void clear();
  void collect();  // frees kernels the audio thread has let go of; call now and then (loads and prepare do)
  bool hasResponse() const { return haveSource_; }
  double responseSeconds() const;
  int responseChannels() const { return haveSource_ ? static_cast<int>(source_.channels.size()) : 0; }
  void setNormalize(bool on) { normalize_.store(on, std::memory_order_relaxed); }
  bool normalize() const { return normalize_.load(std::memory_order_relaxed); }
  // The response as it is convolved (resampled), for tests and tools.
  const std::vector<std::vector<float>>& kernelResponse() const { return built_; }
  int partitionSize() const;
  // A response handed over that the audio thread has not taken yet (the cabinet's worker waits for it).
  bool handoverPending() const { return hasPending(); }

  // --- audio thread ---
  void setControls(bool on, double levelDb);
  bool idle() const { return !on_ && wet_ == 0.0 && !old_ && !warming_ && !hasPending(); }
  void process(float* left, float* right, int frames);
  void process(double* left, double* right, int frames);
  void reset();

 private:
  struct Kernel {
    bool identity = true;
    float normGain = 1.0f;
    int lanes = 0;
    Convolver conv[2];
    bool primed = false;  // fed the slot's recent input on the building thread
    uint64_t fed = 0;     // input samples it has heard, as a count of the slot's input
  };
  void prime(Kernel* k);
  Kernel* build(const Audio& source);
  void install(Kernel* k);
  bool hasPending() const { return pending_.load(std::memory_order_acquire) != nullptr; }
  void runKernel(Kernel* k, int frames, std::vector<float>* out);
  template <class Sample>
  void run(Sample* left, Sample* right, int frames);

  double rate_ = 48000.0;
  int maxBlock_ = 0;
  bool stereoInput_ = false;
  Audio source_;
  bool haveSource_ = false;
  std::vector<std::vector<float>> built_;
  std::atomic<bool> normalize_{true};

  Kernel* current_ = nullptr;  // audio thread's
  Kernel* old_ = nullptr;      // audio thread's, while a swap crossfades
  std::atomic<Kernel*> pending_{nullptr};
  std::atomic<Kernel*> retired_{nullptr};
  double swap_ = 1.0, step_ = 0.0, normWeight_ = 1.0, normStep_ = 0.0;
  bool on_ = false;
  double wet_ = 0.0, gain_ = -1.0;  // gain_ < 0: recompute from the level
  engine::ParamSmoother level_;
  std::vector<float> in_[2], a_[2], b_[2];
  // The slot's recent input: a new response is fed it before it is handed over, and catches up on the audio
  // thread (a bounded number of samples per block) before its crossfade starts, so that it starts with its whole tail.
  std::vector<float> ring_[2];
  uint64_t ringMask_ = 0;
  std::atomic<uint64_t> written_{0}, resetAt_{0};
  Kernel* warming_ = nullptr;  // audio thread's: taken, catching up
  std::vector<float> catchIn_, catchOut_;
};

}  // namespace magmora::cab
