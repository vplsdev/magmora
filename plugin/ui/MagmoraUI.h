// The seam between the plug-in (WP3) and the UI (WP4).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <magmora/Chain.h>

namespace iplug {
namespace igraphics {
class IGraphics;
}
}  // namespace iplug

namespace magmora {
namespace ui {

// What the audio thread measured since the last call.
struct Levels {
  float inputPeak = 0.f;   // linear, full scale, after the input trim
  float outputPeak = 0.f;  // linear, full scale, as sent to the host
  float gateGain = 1.f;    // 0..1
  float powerW = 0.f;      // into the speaker load
};

// A cabinet slot's response file (a slot's source is a pack microphone or its own file).
struct CabIrInfo {
  bool loaded = false, normalize = true;
  std::string reference, message;  // message: why restoring it from the session failed, else empty
  double seconds = 0.;
  int channels = 0;
};

// A preset file of the user's.
struct UserPresetInfo {
  std::string name;    // as shown ("name" in the file)
  std::string folder;  // "" for the presets folder itself, else its subfolder
  std::string path;    // absolute, UTF-8
};

// What the UI needs from the plug-in besides its parameters. Parameters are reached through the editor delegate,
// and read on every frame rather than pushed, so the panel follows every way a host can set them.
// All calls come from the main thread.
class Host {
public:
  virtual Levels TakeLevels() = 0;

  virtual int PresetCount() const = 0;
  virtual const char* PresetName(int index) const = 0;
  virtual const char* PresetCategory(int index) const = 0;
  virtual int PresetIndex() const = 0;
  virtual bool PresetEdited() const = 0;
  // Sends one gesture per changed parameter, so the host records the change.
  virtual void ChoosePreset(int index) = 0;

  // Component value sets of the circuit. Not a host parameter: part of the instance's state.
  virtual int CircuitCount() const = 0;
  virtual const char* CircuitLabel(int index) const = 0;
  virtual int Circuit() const = 0;
  virtual void ChooseCircuit(int index) = 0;
  // Empty, or a quiet remark about the loaded session (saved with a different model of its circuit).
  virtual std::string SessionNote() const = 0;
  // The quality the engine runs at now: Ultra plays at High unless the host renders offline.
  virtual int QualityInUse() const = 0;

  virtual int LatencyInSamples() const = 0;

  // Cabinet: a virtual microphone on the user's own response files (Chain). Main thread.
  virtual CabinetStatus CabStatus() const = 0;
  virtual int CabMicCount() const = 0;
  virtual std::string CabMicName(int index) const = 0;
  virtual std::vector<std::string> CabSpeakers(int mic) const = 0;
  virtual std::vector<std::string> CabVariants(int mic, const std::string& speaker) const = 0;
  virtual std::string CabSource(int mic, bool variant) const = 0;
  virtual void SetCabSource(int mic, const std::string& speaker, const std::string& variant) = 0;
  virtual std::string LoadCabPack(const std::string& folder, const std::string& mapping) = 0;  // error, or empty
  virtual void ClearCabPack() = 0;
  virtual std::string CabMessage() const = 0;
  // The built-in cabinet (plugin/BuiltinCab.h): its name ("" when the build has none), whether it is the pack
  // in use, and loading it back (error, or empty).
  virtual std::string BuiltinCabName() const { return {}; }
  virtual bool CabIsBuiltin() const { return false; }
  virtual std::string LoadBuiltinCab() { return "no built-in cabinet"; }
  // Each slot's response file; slot 0 or 1. Loading one does not change the slot's source parameter.
  virtual CabIrInfo CabIr(int slot) const = 0;
  virtual std::string LoadCabIr(int slot, const std::string& path) = 0;  // error, or empty
  virtual void ClearCabIr(int slot) = 0;
  virtual void SetCabIrNormalize(int slot, bool on) = 0;
  // A parameter set as the editor sets it (one gesture, so that the host records it), without the editor itself:
  // what a late callback (a file dialog's) uses.
  virtual void SetParameterFromUI(int id, double plain) = 0;
  // True while the editor now open is; a callback that may outlive it (a file dialog) takes it when the dialog opens
  // and does nothing once it is false.
  virtual std::shared_ptr<const std::atomic<bool>> UiAlive() const = 0;
  virtual double HostSampleRate() const = 0;
  virtual bool SampleRateSupported() const = 0;
  virtual void EditorScaleChanged(float scale) = 0;

  // ---- user presets (plugin/UserPresets.h). Paths are absolute, UTF-8, and always ones UserPresets() listed. ----
  // Every call returns an error to show, or empty. `exists` (when given) is set instead of failing when the target
  // file is there already and `replace` is false.
  virtual std::vector<UserPresetInfo> UserPresets() { return {}; }  // rescans the folder
  virtual std::vector<std::string> UserPresetFolders() { return {}; }
  virtual std::string CurrentPresetName() const { return PresetName(PresetIndex()); }
  virtual std::string UserPresetPath() const { return {}; }  // the loaded user preset's file, or empty
  virtual bool UserPresetLoaded() const { return false; }    // a user preset (possibly deleted since) is current
  virtual std::string LoadUserPreset(const std::string&) { return "not available"; }
  virtual std::string SaveUserPreset(const std::string&, const std::string&, bool, bool*) { return "not available"; }
  virtual std::string SaveCurrentUserPreset() { return "not available"; }
  virtual std::string RenameUserPreset(const std::string&, const std::string&, bool, bool*) { return "not available"; }
  virtual std::string DeleteUserPreset(const std::string&) { return "not available"; }
  virtual std::string ImportUserPreset(const std::string&, std::string*) { return "not available"; }
  virtual std::string UserPresetFolder(bool create) { (void)create; return {}; }
  // Prev/next: factory presets in menu order, then user presets (root, then folders; by name), wrapping.
  virtual void StepPreset(int by) {
    const int n = PresetCount();
    if (n > 0) ChoosePreset(((PresetIndex() + by) % n + n) % n);
  }
  // Files the last preset load could not find ("room.wav"), shown at the top of the preset menu.
  virtual std::vector<std::string> MissingPresetFiles() const { return {}; }
  // The previous or next .wav in the folder of the slot's loaded file (wrapping); error, or empty.
  virtual std::string StepCabIr(int, int) { return "not available"; }

  // ---- tuner ----
  // While on, the audio thread feeds the input to the tuner; `mute` silences the output (with a short fade).
  virtual void SetTuner(bool on, bool mute) { (void)on; (void)mute; }
  // The newest n samples of the tuner's input and their rate; false if not that many yet.
  virtual bool TunerSamples(float* out, int n, double& rate) { (void)out; (void)n; rate = 0.; return false; }

  // ---- transposer (added 2026-11-24) ----
  // The delay the transposer adds to the latency now, in samples at the host rate (0 while it is out of the path).
  virtual int TransposeLatencyInSamples() const { return 0; }

  // ---- stand-alone ----
  virtual bool CanShowAudioSettings() const { return false; }
  virtual void ShowAudioSettings() {}

protected:
  ~Host() = default;
};

constexpr float kMinScale = 0.75f;
constexpr float kMaxScale = 1.5f;

void Layout(iplug::igraphics::IGraphics& graphics, Host& host);
// Ends any gesture still open. Call before the window goes away.
void Closing(iplug::igraphics::IGraphics& graphics);
void ShowAbout(iplug::igraphics::IGraphics& graphics);

}  // namespace ui
}  // namespace magmora
