#pragma once

#include "IPlug_include_in_plug_hdr.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <magmora/Chain.h>
#include <magmora/Preset.h>
#include <magmora/Tuner.h>

#include "ui/MagmoraUI.h"

#ifdef VST3_API
#include "pluginterfaces/vst/ivstparameterchanges.h"

class MagmoraView;
#endif

// The plug-in around magmora::Chain: pedal, head, EQ, cabinet (virtual microphone on the built-in modelled pack or the
// user's files, BuiltinCab.h) and the user response file. Parameter index = chain parameter id (Params.h, then ChainParams.h).
//
// Threads. The audio thread never waits: it raises mInProcess and looks at mSuspended. Whoever has to rebuild
// the engine (new rate or block size, a structural parameter) fades it out, raises mSuspended, waits until the
// audio thread has left, and only then touches it. mConfigMutex is taken only outside the audio thread.
//
// iPlug2 at the pinned commit needs these work-arounds, all made here and none in the vendored copy:
//  - AUv2 does not call OnReset() from AudioUnitInitialize: OnActivate() does.
//  - Its bypass path leaves the right channel of a 1-2 layout silent, keeps stale delayed audio and does not
//    survive a VST3 state load. Bypass is this class's: latency-aligned, crossfaded, saved.
//  - AUv2 tells listeners about a latency before storing it: the value is stored first here.
//  - The VST3 editor view keeps a dangling pointer and resizes without a frame: MagmoraView.
class Magmora final : public iplug::Plugin, public magmora::ui::Host {
public:
  explicit Magmora(const iplug::InstanceInfo& info);
  ~Magmora();

  void OnReset() override;
  void OnActivate(bool active) override;
  void OnIdle() override;
  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;

  bool SerializeState(iplug::IByteChunk& chunk) const override;
  int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;

  // The panel polls its parameters, so pushing values into it is never needed. These pushes can also arrive on
  // a thread that must not walk the control list.
  void OnRestoreState() override {}
  void SendParameterValueFromDelegate(int, double, bool) override {}

  void OnUIOpen() override;
  void OnUIClose() override;
  void OnParentWindowResize(int width, int height) override;
  bool OnHostRequestingAboutBox() override;
  bool OnHostRequestingProductHelp() override { return OnHostRequestingAboutBox(); }

  // magmora::ui::Host
  magmora::ui::Levels TakeLevels() override;
  int PresetCount() const override;
  const char* PresetName(int index) const override;
  const char* PresetCategory(int index) const override;
  int PresetIndex() const override;
  bool PresetEdited() const override;
  void ChoosePreset(int index) override;
  int CircuitCount() const override;
  const char* CircuitLabel(int index) const override;
  int Circuit() const override { return mValueSet.load(); }
  void ChooseCircuit(int index) override;
  std::string SessionNote() const override;
  int QualityInUse() const override { return static_cast<int>(mApplied[magmora::kQuality].load()); }
  int LatencyInSamples() const override { return mReportedLatency.load(); }
  int TransposeLatencyInSamples() const override { return mTransposeLatency.load(); }
  magmora::CabinetStatus CabStatus() const override { return mChain.cabinetStatus(); }  // the cabinet locks itself
  int CabMicCount() const override { return mChain.cabinetMicCount(); }
  std::string CabMicName(int index) const override { return mChain.cabinetMicName(index); }
  std::vector<std::string> CabSpeakers(int mic) const override;
  std::vector<std::string> CabVariants(int mic, const std::string& speaker) const override;
  std::string CabSource(int mic, bool variant) const override;
  void SetCabSource(int mic, const std::string& speaker, const std::string& variant) override;
  std::string LoadCabPack(const std::string& folder, const std::string& mapping) override;
  void ClearCabPack() override;
  std::string BuiltinCabName() const override;
  bool CabIsBuiltin() const override;
  std::string LoadBuiltinCab() override;
  std::string CabMessage() const override;
  magmora::ui::CabIrInfo CabIr(int slot) const override;
  std::string LoadCabIr(int slot, const std::string& path) override;
  void ClearCabIr(int slot) override;
  void SetCabIrNormalize(int slot, bool on) override;
  void SetParameterFromUI(int id, double plain) override;
  std::shared_ptr<const std::atomic<bool>> UiAlive() const override { return mUiAlive; }
  double HostSampleRate() const override { return GetSampleRate(); }
  bool SampleRateSupported() const override { return !mRateRejected.load(); }
  void EditorScaleChanged(float scale) override { mScale.store(scale); }
  std::vector<magmora::ui::UserPresetInfo> UserPresets() override;
  std::vector<std::string> UserPresetFolders() override;
  std::string CurrentPresetName() const override;
  std::string UserPresetPath() const override;
  bool UserPresetLoaded() const override;
  std::string LoadUserPreset(const std::string& path) override;
  std::string SaveUserPreset(const std::string& name, const std::string& folder, bool replace, bool* exists) override;
  std::string SaveCurrentUserPreset() override;
  std::string RenameUserPreset(const std::string& path, const std::string& name, bool replace, bool* exists) override;
  std::string DeleteUserPreset(const std::string& path) override;
  std::string ImportUserPreset(const std::string& file, std::string* imported) override;
  std::string UserPresetFolder(bool create) override;
  void StepPreset(int by) override;
  std::vector<std::string> MissingPresetFiles() const override;
  std::string StepCabIr(int slot, int by) override;
  void SetTuner(bool on, bool mute) override;
  bool TunerSamples(float* out, int n, double& rate) override;
  bool CanShowAudioSettings() const override;
  void ShowAudioSettings() override;

