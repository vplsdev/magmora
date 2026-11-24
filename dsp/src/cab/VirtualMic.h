// The cabinet block: a virtual microphone on the user's own response files (or the built-in pack). A pack folder is
// indexed on the main thread (names parsed, files decoded). A worker thread builds the response for the current
// controls (morph or snap, below; two microphones mixed) and hands it to
// the audio thread through the user-response slot's lock-free hand-over, which crossfades in 20 ms
// (cab/UserIr.h). The audio thread only convolves: no allocation, no lock, no wait.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <magmora/Chain.h>

#include "cab/Morph.h"
#include "cab/Positions.h"
#include "cab/UserIr.h"

namespace magmora::cab {

// The cabinet's controls, in the order of the chain's ids kCabOn .. kMicOffset.
enum CabControl : int {
  kCtlOn = 0, kCtlFloor,
  kCtlMic1On, kCtlMic1Type, kCtlMic1Position, kCtlMic1Distance, kCtlMic1Angle, kCtlMic1Level, kCtlMic1Pan, kCtlMic1Polarity,
  kCtlMic2On, kCtlMic2Type, kCtlMic2Position, kCtlMic2Distance, kCtlMic2Angle, kCtlMic2Level, kCtlMic2Pan, kCtlMic2Polarity,
  kCtlAlign, kCtlOffset, kCtlCount
};

// Plain morph only between recorded neighbours at most half an inch apart (or adjacent steps of an ordered path).
constexpr double kMorphGapMm = 12.7;

enum class TrackKind { Millimetres, Path, Zones, Files };  // x across the cone; ordered steps; zone words; no position

struct PackFile {
  std::string path, name;
  Position pos;
  double rate = 0.0;
  std::vector<float> samples;  // the first channel, at the file's rate
};

// One microphone's choices as the worker resolves them: magmora::CabinetMicStatus (kind: TrackKind as int).
using MicResolved = CabinetMicStatus;

class Pack {
 public:
  // Main thread. folder is scanned recursively for .wav files; mappingPath empty: <folder>/magmora-positions.json if
  // present. False with a reason when nothing usable is found (the old pack, if any, stays in place).
  bool load(const std::string& folder, const std::string& mappingPath, std::string* error);
  // The same from responses held in memory (a built-in pack): names, rates and samples as a folder's files
  // would give them; mappingText is the mapping file's JSON (empty: none). path may be empty (the name is used).
  bool loadMemory(std::vector<PackFile> files, const std::string& mappingText, std::string* error);
  const std::vector<PackFile>& files() const { return files_; }
  const std::vector<std::string>& mics() const { return mics_; }  // "" listed as "unlabelled"
  morph::PackPhase phase() const { return phase_; }
  bool phaseDeclared() const { return phaseDeclared_; }
  double gain() const { return gain_; }  // 1 / rms energy of the pack's files: packs play at comparable loudness
  size_t bytes() const;
  int skipped() const { return skipped_; }
  std::vector<std::string> speakers(int mic) const;
  std::vector<std::string> variants(int mic, const std::string& speaker) const;
  std::vector<double> angles(int mic, const std::string& speaker, const std::string& variant) const;
  static const char* micLabel(const std::string& mic) { return mic.empty() ? "unlabelled" : mic.c_str(); }

  // Worker: the response for one microphone at the host rate (float64), and what it was built from.
  std::vector<double> response(int mic, const std::string& speaker, const std::string& variant, double position,
                               double distanceMm, double angleDeg, double hostRate, MicResolved& info) const;

 private:
  bool index(std::vector<PackFile> files, const Mapping* mapping, const std::string& folder, int skipped,
             std::string* error);
  std::vector<double> atRate(size_t file, double rate) const;  // other rates: resampled on first use, then cached
  std::vector<double> blend(size_t a, size_t b, double t, bool morph, double rate) const;

  std::vector<PackFile> files_;
  std::vector<std::string> mics_;
  morph::PackPhase phase_ = morph::PackPhase::Aligned;
  bool phaseDeclared_ = false;
  double gain_ = 1.0;
  int skipped_ = 0;
  mutable std::mutex cacheMutex_;
  mutable std::map<std::pair<size_t, double>, std::vector<float>> cache_;  // responses at another rate
};

class VirtualMic {
 public:
  VirtualMic() = default;
  ~VirtualMic();
  VirtualMic(const VirtualMic&) = delete;
  VirtualMic& operator=(const VirtualMic&) = delete;

