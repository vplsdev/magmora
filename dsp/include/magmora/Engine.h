// Magmora DSP - public engine interface. A stable interface: it is only appended to.
// Framework independent: C++17, no dependencies, no allocation or locks on the audio thread.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "Params.h"

namespace magmora {

namespace engine {
// A block that runs inside the head's oversampled loop, after the amp and before the downsampler, on the speaker
// voltage of each amp core. For magmora::Chain (the EQ, XP3); hosts do not use it. Called on the audio thread once
// per host sample with the oversampling factor's worth of samples; `right` lane is null with one core.
class InternalStage {
public:
  virtual bool active() const = 0;  // false: not called, the head is exactly as without it
  virtual void process(double* const* lanes, int frames) = 0;
  virtual long fallbacks() const { return 0; }  // solver fallbacks so far, added to Meters::solverFallbacks

protected:
  ~InternalStage() = default;
};
}  // namespace engine

struct Calibration {
  // Voltage at the amp input jack for a digital full-scale (1.0) input sample, in volts peak, before kInputTrim.
  // 2.0 V corresponds to an instrument input that clips at about +5 dBu. Hosts with a known calibration may set it.
  double inputVoltsPerFS = 2.0;
  // Speaker-terminal voltage that maps to digital full scale at kOutputLevel = 0 dB.
  double outputVoltsAtFS = 100.0;
};

struct Meters {            // written by the audio thread once per block, read by the UI (relaxed atomics)
  std::atomic<float> inputPeak{0.f};      // linear, after trim, full-scale units
  std::atomic<float> outputPeak{0.f};     // linear, full-scale units
  std::atomic<float> platePowerW{0.f};    // short-term output power into the load, watts
  std::atomic<float> supplySag{0.f};      // 0 = idle plate voltage, 1 = 15 % below idle (drives the lamp)
  std::atomic<float> gateGain{1.f};       // 0..1
  std::atomic<uint32_t> solverFallbacks{0};  // samples on which a cluster hit the iteration cap (diagnostic)
};

class Engine {
public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // --- main thread, not real-time safe -------------------------------------------------------
  // Allocates everything. sampleRate 44100..192000, maxBlockSize >= 1. May be called again at any time
  // when audio is stopped. Returns false for an unsupported rate.
  bool prepare(double sampleRate, int maxBlockSize, int numChannels /* 1 or 2 */);
  void setCalibration(const Calibration&);
  // Latency in samples at the host rate for the current kQuality / kPhaseMode (valid after prepare()).
  int latencySamples() const;
  // Changing kQuality, kPhaseMode, kLoad or kChannelMode needs this (it re-prepares internally); the host
  // must be told about the new latency. Call with audio stopped or from the plugin's reset path.
  void applyStructuralParams();

  // --- component value sets (added 2026-11-23) --------------------------------------------------
  // A structural setting of this instance, like kQuality: it takes effect at the next prepare() or
  // applyStructuralParams(). Index 0 .. valueSetCount() - 1; defaultValueSet() is what a new instance uses.
  bool setValueSet(int index);  // false (and no change) for an index out of range
  int valueSet() const;
  static int valueSetCount();
  static int defaultValueSet();
  static const char* valueSetName(int index);       // a stable key for state files, e.g. "prototype", "as-drawn"
  static const char* valueSetModelHash(int index);  // the model hash of that set; null for an index out of range
  static int findValueSet(const char* name);        // -1 if there is no such set

  // --- quality (added 2026-11-23) -----------------------------------------------------------
  // False for qualities meant for offline rendering only (Ultra): they can exceed one core in real time.
  static bool qualityIsRealtime(Quality);
  // The internal sample rate in use, and its oversampling factor (valid after prepare()).
  double internalSampleRate() const;
  int oversamplingFactor() const;

  // --- chain hook (added 2026-11-23, XP3) ------------------------------------------------------
  // Not for hosts: magmora::Chain attaches its internal-rate blocks here. Set with audio stopped; null detaches.
  void setInternalStage(engine::InternalStage*);
  // The same ahead of the amp's first cluster, on the volts at the input jack (the pedal, XP2; added 2026-11-23).
  void setPreStage(engine::InternalStage*);
  int ampCores() const;  // 2 with dual-mono routing of a stereo input, else 1 (valid after prepare())
  // The head itself on or off (added 2026-11-23). Off, the amp's circuit is crossfaded out in 12 ms (raised
  // cosine, as the pedal's bypass) and then not computed at all; the pre-stage and the internal stage still run in
  // the same oversampled loop, so the latency does not change. The volts at the input jack then reach the output at
  // unity: the input trim, the gate and the output level still apply; standby and power, being the head's, do not.
  // Switched back on, the amp starts from the operating point of its settings and fades in. Any thread, wait-free.
  // Default on: a host that never calls it gets the head as it always was, bit for bit.
  void setAmpOn(bool on);
  bool ampOn() const;

  // --- any thread, wait-free -------------------------------------------------------------------
  // value is in the units of kParams[id] (not normalised). Out-of-range values are clamped.
  void setParam(ParamId id, double value);
  double getParam(ParamId id) const;

  // --- audio thread, real-time safe ------------------------------------------------------------
  // Clears all state to the DC operating point of the current settings (no thump).
  void reset();
  // in/out: numChannels pointers to numFrames samples, may alias. numFrames <= maxBlockSize, may be 0.
  // Mono amp: with 2 channels kChannelMode decides the feed; MonoLeft/MonoSum write the same signal to both.
  void process(const float* const* in, float* const* out, int numFrames);
  void process(const double* const* in, double* const* out, int numFrames);

  const Meters& meters() const;

  // --- state -----------------------------------------------------------------------------------
  static constexpr int kStateVersion = 1;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Library version and the hash of the netlist/model data compiled in (shown in the About box and
// written into reference files so test data cannot silently go stale). modelHash() is the default value set's;
// Engine::valueSetModelHash() gives each set's.
const char* versionString();
const char* modelHash();

}  // namespace magmora