  // A setting that changes the engine's structure was made; rebuild now unless this is the audio thread.
  void StructureMayHaveChanged();

#if defined AU_API
  void SetHostBypassFromAU(bool bypass) { mBypass.store(bypass); }
  bool HostBypass() const { return mBypass.load(); }
  void SetInBypassCall(bool in) { mInBypassCall = in; }
  void RenderingOfflineChanged() { mOffline.store(GetRenderingOffline()); }  // on the thread that set it
  // kAudioUnitProperty_ClassInfo: iPlug2 applies the factory preset the dictionary names before it reads the data,
  // so the data is checked first (a rejected state leaves the instance untouched), and during the restore that
  // preset step does nothing (the data that follows sets everything).
  bool AcceptsAUClassInfo(CFPropertyListRef list);
  void BeginAUStateRestore(CFPropertyListRef list);
  void EndAUStateRestore();
#endif

#if defined VST3_API
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
  Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue v) override;
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* stream) override;
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* stream) override;
  Steinberg::IPlugView* PLUGIN_API createView(const char* name) override;
  bool EditorResize(int viewWidth, int viewHeight) override;
#endif

private:
  static constexpr int kMaxChunk = 4096;
  static constexpr double kEngineTailSeconds = 0.25;  // the head and the EQ ringing out
  static constexpr int kDryLength = 1 << 14;  // longer than any latency the chain can report (head + transposer)

  void InitParams();
  void InitPresets();
  void Reconfigure();
  void FadeOutForReconfigure();
  bool StructureChanged() const;
  // The transposer's path the parameters ask for: -1 out (0 st), else the mode (TransposeMode). Its changes are
  // structural (they change the latency) but need neither a fade-out nor a rebuild: the chain crossfades.
  int WantedTransposePath() const;
  bool TransposeChanged() const { return WantedTransposePath() != mAppliedTransposePath.load(); }
  void ApplyTransposePath();  // mConfigMutex held
  void UpdateLatency(bool ready);  // mConfigMutex held
  double WantedQuality() const;
  bool OnAudioThread() const;
  bool StateIsValid(const iplug::IByteChunk& chunk, int startPos);
  void RefreshCabView();  // main thread, mChainMutex held
  void UpdateTail();
  bool WaitForCabinet();
  void PushParams();
  bool OnMainThread() const { return std::this_thread::get_id() == mMainThread; }
  void LoadReferences();
  // end: where the enclosing section ends (-1: the chunk's end); the head never reads past it.
  int ReadHead(const iplug::IByteChunk& chunk, int startPos, bool apply, int end = -1);
  void PullBlockParams();
  void ApplyPresetValues(int index, bool withGestures);
  // User presets (main thread unless said otherwise).
  magmora::UserPreset Snapshot();  // the current values and file references, as a user preset saves them
  std::string WriteUserPreset(const std::string& path, magmora::UserPreset preset);
  void ApplyUserValues(const magmora::UserPreset& preset);
  void ApplyCabFiles(const magmora::UserPreset& preset, const std::string& presetPath);  // mChainMutex held
  void SetUserPreset(const std::string& name, const std::string& path, const magmora::UserPreset* compare);
  void ClearUserPreset();
  void LoadUserComparison();  // reads the loaded user preset's file for the edit mark
  std::string LastIrFolder() const;
  void ProcessChunk(const iplug::sample* left, const iplug::sample* right, iplug::sample* outL, iplug::sample* outR,
                    int n, bool run);

  // mutable: SerializeState is const in iPlug2 and has to bring the chain's own copy of the block values up to date.
  mutable magmora::Chain mChain;
  const std::thread::id mMainThread = std::this_thread::get_id();

  // Every call into the chain other than its parameters and its audio (state, pack, response files and their
  // references, the chain's strings) and the plug-in's own cabinet strings below: serialised by this mutex, which is
  // never taken on the audio thread. A host may load a state on any thread while the editor polls the cabinet.
  mutable std::mutex mChainMutex;
  // References restored from a state on the audio thread (some hosts do), loaded by OnIdle instead.
  std::atomic<bool> mLoadReferences{false};
  std::string mCabMessage, mIrMessage[2];  // under mChainMutex: why restoring the pack or a slot's file failed
  // A session's pack that could not be loaded: its folder and mapping file, written back on save (the chain's own
  // references are cleared with the pack so that nothing of a previous pack plays). Under mChainMutex.
  std::string mMissingFolder, mMissingMapping;
  bool mPackMissing = false;
  // What the editor shows of the cabinet's references and messages: a copy made on the main thread (OnIdle, and after
  // the editor's own actions), so that the editor never waits for, or races with, a state load on another thread.
  struct CabView {
    bool builtin = false;
    std::string pack, mapping;  // the session's pack reference (also when it is missing)
    std::string message;
    magmora::ui::CabIrInfo ir[2];
  } mCabView;
  // Our own copy of the current factory preset's index: a state may be loaded on any thread. iPlug2's own index is
  // brought up to date on the main thread.
  std::atomic<int> mPresetIdx{0};
  // The user preset in use, if any: its name and file (relative to the presets folder), and what it holds, for the
  // edit mark. A state may be loaded on any thread, so under mUserMutex; never taken on the audio thread, and no file
  // is read or written while it is held.
  mutable std::mutex mUserMutex;
  struct UserState {
    bool active = false;
    std::string name, relative;
    bool compare = false;        // the file was read: `preset` holds it
    magmora::UserPreset preset;  // with the references as they resolved
    std::vector<std::string> missing;  // files the load could not find
  } mUser;
  std::atomic<bool> mUserCheck{false};  // a state named a user preset: read its file on the main thread
  std::string mLastIrFolder;            // main thread
  // Cleared first in the destructor: OnIdle and the editor's late callbacks do nothing once it is.
  std::atomic<bool> mDying{false};
  std::shared_ptr<std::atomic<bool>> mUiAlive;  // while an editor is open; its pending file dialogs check it
  std::atomic<int> mTailSamples{0};

  // Configuration: written under mConfigMutex, with the audio thread out.
  std::mutex mConfigMutex;
  std::atomic<double> mEngineRate{0.};
  std::atomic<int> mEngineBlock{0};
  std::atomic<bool> mEngineReady{false};
  std::atomic<bool> mRateRejected{false};
  std::atomic<double> mApplied[magmora::kNumChainParams]{};  // the structural ones the engine runs with
  std::atomic<int> mReportedLatency{0};
  std::atomic<int> mAppliedTransposePath{-1};  // what the chain was last given (Chain::applyTransposeStructure)
  std::atomic<int> mTransposeLatency{0};       // its share of mReportedLatency
  std::atomic<int> mValueSet{magmora::Engine::defaultValueSet()};  // wanted
  std::atomic<int> mAppliedValueSet{magmora::Engine::defaultValueSet()};
  mutable std::mutex mNoteMutex;
  std::string mSessionNote;
  std::atomic<bool> mNotifyLatency{false};
  // Whether the host renders offline: iPlug2's own flag is written on the audio thread (VST3) or a property call (AU)
  // and read here on other threads, so a copy is kept.
  std::atomic<bool> mOffline{false};

  // Hand-shake with the audio thread.
  std::atomic<bool> mInProcess{false};
  std::atomic<bool> mSuspended{false};
  std::atomic<bool> mFadeOut{false};
  std::atomic<bool> mFadedOut{false};
  std::atomic<bool> mResetPending{false};
  std::atomic<bool> mBypass{false};
  std::atomic<unsigned> mBlocks{0};

  // Audio thread only.
  double mSent[magmora::kNumChainParams];
  std::vector<double> mIn[2], mScratchR, mDry[2];
  int mDryPos = 0;
  int mDryLatency = -1, mDryOldLatency = 0;  // the dry path's delay; on a latency change it crossfades from the old
  double mDryShift = 1.;                     // 0 .. 1 along that crossfade
  int mWetHoldOff = 0;  // after bypass: the engine's latency passes before its output is faded in
  double mDryMix = 0.;
  double mWetGain = 1.;
  double mTrimDb = 1e9, mTrimGain = 1.;
  bool mResumeEngine = false;
#if defined VST3_API
  bool mNoInput = false;
#endif

  // Audio thread to UI.
  std::atomic<float> mInputPeak{0.f}, mOutputPeak{0.f};

  // Tuner: the editor switches it; the audio thread feeds the ring and fades the output out while muted.
  magmora::TunerFeed mTuner;
  std::atomic<bool> mTunerOn{false}, mTunerMute{false};
  std::vector<double> mTunerIn;  // audio thread
  double mMuteGain = 1.;         // audio thread

  // Editor.
  std::atomic<float> mScale{1.f};
  std::atomic<float> mPendingScale{0.f};
#if defined AU_API
  bool mInBypassCall = false;
  std::atomic<int> mSkipPresetOfAUState{-1};  // during a ClassInfo restore: the preset its dictionary names
#endif
#if defined VST3_API
  friend class MagmoraView;
  MagmoraView* mEditorView = nullptr;
#endif
};