  // --- main thread ---
  // controls: kCtlCount values the chain writes (any thread) and the worker reads.
  // sources: the two slots' source parameters (kMic1Source, kMic2Source), or null (both: the pack).
  void prepare(double hostRate, int maxBlockSize, bool stereoInput, const std::atomic<double>* controls,
               const std::atomic<double>* sources = nullptr);
  bool loadPack(const std::string& folder, const std::string& mappingPath, std::string* error);
  // A pack held in memory (Pack::loadMemory); reference is what the status shows as its folder.
  bool loadPackMemory(std::vector<PackFile> files, const std::string& mappingText, const std::string& reference,
                      std::string* error);
  void clearPack();
  bool packLoaded() const;
  // Which recordings each microphone uses: speaker and variant (preamp, take); "" picks the pack's largest set.
  void setSource(int mic, const std::string& speaker, const std::string& variant);
  std::string speaker(int mic) const;
  std::string variant(int mic) const;
  std::shared_ptr<const Pack> pack() const;
  // A slot's response file: used while that slot's source is 1. Mono or stereo, any rate, at most 10 s.
  bool setFile(int slot, const Audio& audio, const std::string& name, std::string* error = nullptr);
  void clearFile(int slot);
  bool fileLoaded(int slot) const;
  double fileSeconds(int slot) const;
  int fileChannels(int slot) const;
  void setFileNormalize(int slot, bool on);  // scaled to unit energy of its louder channel (XP10's switch)
  bool fileNormalize(int slot) const;
  void setFileRaw(int slot, bool raw);  // declared raw: its arrival time may be measured against the other slot
  bool fileRaw(int slot) const;
  CabinetStatus status() const;
  // Blocks until the response for the current controls is built and handed over (tools and tests).
  bool waitUntilBuilt(double timeoutSeconds = 5.0);
  // The length of the last built response (what the convolution adds to the tail), 0 without one.
  double responseSeconds() const;
  // The last built response, before the pack gain (tools: comparison with the prototype).
  void lastResponse(std::vector<double>& left, std::vector<double>& right) const;

  struct Settings {
    bool on[2] = {false, false};
    int source[2] = {0, 0};  // 0 the pack, 1 the slot's file
    int type[2] = {0, 0};
    double position[2] = {0, 0}, distanceMm[2] = {0, 0}, angleDeg[2] = {0, 0};
    morph::MicMix mix[2];
    bool align = true;
    double offsetMm = 0.0;
  };
  static Settings settingsFrom(const double* controls, const double* sources = nullptr);
  struct SlotFile {
    const std::vector<std::vector<double>>* channels = nullptr;  // at the build's rate; null or empty: none
    bool normalize = true, raw = false;
    std::string name;
  };
  // The response for these settings (any thread but the audio thread; allocates). pack may be null. Pack slots carry
  // the pack's gain unless packGain is false (tools: comparison with the prototype); file slots their normalisation.
  static morph::TwoMics build(const Pack* pack, const Settings& s, const std::string* speakers,
                              const std::string* variants, const SlotFile* files, double rate, CabinetStatus& status,
                              bool packGain = true);

  // --- audio thread ---
  void process(float* left, float* right, int frames);
  void process(double* left, double* right, int frames);
  void reset() { ir_.reset(); }
  bool idle() const { return ir_.idle(); }

 private:
  void start();
  void stop();
  void run();
  bool buildIfChanged(std::unique_lock<std::mutex>& lock);

  UserIr ir_;
  double rate_ = 0.0;
  int maxBlock_ = 0;
  const std::atomic<double>* controls_ = nullptr;
  const std::atomic<double>* sources_ = nullptr;
  std::shared_ptr<const Audio> file_[2];
  std::string fileName_[2];
  bool fileNormalize_[2] = {true, true}, fileRaw_[2] = {false, false};
  std::vector<std::vector<double>> fileAtRate_[2];  // worker's: file_ resampled to rate_
  const Audio* fileAtRateOf_[2] = {nullptr, nullptr};  // worker's (prepare resets it with the worker stopped)
  std::vector<double> currentKey() const;
  std::shared_ptr<const Pack> pack_;
  std::string speaker_[2], variant_[2];
  long generation_ = 0;  // bumped by pack and source changes

  mutable std::mutex mutex_;  // pack_, file_*, sources, status_, last*; not held while the worker builds
  std::condition_variable wake_;
  std::thread worker_;
  bool quit_ = false;
  std::vector<double> lastKey_;
  bool built_ = false;
  std::chrono::steady_clock::time_point handedOver_{};
  CabinetStatus status_;
  std::vector<double> lastLeft_, lastRight_;
};

}  // namespace magmora::cab
