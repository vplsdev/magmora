#include "Magmora.h"
#include "BuiltinCab.h"
#include "UserPresets.h"

#if defined AU_API
// iPlug2's AUv2 has no hook for bypass, and indexes its parameters with whatever ID a host passes. The factory
// named in config.h (and in the Info.plist) is therefore ours: it takes iPlug2's component and wraps the
// methods concerned. iPlug2's own factory is emitted below under another name.
namespace au_hooks {

using Lookup = AudioComponentMethod (*)(SInt16);
// iPlug2's method table: one function for every instance, stored by the first and only read after that.
static std::atomic<Lookup> gLookup{nullptr};

static AudioComponentMethod Original(SInt16 selector) { return gLookup.load(std::memory_order_acquire)(selector); }

static Magmora* Plug(void* self) {
  auto* instance = static_cast<iplug::AudioComponentPlugInInstance*>(self);
  return static_cast<Magmora*>(reinterpret_cast<iplug::IPlugAU*>(&instance->mInstanceStorage));
}

static OSStatus SetProperty(void* self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                            const void* data, UInt32 size) {
  const auto original = reinterpret_cast<AudioUnitSetPropertyProc>(Original(kAudioUnitSetPropertySelect));
  if (id == kAudioUnitProperty_ClassInfo && scope == kAudioUnitScope_Global) {
    // The state is checked before iPlug2 applies the factory preset it names: a rejected state changes nothing.
    if (!data || size < sizeof(CFPropertyListRef)) return kAudioUnitErr_InvalidPropertyValue;
    Magmora* plug = Plug(self);
    const CFPropertyListRef list = *static_cast<const CFPropertyListRef*>(data);
    if (!plug->AcceptsAUClassInfo(list)) return kAudioUnitErr_InvalidPropertyValue;
    plug->BeginAUStateRestore(list);
    const OSStatus status = original(self, id, scope, element, data, size);
    plug->EndAUStateRestore();
    return status;
  }
  if (id == kAudioUnitProperty_OfflineRender) {
    const OSStatus status = original(self, id, scope, element, data, size);
    Plug(self)->RenderingOfflineChanged();
    Plug(self)->StructureMayHaveChanged();  // Ultra runs only offline
    return status;
  }
  if (id != kAudioUnitProperty_BypassEffect || scope != kAudioUnitScope_Global)
    return original(self, id, scope, element, data, size);
  if (!data || size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
  Magmora* plug = Plug(self);
  plug->SetHostBypassFromAU(*static_cast<const UInt32*>(data) != 0);
  // iPlug2 still informs the listeners, but is never told of the bypass, so never takes its pass-through path.
  const UInt32 off = 0;
  plug->SetInBypassCall(true);
  const OSStatus status = original(self, id, scope, element, &off, sizeof off);
  plug->SetInBypassCall(false);
  return status;
}

static OSStatus GetPropertyInfo(void* self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                                UInt32* size, Boolean* writable) {
  const auto original = reinterpret_cast<AudioUnitGetPropertyInfoProc>(Original(kAudioUnitGetPropertyInfoSelect));
  const OSStatus status = original(self, id, scope, element, size, writable);
  if (status == noErr && id == kAudioUnitProperty_ParameterList && scope == kAudioUnitScope_Global && size) {
    int shown = 0;
    for (int i = 0; i < magmora::kNumChainParams; ++i) shown += magmora::Chain::parameterHidden(i) ? 0 : 1;
    *size = static_cast<UInt32>(shown * sizeof(AudioUnitParameterID));
  }
  return status;
}

static OSStatus GetProperty(void* self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                            void* data, UInt32* size) {
  // Parameters kept only for compatibility (Chain::parameterHidden) are not listed, so hosts do not show them.
  if (id == kAudioUnitProperty_ParameterList && scope == kAudioUnitScope_Global && data && size) {
    UInt32 used = 0;
    auto* ids = static_cast<AudioUnitParameterID*>(data);
    for (int i = 0; i < magmora::kNumChainParams; ++i) {
      if (magmora::Chain::parameterHidden(i)) continue;
      if ((used + 1) * sizeof(AudioUnitParameterID) > *size) return kAudioUnitErr_InvalidPropertyValue;
      ids[used++] = static_cast<AudioUnitParameterID>(i);
    }
    *size = used * sizeof(AudioUnitParameterID);
    return noErr;
  }
  if (id == kAudioUnitProperty_BypassEffect && scope == kAudioUnitScope_Global && data && size) {
    if (*size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
    *static_cast<UInt32*>(data) = Plug(self)->HostBypass() ? 1 : 0;
    *size = sizeof(UInt32);
    return noErr;
  }
  const auto original = reinterpret_cast<AudioUnitGetPropertyProc>(Original(kAudioUnitGetPropertySelect));
  return original(self, id, scope, element, data, size);
}

static OSStatus SetParameter(void* self, AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement element,
                             AudioUnitParameterValue value, UInt32 offset) {
  if (id >= magmora::kNumChainParams) return kAudioUnitErr_InvalidParameter;
  const auto original = reinterpret_cast<AudioUnitSetParameterProc>(Original(kAudioUnitSetParameterSelect));
  return original(self, id, scope, element, value, offset);
}

static OSStatus GetParameter(void* self, AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement element,
                             AudioUnitParameterValue* value) {
  if (id >= magmora::kNumChainParams) return kAudioUnitErr_InvalidParameter;
  const auto original = reinterpret_cast<AudioUnitGetParameterProc>(Original(kAudioUnitGetParameterSelect));
  return original(self, id, scope, element, value);
}

static AudioComponentMethod Methods(SInt16 selector) {
  switch (selector) {
    case kAudioUnitSetPropertySelect: return reinterpret_cast<AudioComponentMethod>(&SetProperty);
    case kAudioUnitGetPropertySelect: return reinterpret_cast<AudioComponentMethod>(&GetProperty);
    case kAudioUnitGetPropertyInfoSelect: return reinterpret_cast<AudioComponentMethod>(&GetPropertyInfo);
    case kAudioUnitSetParameterSelect: return reinterpret_cast<AudioComponentMethod>(&SetParameter);
    case kAudioUnitGetParameterSelect: return reinterpret_cast<AudioComponentMethod>(&GetParameter);
    default: return Original(selector);
  }
}

}  // namespace au_hooks

extern "C" {
__attribute__((visibility("default"))) void* Magmora_IPlugFactory(const AudioComponentDescription* desc);

__attribute__((visibility("default"))) void* AUV2_FACTORY(const AudioComponentDescription* desc) {
  auto* instance = static_cast<iplug::AudioComponentPlugInInstance*>(Magmora_IPlugFactory(desc));
  if (instance) {
    au_hooks::Lookup expected = nullptr;  // the same for every instance: kept from the first
    au_hooks::gLookup.compare_exchange_strong(expected, instance->mPlugInInterface.Lookup, std::memory_order_acq_rel);
    instance->mPlugInInterface.Lookup = &au_hooks::Methods;
  }
  return instance;
}
}

#undef AUV2_FACTORY
#define AUV2_FACTORY Magmora_IPlugFactory
#endif  // AU_API

#include "IPlug_include_in_plug_src.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>

#if defined APP_API && defined OS_MAC
#include <cstdlib>
#include <sys/stat.h>

#include "IPlugSWELL.h"
#include "resource.h"
extern HWND gHWND;  // iPlug2's stand-alone window (IPlugAPP_main.cpp)
#endif
#if defined OS_MAC
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace {

constexpr int kAll = magmora::kNumChainParams;

struct PresetDef {
  const char* name;
  const char* category;
  double values[kAll];
  bool given[kAll];  // the parameters this preset sets: the head's preset parameters, and the blocks it has objects for
};
#include "generated/Presets.inc"
static_assert(kNumGeneratedParams == kAll && kNumGeneratedHeadParams == magmora::kNumParams,
              "run plugin/scripts/generate.py");

const magmora::ParamInfo& Info(int id) { return *magmora::Chain::parameterInfo(id); }

constexpr int kNumPresets = static_cast<int>(std::size(kPresets));

constexpr int kStructural[] = {magmora::kLoad, magmora::kQuality, magmora::kPhaseMode, magmora::kChannelMode};

bool IsStructural(int id) { return std::find(std::begin(kStructural), std::end(kStructural), id) != std::end(kStructural); }

bool InPresets(int id) {
  return std::find(std::begin(kSessionParams), std::end(kSessionParams), id) == std::end(kSessionParams);
}

int ScaledPoints(int points, float scale) { return static_cast<int>(static_cast<float>(points) * scale); }

// ---- the state chunk --------------------------------------------------------------------------------------------
//
//   u32 magic, i32 version, i32 size of the body, body:
//   i32 flags, i32 count, count * (string key, f64 value), string preset name, f32 editor scale,
//   from version 2: string value set name, string that set's model hash
//   string: i32 length, bytes. Everything little-endian.
//
// Version 1 had no value set. Such a state loads the set that was the default then: "prototype".
//
// Parameters go by key, so later versions may append to them: unknown keys are skipped, missing ones take their
// defaults. Later versions may also append to the body: readers skip what they do not know.

constexpr uint32_t kMagic = 0x41524353;  // "SCRA"
// The plug-in's state since the chain (XP7) is Chain's container ("SCCH", Chain::getState), whose "head" section is
// the chunk below, unchanged. A factory preset is "SCPI" and its index: presets are compiled in (generate.py).
constexpr uint32_t kChainMagic = 0x48434353;   // "SCCH"
constexpr uint32_t kPresetMagic = 0x49504353;  // "SCPI"
constexpr int32_t kStateVersion = 2;
constexpr const char* kVersion1ValueSet = "prototype";
constexpr int32_t kPresetChunk = 1;      // flags: holds a preset's parameters, the session's own are left alone
constexpr int32_t kMaxString = 128;
constexpr int32_t kMaxEntries = 4096;
constexpr int32_t kMaxBody = 1 << 20;
constexpr const char* kUserSection = "upre";  // the user preset in use: name, file relative to the presets folder
constexpr int32_t kMaxUserPath = 4096;

void PutString(iplug::IByteChunk& chunk, const char* text) {
  const int32_t length = static_cast<int32_t>(std::strlen(text));
  chunk.Put(&length);
  if (length) chunk.PutBytes(text, length);
}

void WriteState(iplug::IByteChunk& out, int32_t flags, const double* values, const char* preset, float scale,
                int valueSet) {
  iplug::IByteChunk body;
  body.Put(&flags);
  int32_t count = 0;
  for (int i = 0; i < magmora::kNumParams; ++i) count += (flags & kPresetChunk) && !InPresets(i) ? 0 : 1;
  body.Put(&count);
  for (int i = 0; i < magmora::kNumParams; ++i) {
    if ((flags & kPresetChunk) && !InPresets(i)) continue;
    PutString(body, magmora::kParams[i].key);
    body.Put(&values[i]);
  }
  PutString(body, preset);
  body.Put(&scale);
  PutString(body, magmora::Engine::valueSetName(valueSet));
  PutString(body, magmora::Engine::valueSetModelHash(valueSet));

  const uint32_t magic = kMagic;
  const int32_t version = kStateVersion, size = body.Size();
  out.Put(&magic);
  out.Put(&version);
  out.Put(&size);
  out.PutBytes(body.GetData(), size);
}

class Reader {
public:
  // Reads from `from` up to `end` (-1, or past the chunk: the chunk's end); Limit narrows it further.
  Reader(const iplug::IByteChunk& chunk, int from, int end = -1)
  : mChunk(chunk), mPos(from), mEnd(end < 0 || end > chunk.Size() ? chunk.Size() : end) {}

  template <class T>
  bool Get(T& value) {
    return Bytes(&value, static_cast<int32_t>(sizeof(T)));
  }

  bool String(std::string& text) {
    int32_t length = 0;
    if (!Get(length) || length < 0 || length > kMaxString) return false;
    text.assign(static_cast<size_t>(length), '\0');
    return !length || Bytes(&text[0], length);
  }

  bool Limit(int32_t bytes) {
    if (bytes < 0 || bytes > mEnd - mPos) return false;
    mEnd = mPos + bytes;
    return true;
  }

  int End() const { return mEnd; }

private:
  bool Bytes(void* to, int32_t n) {
    if (mPos < 0 || n < 0 || n > mEnd - mPos) return false;
    if (mChunk.GetBytes(to, n, mPos) < 0) return false;
    mPos += n;
    return true;
  }

  const iplug::IByteChunk& mChunk;
  int mPos, mEnd;
};

// Set on a thread the first time it runs ProcessBlock (of any instance), and raised while it is inside one. Hosts
// render on pools of real-time threads, so "the audio thread" is any thread that has rendered, not the last one.
thread_local bool tRendered = false;
thread_local int tInProcess = 0;

// A thread the system schedules in real time (macOS: the time-constraint policy, as audio I/O and workgroup threads
// have), whether or not it has rendered yet.
bool RealtimeThread() {
#if defined OS_MAC
  thread_time_constraint_policy_data_t policy{};
  mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
  boolean_t defaults = false;
  const kern_return_t result = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                                 reinterpret_cast<thread_policy_t>(&policy), &count, &defaults);
  return result == KERN_SUCCESS && !defaults;
#else
  return false;
#endif
}

// Keeps the peak until the UI takes it. One writer; losing a race with the reader costs one block's peak.
void KeepPeak(std::atomic<float>& meter, double value) {
  const float v = static_cast<float>(value);
  if (v > meter.load(std::memory_order_relaxed)) meter.store(v, std::memory_order_relaxed);
}

#if defined APP_API && defined OS_MAC
// iPlug2's stand-alone host starts at 44.1 kHz and 512 samples, and offers no hook to change that. It reads this
// file straight after the plug-in is made, so a first launch finds our defaults there. An existing file is left.
void SeedStandaloneSettings() {
  const char* home = std::getenv("HOME");
  if (!home) return;
  const std::string folder = std::string(home) + "/Library/Application Support/" BUNDLE_NAME;
  const std::string file = folder + "/settings.ini";
  struct stat info;
  if (stat(file.c_str(), &info) == 0) return;
  if (stat(folder.c_str(), &info) != 0 && mkdir(folder.c_str(), 0755) != 0) return;
  if (std::FILE* f = std::fopen(file.c_str(), "wx")) {
    std::fputs("[audio]\nsr=48000\nbuffer=128\nin1=1\nin2=1\nout1=1\nout2=2\n", f);
    std::fclose(f);
  }
}
#endif

}  // namespace

// ---- construction ------------------------------------------------------------------------------------------------

Magmora::Magmora(const iplug::InstanceInfo& info)
: iplug::Plugin(info, iplug::MakeConfig(kAll, kNumPresets)) {
  std::fill(std::begin(mSent), std::end(mSent), std::nan(""));
  for (auto& channel : mIn) channel.assign(kMaxChunk, 0.);
  for (auto& channel : mDry) channel.assign(kDryLength, 0.);
  mScratchR.assign(kMaxChunk, 0.);
  mTunerIn.assign(kMaxChunk, 0.);
  for (int i = 0; i < kAll; ++i) mApplied[i].store(Info(i).def);

  InitParams();
  InitPresets();
  // A new instance plays the built-in cabinet; a session's own pack, or none, replaces it in LoadReferences.
  if (builtin_cab::kEnabled) builtin_cab::Load(mChain, nullptr);
#if defined APP_API && defined OS_MAC
  SeedStandaloneSettings();
#endif

  mMakeGraphicsFunc = [this]() {
    return iplug::igraphics::MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS);
  };
  mLayoutFunc = [this](iplug::igraphics::IGraphics* graphics) { magmora::ui::Layout(*graphics, *this); };
}

Magmora::~Magmora() {
  mDying.store(true);  // first: an OnIdle or a late editor callback from here on does nothing
  if (mUiAlive) mUiAlive->store(false);
  // An AU host may dispose of the instance with the view still up. Take it down while the object is whole.
  if (GetUI()) CloseWindow();
}

void Magmora::InitParams() {
  using iplug::IParam;
  // The iPlug parameter index is the chain parameter id. The engine owns every taper.
  for (int id = 0; id < kAll; ++id) {
    const magmora::ParamInfo& p = Info(id);
    IParam& param = *GetParam(id);
    const int flags = p.automatable ? 0 : IParam::kFlagCannotAutomate;
    const double initial = builtin_cab::Default(id, p.def);  // the chain's default, but the cabinet on
    const int def = static_cast<int>(initial);
    switch (id) {
      case magmora::kInputJack: param.InitEnum(p.name, def, {"High", "Normal"}, flags); continue;
      case magmora::kChannel: param.InitEnum(p.name, def, {"Rhythm", "Lead"}, flags); continue;
      case magmora::kStandby: param.InitBool(p.name, def != 0, "", flags, "", "Standby", "Operate"); continue;
      case magmora::kLoad: param.InitEnum(p.name, def, {"Reactive 4x12", "Resistive"}, flags); continue;
      case magmora::kQuality: param.InitEnum(p.name, def, {"Standard", "High", "Ultra"}, flags); continue;
      case magmora::kPhaseMode: param.InitEnum(p.name, def, {"Linear", "Minimum"}, flags); continue;
      case magmora::kChannelMode: param.InitEnum(p.name, def, {"Mono left", "Mono sum", "Dual mono"}, flags); continue;
      case magmora::kTransposeMode: param.InitEnum(p.name, def, {"Tight", "Standard"}, flags); continue;
      case magmora::kEqPosition: param.InitEnum(p.name, def, {"Before cabinet", "After cabinet"}, flags); continue;
      case magmora::kEqHighPassSlope:
      case magmora::kEqLowPassSlope: param.InitEnum(p.name, def, {"Off", "12 dB/oct", "24 dB/oct"}, flags); continue;
      case magmora::kEqBand1Type:
      case magmora::kEqBand4Type: param.InitEnum(p.name, def, {"Bell", "Shelf"}, flags); continue;
      case magmora::kMic1Polarity:
      case magmora::kMic2Polarity: param.InitBool(p.name, def != 0, "", flags, "", "Normal", "Inverted"); continue;
      case magmora::kMic1Source:
      case magmora::kMic2Source: param.InitEnum(p.name, def, {"Pack microphone", "Response file"}, flags); continue;
      default: break;
    }
    if (p.taper == magmora::Taper::Stepped) {
      if (p.max - p.min == 1.)
        param.InitBool(p.name, def != 0, "", flags, "", "Off", "On");
      else
        param.InitInt(p.name, def, static_cast<int>(p.min), static_cast<int>(p.max), p.unit, flags);
      continue;
    }
    const bool gate = id == magmora::kGateThreshold;
    const bool hertz = std::strcmp(p.unit, "Hz") == 0;
    const double bottom = p.min;
    const int decimals = hertz ? 0 : (p.step < 0.01 ? 3 : (p.step < 0.1 ? 2 : 1));
    // Hosts parse this text back with atof, so the number always comes first ("off" follows it).
    const auto display = [gate, bottom, decimals](double value, WDL_String& text) {
      if (gate && value <= bottom + 0.05)
        text.SetFormatted(32, "%.1f (off)", bottom);
      else
        text.SetFormatted(32, "%.*f", decimals, std::fabs(value) < 0.5 * std::pow(10., -decimals) ? 0. : value);
    };
    const auto unit = p.taper == magmora::Taper::Decibel ? IParam::kUnitDB
                      : hertz                           ? IParam::kUnitFrequency
                                                        : IParam::kUnitCustom;
    if (p.taper == magmora::Taper::Logarithmic)
      param.InitDouble(p.name, initial, p.min, p.max, p.step, p.unit, flags, "", IParam::ShapeExp(), unit, display);
    else
      param.InitDouble(p.name, initial, p.min, p.max, p.step, p.unit, flags, "", IParam::ShapeLinear(), unit, display);
  }
}

void Magmora::InitPresets() {
  for (int i = 0; i < kNumPresets; ++i) {
    iplug::IByteChunk chunk;
    const uint32_t magic = kPresetMagic;
    const int32_t index = i;
    chunk.Put(&magic);
    chunk.Put(&index);
    MakePresetFromChunk(kPresets[i].name, chunk);
  }
}

// ---- configuration -----------------------------------------------------------------------------------------------

// Ultra is for offline rendering (Engine::qualityIsRealtime): in real time the engine runs it at High.
double Magmora::WantedQuality() const {
  const auto q = static_cast<magmora::Quality>(static_cast<int>(GetParam(magmora::kQuality)->Value()));
  if (!magmora::Engine::qualityIsRealtime(q) && !mOffline.load()) return static_cast<double>(magmora::Quality::High);
  return static_cast<double>(q);
}

int Magmora::WantedTransposePath() const {
  const double semitones = GetParam(magmora::kTranspose)->Value();
  if (std::lround(semitones) == 0) return -1;
  return std::min(std::max(static_cast<int>(std::lround(GetParam(magmora::kTransposeMode)->Value())), 0), 1);
}

bool Magmora::StructureChanged() const {
  for (int id : kStructural) {
    const double wanted = id == magmora::kQuality ? WantedQuality() : GetParam(id)->Value();
    if (wanted != mApplied[id].load()) return true;
  }
  return mValueSet.load() != mAppliedValueSet.load();
}

// Reconfigure() takes a mutex, sleeps and allocates, so it must never run on a real-time thread, and it waits for
// the audio thread to leave ProcessBlock, so inside ProcessBlock it would also deadlock. Every caller goes through
// this test (OnReset, OnActivate, UnserializeState, the AU offline switch) or runs on the main thread (OnIdle).
// An audio thread is one inside ProcessBlock, any other thread that has rendered (hosts use pools of them), or one the
// system schedules in real time. The main thread counts only while inside ProcessBlock: a host that renders offline
// on its main thread may block it between renders, and must, for a state it loads to apply before it renders.
bool Magmora::OnAudioThread() const {
  return tInProcess > 0 || (tRendered && !OnMainThread()) || RealtimeThread();
}

void Magmora::StructureMayHaveChanged() {
  if (!OnAudioThread() && mEngineReady.load() && (StructureChanged() || TransposeChanged())) Reconfigure();
}

// The transposer in or out, or its mode: the chain stores the new path and its latency (wait-free) and crossfades to
// it on the audio thread, so audio keeps running. mConfigMutex held.
void Magmora::ApplyTransposePath() {
  const double semitones = GetParam(magmora::kTranspose)->Value(), mode = GetParam(magmora::kTransposeMode)->Value();
  mChain.setParameter(magmora::kTranspose, semitones);
  mChain.setParameter(magmora::kTransposeMode, mode);
  mChain.applyTransposeStructure();
  const int path = std::lround(semitones) == 0 ? -1 : std::min(std::max(static_cast<int>(std::lround(mode)), 0), 1);
  mAppliedTransposePath.store(path);
}

// Asked again after every prepare and structural change: it depends on rate, quality, phase and the transposer's path.
// The other blocks around the head add none (Chain::latencySamples, gate G-X2). mConfigMutex held.
void Magmora::UpdateLatency(bool ready) {
  const int latency = ready ? std::min(std::max(mChain.latencySamples(), 0), kDryLength - 1) : 0;
  mTransposeLatency.store(ready ? mChain.transposeLatencySamples() : 0);
  if (latency != mReportedLatency.exchange(latency)) mNotifyLatency.store(true);
  // Stored at once, for a host that asks straight away. The host is told on the main thread, from OnIdle.
  IPlugProcessor::SetLatency(latency);
}

void Magmora::FadeOutForReconfigure() {
  mFadedOut.store(false);
  mFadeOut.store(true);
  const unsigned before = mBlocks.load();
  for (int ms = 0; ms < 60 && !mFadedOut.load(); ++ms) {
    if (ms >= 25 && mBlocks.load() == before) break;  // no audio is running: nothing to fade
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void Magmora::Reconfigure() {
  std::lock_guard<std::mutex> lock(mConfigMutex);
  const double rate = GetSampleRate();
  const int block = std::min(std::max(GetBlockSize(), 1), kMaxChunk);
  const bool format = rate != mEngineRate.load() || block != mEngineBlock.load();
  const bool structure = StructureChanged();
  if (!format && !structure) {
    if (!TransposeChanged()) return;
    // Only the transposer goes in or out of the path, or changes mode: no fade-out and no rebuild. The chain lets the
    // new path's delay fill and crossfades to it; the host is told the new latency as for any other change.
    ApplyTransposePath();
    UpdateLatency(mEngineReady.load());
    return;
  }

  if (!format && mEngineReady.load()) FadeOutForReconfigure();
  mSuspended.store(true);
  while (mInProcess.load()) std::this_thread::sleep_for(std::chrono::microseconds(100));

  for (int i = 0; i < kAll; ++i) {
    const double value = i == magmora::kQuality ? WantedQuality() : GetParam(i)->Value();
    mChain.setParameter(i, value);
    mSent[i] = value;
    if (IsStructural(i)) mApplied[i].store(value);
  }
  ApplyTransposePath();  // its structure is applied by prepare() or applyStructuralParams() below
  // The value set takes effect in prepare() or applyStructuralParams(), so it is set first.
  const int set = mValueSet.load();
  mChain.engine().setValueSet(set);
  mAppliedValueSet.store(set);
  bool ready = mEngineReady.load();
  if (format) {
    // Always two channels in: outputs[1] exists, as scratch, on a mono bus; the chain's output is always stereo.
    ready = mChain.prepare(rate, block, 2);
    mTuner.prepare(rate);  // the audio thread is out: nothing pushes meanwhile
    if (ready) mChain.applyStructuralParams();
    mEngineRate.store(rate);
    mEngineBlock.store(block);
  } else if (ready) {
    mChain.applyStructuralParams();
  }
  // The cabinet's first response is built on its worker; the audio starts with it, so that a new instance (which
  // plays the built-in cabinet) and an offline render do not depend on how fast the worker was. The reset comes after
  // the wait: a response handed over by then plays from the first sample, without a crossfade from the dry signal.
  if (ready && mChain.blockOn(magmora::ChainBlock::Cabinet)) WaitForCabinet();
  if (ready) mChain.reset();

  mEngineReady.store(ready);
  mRateRejected.store(!ready);
  UpdateLatency(ready);
  UpdateTail();

  mFadeOut.store(false);
  mSuspended.store(false);
}

// The cabinet's response for the current controls, handed to the audio thread. A real-time host waits at most a
// second (its main thread is blocked meanwhile); an offline render waits for it, so that what it renders is exact.
bool Magmora::WaitForCabinet() {
  if (mChain.waitForCabinet(1.0)) return true;
  if (mOffline.load() && mChain.waitForCabinet(30.0)) return true;
  DBGMSG("Magmora: the cabinet's response was not ready in time; it fades in when it is\n");
  return false;
}

// The head rings out for kEngineTailSeconds; the cabinet's convolution adds its response's length. mConfigMutex held.
void Magmora::UpdateTail() {
  const double rate = mEngineRate.load();
  if (!(rate > 0.)) return;
  const double cabinet = mChain.blockOn(magmora::ChainBlock::Cabinet) ? mChain.cabinetResponseSeconds() : 0.;
  const int tail = static_cast<int>(std::ceil((kEngineTailSeconds + cabinet) * rate));
  if (tail != mTailSamples.exchange(tail)) SetTailSize(tail);
}

void Magmora::OnReset() {
#if defined AU_API
  if (mInBypassCall) return;
#endif
  // Hosts also call this from the audio thread (AudioUnitReset, setProcessing(false)). There only the engine reset
  // is requested; a new format, which hosts do not set while processing, waits for OnIdle (see OnAudioThread).
  const int block = std::min(std::max(GetBlockSize(), 1), kMaxChunk);
  const bool format = GetSampleRate() != mEngineRate.load() || block != mEngineBlock.load();
  if (format && !OnAudioThread()) Reconfigure();
  mResetPending.store(true);
}

// Needed for AUv2: iPlug2 does not call OnReset() from AudioUnitInitialize, and a host does not have to set
// the sample rate or slice size before initialising.
void Magmora::OnActivate(bool active) {
  if (!active) return;
#if defined AU_API
  if (mInBypassCall) return;
#endif
  if (!OnAudioThread()) Reconfigure();
  mResetPending.store(true);
}

void Magmora::OnIdle() {
  if (mDying.load()) return;
  const int block = std::min(std::max(GetBlockSize(), 1), kMaxChunk);
  const bool format = mEngineRate.load() > 0. && (GetSampleRate() != mEngineRate.load() || block != mEngineBlock.load());
  if (format || (mEngineReady.load() && (StructureChanged() || TransposeChanged()))) Reconfigure();
  if (mNotifyLatency.exchange(false)) SetLatency(mReportedLatency.load());
  if (mUserCheck.exchange(false)) LoadUserComparison();
  if (mLoadReferences.exchange(false)) {
    std::lock_guard<std::mutex> lock(mChainMutex);
    LoadReferences();
  }
  mChain.idle();
  {
    // A state load on another thread holds the chain: the editor keeps its copy until the next idle.
    std::unique_lock<std::mutex> lock(mChainMutex, std::try_to_lock);
    if (lock) RefreshCabView();
  }
  {
    std::unique_lock<std::mutex> lock(mConfigMutex, std::try_to_lock);  // a response file loaded, or the cabinet off
    if (lock) UpdateTail();
  }
  // iPlug2's index of the current preset, which its AU and VST3 code read: brought up to date here, on the main thread.
  const int preset = mPresetIdx.load();
  if (GetCurrentPresetIdx() != preset) SetCurrentPresetIdx(preset);
  const float scale = mPendingScale.exchange(0.f);
  if (scale > 0.f && GetUI()) GetUI()->Resize(PLUG_WIDTH, PLUG_HEIGHT, scale);
}

// ---- audio -------------------------------------------------------------------------------------------------------

void Magmora::PushParams() {
  for (int i = 0; i < kAll; ++i) {
    if (IsStructural(i)) continue;
    const double value = GetParam(i)->Value();
    if (value == mSent[i] || !std::isfinite(value)) continue;
    mSent[i] = value;
    mChain.setParameter(i, value);
  }
}

void Magmora::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) {
  if (nFrames <= 0) return;

  struct InProcess {
    std::atomic<bool>& flag;
    explicit InProcess(std::atomic<bool>& f) : flag(f) {
      flag.store(true);
      tRendered = true;
      ++tInProcess;
    }
    ~InProcess() {
      --tInProcess;
      flag.store(false);
    }
  } inProcess(mInProcess);

  const bool run = !mSuspended.load() && mEngineReady.load();
  mBlocks.fetch_add(1, std::memory_order_relaxed);
  if (GetRenderingOffline() != mOffline.load(std::memory_order_relaxed)) mOffline.store(GetRenderingOffline());  // iPlug2 sets it here

  if (mResetPending.exchange(false)) {
    for (auto& channel : mDry) std::fill(channel.begin(), channel.end(), 0.);
    mDryMix = mBypass.load() ? 1. : 0.;
    mWetHoldOff = 0;
    mResumeEngine = true;
  }
  if (run) PushParams();

  int nIn = NInChansConnected();
#if defined VST3_API
  if (mNoInput) nIn = 0;
#endif
  const bool stereoOut = NOutChansConnected() > 1;
  const int chunk = run ? mEngineBlock.load() : kMaxChunk;

  for (int at = 0; at < nFrames; at += chunk) {
    const int n = std::min(chunk, nFrames - at);
    // The channels may alias each other crosswise (1 in, 2 out, in place), so the engine is fed from copies.
    const iplug::sample* left = nIn > 0 ? inputs[0] + at : nullptr;
    const iplug::sample* right = nIn > 1 ? inputs[1] + at : left;
    ProcessChunk(left, right, outputs[0] + at, stereoOut ? outputs[1] + at : mScratchR.data(), n, run);
  }
}

void Magmora::ProcessChunk(const iplug::sample* left, const iplug::sample* right, iplug::sample* outL,
                          iplug::sample* outR, int n, bool run) {
  double* inL = mIn[0].data();
  double* inR = mIn[1].data();
  for (int i = 0; i < n; ++i) {
    const double l = left ? left[i] : 0., r = right ? right[i] : 0.;
    inL[i] = std::isfinite(l) ? l : 0.;
    inR[i] = std::isfinite(r) ? r : 0.;
  }

  const double trim = GetParam(magmora::kInputTrim)->Value();
  if (trim != mTrimDb) {
    mTrimDb = trim;
    mTrimGain = std::pow(10., trim / 20.);
  }
  const auto route = static_cast<magmora::ChannelMode>(static_cast<int>(mApplied[magmora::kChannelMode].load()));
  double inPeak = 0.;
  for (int i = 0; i < n; ++i) {
    double x = std::fabs(inL[i]);
    if (route == magmora::ChannelMode::MonoSum) x = std::fabs(0.5 * (inL[i] + inR[i]));
    if (route == magmora::ChannelMode::DualMono) x = std::max(x, std::fabs(inR[i]));
    inPeak = std::max(inPeak, x);
  }

  if (mTunerOn.load(std::memory_order_relaxed)) {
    // What the amp gets (the routing's channel), at the meter's level; the tuner reads it on the editor's timer.
    double* mono = mTunerIn.data();
    for (int i = 0; i < n; ++i)
      mono[i] = route == magmora::ChannelMode::MonoSum ? 0.5 * (inL[i] + inR[i]) : inL[i];
    mTuner.push(mono, n, mTrimGain);
  }

  const bool bypass = mBypass.load();
  const bool fullyDry = bypass && mDryMix >= 1.;
  const int latency = mReportedLatency.load(std::memory_order_relaxed);
  bool wet = run && !fullyDry;
  if (wet) {
    if (mResumeEngine) {
      mResumeEngine = false;
      mChain.reset();
      mWetHoldOff = latency;
    }
    const double* in[2] = {inL, inR};
    double* out[2] = {outL, outR};
    mChain.process(in, out, n);
  } else if (fullyDry) {
    mResumeEngine = true;
  }

  const double rate = std::max(GetSampleRate(), 8000.);
  const double mixStep = 1. / (0.010 * rate);   // bypass: 10 ms
  const double gainStep = 1. / (0.005 * rate);  // around a reconfiguration: 5 ms
  const bool fadeOut = mFadeOut.load();
  if (!run) mWetGain = 0.;
  const double muteTarget = mTunerMute.load(std::memory_order_relaxed) ? 0. : 1.;

  double outPeak = 0.;
  constexpr int mask = kDryLength - 1;
  // The dry path follows the reported latency. When it changes (the transposer in or out), the dry path crossfades
  // from the old delay to the new one in 10 ms, so that a bypassed signal does not jump.
  if (latency != mDryLatency) {
    mDryOldLatency = mDryLatency < 0 ? latency : mDryLatency;
    mDryLatency = latency;
    mDryShift = mDryMix > 0. ? 0. : 1.;
  }
  for (int i = 0; i < n; ++i) {
    const auto at = static_cast<size_t>(mDryPos), from = static_cast<size_t>((mDryPos - latency) & mask);
    const auto old = static_cast<size_t>((mDryPos - mDryOldLatency) & mask);
    mDry[0][at] = inL[i];
    mDry[1][at] = inR[i];
    mDryPos = (mDryPos + 1) & mask;
    double dryL = mDry[0][from], dryR = mDry[1][from];
    if (mDryShift < 1.) {
      mDryShift = std::min(1., mDryShift + mixStep);
      dryL = mDry[0][old] + mDryShift * (dryL - mDry[0][old]);
      dryR = mDry[1][old] + mDryShift * (dryR - mDry[1][old]);
    }

    if (bypass) {
      mDryMix = std::min(1., mDryMix + mixStep);
    } else if (mWetHoldOff > 0) {
      --mWetHoldOff;
    } else {
      mDryMix = std::max(0., mDryMix - mixStep);
    }
    if (run) mWetGain = fadeOut ? std::max(0., mWetGain - gainStep) : std::min(1., mWetGain + gainStep);

    const double g = wet ? mWetGain * (1. - mDryMix) : 0.;
    const double l = (wet ? outL[i] * g : 0.) + dryL * mDryMix;
    const double r = (wet ? outR[i] * g : 0.) + dryR * mDryMix;
    // The tuner's mute: a 10 ms fade either way.
    if (mMuteGain != muteTarget)
      mMuteGain = muteTarget > mMuteGain ? std::min(1., mMuteGain + mixStep) : std::max(0., mMuteGain - mixStep);
    outL[i] = l * mMuteGain;
    outR[i] = r * mMuteGain;
    outPeak = std::max(outPeak, std::max(std::fabs(outL[i]), std::fabs(outR[i])));
  }
  if (fadeOut && (mWetGain <= 0. || fullyDry)) mFadedOut.store(true);

  KeepPeak(mInputPeak, inPeak * mTrimGain);
  KeepPeak(mOutputPeak, outPeak);
}

// ---- state and presets -------------------------------------------------------------------------------------------

namespace {
// One section of the chain container: where its tag, hash and payload are.
struct Section {
  char tag[4];
  int32_t version;
  std::string hash;
  int payload, size;  // offsets into the chunk
};

// The container's sections and its end, checked for size only (Chain::setState checks the rest).
bool WalkContainer(const iplug::IByteChunk& chunk, int from, std::vector<Section>& sections, int& end) {
  Reader in(chunk, from);
  uint32_t magic = 0;
  int32_t version = 0, count = 0;
  if (!in.Get(magic) || magic != kChainMagic || !in.Get(version) || !in.Get(count) || count < 0 || count > 64)
    return false;
  int pos = from + 12;
  for (int32_t n = 0; n < count; ++n) {
    Section s;
    int32_t length = 0;
    Reader at(chunk, pos);
    if (!at.Get(s.tag) || !at.Get(s.version) || !at.Get(length) || length < 0 || length > kMaxString) return false;
    s.hash.assign(static_cast<size_t>(length), '\0');
    for (int32_t k = 0; k < length; ++k)
      if (chunk.GetBytes(&s.hash[static_cast<size_t>(k)], 1, pos + 12 + k) < 0) return false;
    Reader size(chunk, pos + 12 + length);
    if (!size.Get(s.size) || s.size < 0 || s.size > kMaxBody) return false;
    s.payload = pos + 16 + length;
    if (s.payload + s.size > chunk.Size()) return false;
    pos = s.payload + s.size;
    sections.push_back(std::move(s));
  }
  end = pos;
  return true;
}

void PutSection(iplug::IByteChunk& out, const char* tag, int32_t version, const std::string& hash, const void* data,
                int32_t size) {
  out.PutBytes(tag, 4);
  out.Put(&version);
  const int32_t length = static_cast<int32_t>(hash.size());
  out.Put(&length);
  if (length) out.PutBytes(hash.data(), length);
  out.Put(&size);
  if (size) out.PutBytes(data, size);
}
// A "cabp" payload (Chain's: folder, mapping file, then the rest) with its first two strings replaced.
std::vector<uint8_t> PackWithFolder(const iplug::IByteChunk& chunk, const Section& s, const std::string& folder,
                                    const std::string& mapping) {
  const uint8_t* data = chunk.GetData() + s.payload;
  std::vector<uint8_t> out;
  const auto put = [&out](const std::string& text) {
    const int32_t n = static_cast<int32_t>(text.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
    out.insert(out.end(), text.begin(), text.end());
  };
  int at = 0;
  for (int k = 0; k < 2; ++k) {
    int32_t n = 0;
    if (at + 4 > s.size) return std::vector<uint8_t>(data, data + s.size);
    std::memcpy(&n, data + at, 4);
    if (n < 0 || at + 4 + n > s.size) return std::vector<uint8_t>(data, data + s.size);
    at += 4 + n;
  }
  put(folder);
  put(mapping);
  out.insert(out.end(), data + at, data + s.size);
  return out;
}
}  // namespace

bool Magmora::SerializeState(iplug::IByteChunk& chunk) const {
  std::lock_guard<std::mutex> lock(mChainMutex);
  // The blocks' values as the host sees them (the audio thread copies them into the chain only while it runs).
  for (int i = magmora::kNumParams; i < kAll; ++i) mChain.setParameter(i, GetParam(i)->Value());
  std::vector<uint8_t> state;
  mChain.getState(state);
  iplug::IByteChunk container;
  container.PutBytes(state.data(), static_cast<int>(state.size()));
  std::vector<Section> sections;
  int end = 0;
  if (!WalkContainer(container, 0, sections, end)) return false;

  // The head section is this plug-in's head chunk: the values the host sees, the value set wanted, preset and scale.
  double values[magmora::kNumParams];
  for (int i = 0; i < magmora::kNumParams; ++i) values[i] = GetParam(i)->Value();
  iplug::IByteChunk head;
  WriteState(head, 0, values, kPresets[PresetIndex()].name, mScale.load(), mValueSet.load());

  // The user preset in use: its name and its file relative to the presets folder ("upre", this plug-in's section; the
  // chain keeps a copy of the one it was given as an unknown section, which is replaced here).
  std::string userName, userFile;
  {
    std::lock_guard<std::mutex> user(mUserMutex);
    if (mUser.active) {
      userName = mUser.name;
      userFile = mUser.relative;
    }
  }
  const uint32_t magic = kChainMagic;
  int32_t version = 0, count = userName.empty() ? 0 : 1;
  for (const Section& s : sections) count += std::memcmp(s.tag, kUserSection, 4) == 0 ? 0 : 1;
  container.GetBytes(&version, 4, 4);
  chunk.Put(&magic);
  chunk.Put(&version);
  chunk.Put(&count);
  if (!userName.empty()) {
    iplug::IByteChunk user;
    PutString(user, userName.c_str());
    PutString(user, userFile.c_str());
    PutSection(chunk, kUserSection, 1, "", user.GetData(), user.Size());
  }
  for (const Section& s : sections) {
    if (std::memcmp(s.tag, kUserSection, 4) == 0) continue;
    if (std::memcmp(s.tag, "head", 4) == 0)
      PutSection(chunk, "head", s.version, magmora::Engine::valueSetModelHash(mValueSet.load()), head.GetData(), head.Size());
    else if (std::memcmp(s.tag, "cabp", 4) == 0 && mPackMissing)  // keep the session's pack, although it is absent
    {
      const std::vector<uint8_t> payload = PackWithFolder(container, s, mMissingFolder, mMissingMapping);
      PutSection(chunk, "cabp", s.version, s.hash, payload.data(), static_cast<int32_t>(payload.size()));
    }
    else
      PutSection(chunk, std::string(s.tag, 4).c_str(), s.version, s.hash, container.GetData() + s.payload, s.size);
  }
  return true;
}

// The head chunk ("SCRA"): a session's head, or a preset of the head alone. Returns its end, or 0 if malformed.
int Magmora::ReadHead(const iplug::IByteChunk& chunk, int startPos, bool apply, int end) {
  Reader in(chunk, startPos, end);  // within its section; its own body size then narrows the reading (Limit)
  uint32_t magic = 0;
  int32_t version = 0, size = 0, flags = 0, count = 0;
  // Failure is 0: iPlug2's AU state path tests the result for truth, so -1 would report success to the host.
  if (!in.Get(magic) || magic != kMagic || !in.Get(version) || version < 1) return 0;
  if (!in.Get(size) || size > kMaxBody || !in.Limit(size)) return 0;
  if (!in.Get(flags) || !in.Get(count) || count < 0 || count > kMaxEntries) return 0;

  const bool preset = (flags & kPresetChunk) != 0;
  double values[magmora::kNumParams];
  for (int i = 0; i < magmora::kNumParams; ++i)
    values[i] = preset && !InPresets(i) ? GetParam(i)->Value() : magmora::kParams[i].def;
  for (int32_t n = 0; n < count; ++n) {
    std::string key;
    double value = 0.;
    if (!in.String(key) || !in.Get(value)) return 0;
    if (!std::isfinite(value)) continue;
    for (const magmora::ParamInfo& p : magmora::kParams)
      if (key == p.key && !(preset && !InPresets(p.id))) values[p.id] = value;
  }
  std::string name;
  float scale = 0.f;
  if (!in.String(name) || !in.Get(scale)) return 0;
  std::string setName = kVersion1ValueSet, setHash;
  if (version >= 2 && (!in.String(setName) || !in.String(setHash))) return 0;
  if (!apply) return in.End();

  // Nothing is touched until the whole chunk has been read.
  for (int i = 0; i < magmora::kNumParams; ++i) GetParam(i)->Set(values[i]);
  for (int i = 0; i < kNumPresets; ++i)
    if (name == kPresets[i].name) mPresetIdx.store(i);

  if (!preset) {
    // The value set belongs to the session. An unknown name falls back to the default; a model that changed since
    // the session was saved keeps the set (the circuit the player chose) and leaves a note.
    int set = magmora::Engine::findValueSet(setName.c_str());
    std::string note;
    if (set < 0) {
      set = magmora::Engine::defaultValueSet();
      note = "Unknown circuit in this session; playing the default.";
    } else if (!setHash.empty() && setHash != magmora::Engine::valueSetModelHash(set)) {
      note = "Saved with an earlier model of this circuit (" + setHash.substr(0, 8) + ").";
    }
    mValueSet.store(set);
    {
      std::lock_guard<std::mutex> lock(mNoteMutex);
      mSessionNote = note;
    }
    if (std::isfinite(scale) && scale > 0.f) {
      scale = std::min(std::max(scale, magmora::ui::kMinScale), magmora::ui::kMaxScale);
      mScale.store(scale);
      if (GetUI()) {
        mPendingScale.store(scale);  // this need not be the main thread
      } else {
        SetEditorSize(ScaledPoints(PLUG_WIDTH, scale), ScaledPoints(PLUG_HEIGHT, scale));
        iplug::IByteChunk editor;
        const int w = PLUG_WIDTH, h = PLUG_HEIGHT;
        editor.Put(&w);
        editor.Put(&h);
        editor.Put(&scale);
        UnserializeEditorSize(editor, 0);
      }
    }
  }
  return in.End();
}

// What UnserializeState would accept, without changing anything. Any thread.
bool Magmora::StateIsValid(const iplug::IByteChunk& chunk, int startPos) {
  Reader peek(chunk, startPos);
  uint32_t magic = 0;
  if (!peek.Get(magic)) return false;
  if (magic == kPresetMagic) {
    int32_t index = -1;
    return peek.Get(index) && index >= 0 && index < kNumPresets;
  }
  int end = 0;
  if (magic == kMagic) {
    end = ReadHead(chunk, startPos, false);  // apply = false: reads only
    if (end <= 0) return false;
    int32_t flags = 0;
    chunk.GetBytes(&flags, 4, startPos + 12);
    if (flags & kPresetChunk) return true;
  } else if (magic == kChainMagic) {
    std::vector<Section> sections;
    if (!WalkContainer(chunk, startPos, sections, end)) return false;
    const Section* head = nullptr;
    for (const Section& s : sections)
      if (std::memcmp(s.tag, "head", 4) == 0) head = &s;
    if (!head || ReadHead(chunk, head->payload, false, head->payload + head->size) <= 0)
      return false;
  } else {
    return false;
  }
  return magmora::Chain::checkState(chunk.GetData() + startPos, static_cast<size_t>(end - startPos));
}

// Any thread but a real-time one in practice (hosts load states on their main thread or a worker). The state is
// checked whole first; then everything that is not a parameter is applied under mChainMutex, and the pack and the
// response files it references are loaded at once (on an audio thread: by the next OnIdle), so that an offline render
// that follows starts with the session's cabinet.
int Magmora::UnserializeState(const iplug::IByteChunk& chunk, int startPos) {
  if (!StateIsValid(chunk, startPos)) return 0;
  Reader peek(chunk, startPos);
  uint32_t magic = 0;
  peek.Get(magic);

  if (magic == kPresetMagic) {  // a factory preset, by index
    int32_t index = -1;
    peek.Get(index);
#if defined AU_API
    // Restoring an AU state, iPlug2 first applies the factory preset the dictionary names, then the state's data:
    // that first step is skipped (the data sets everything, and may be a different preset).
    int expected = index;
    if (mSkipPresetOfAUState.compare_exchange_strong(expected, -1)) return startPos + 8;
#endif
    ApplyPresetValues(index, false);
    mPresetIdx.store(index);
    ClearUserPreset();
    if (OnMainThread()) SetCurrentPresetIdx(index);
    return startPos + 8;
  }

  int end = 0, headAt = startPos, headEnd = -1;
  std::string userName, userFile;
  if (magic == kMagic) {
    // The head alone: a session from before the chain (its blocks load off), or a preset of the head.
    end = ReadHead(chunk, startPos, false);
    int32_t flags = 0;
    chunk.GetBytes(&flags, 4, startPos + 12);
    if (flags & kPresetChunk) {
      ClearUserPreset();
      return ReadHead(chunk, startPos, true);
    }
  } else {
    std::vector<Section> sections;
    WalkContainer(chunk, startPos, sections, end);
    for (const Section& s : sections) {
      if (std::memcmp(s.tag, "head", 4) == 0) {
        headAt = s.payload;
        headEnd = s.payload + s.size;
      }
      if (std::memcmp(s.tag, kUserSection, 4) == 0) {
        // Two strings; anything malformed is ignored (the session then shows its factory preset's name).
        Reader in(chunk, s.payload, s.payload + s.size);
        const auto text = [&in](std::string& out) {
          int32_t length = 0;
          if (!in.Get(length) || length < 0 || length > kMaxUserPath) return false;
          out.assign(static_cast<size_t>(length), '\0');
          for (int32_t k = 0; k < length; ++k)
            if (!in.Get(out[static_cast<size_t>(k)])) return false;
          return true;
        };
        std::string name, file;
        if (text(name) && text(file)) {
          userName = magmora::cleanPresetName(name);
          userFile = file;
        }
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(mChainMutex);
    // Checked above: Chain::setState accepts it (and would change nothing if it did not).
    if (!mChain.setState(chunk.GetData() + startPos, static_cast<size_t>(end - startPos))) return 0;
    ReadHead(chunk, headAt, true, headEnd);
    PullBlockParams();
    {
      std::lock_guard<std::mutex> user(mUserMutex);
      mUser = UserState{};
      if (!userName.empty()) {
        mUser.active = true;
        mUser.name = userName;
        mUser.relative = userFile;
      }
    }
    if (!userName.empty()) mUserCheck.store(true);  // its file is read on the main thread, for the edit mark
    // The pack and the response files are referenced, not stored: they are read now, unless this is an audio thread.
    if (OnAudioThread())
      mLoadReferences.store(true);
    else
      LoadReferences();
    if (OnMainThread()) {
      RefreshCabView();
      SetCurrentPresetIdx(mPresetIdx.load());
    }
  }
  // Now, not at the next idle: an offline render may follow at once and the latency must be right.
  StructureMayHaveChanged();
  return end;
}

#if defined AU_API
void Magmora::BeginAUStateRestore(CFPropertyListRef list) {
  int named = -1;
  const void* value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(list), CFSTR(kAUPresetNameKey));
  char name[256] = {};
  if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
      CFStringGetCString(static_cast<CFStringRef>(value), name, sizeof name, kCFStringEncodingUTF8))
    for (int i = 0; i < kNumPresets && named < 0; ++i)
      if (std::strcmp(name, kPresets[i].name) == 0) named = i;
  mSkipPresetOfAUState.store(named);
}

void Magmora::EndAUStateRestore() { mSkipPresetOfAUState.store(-1); }

bool Magmora::AcceptsAUClassInfo(CFPropertyListRef list) {
  if (!list || CFGetTypeID(list) != CFDictionaryGetTypeID()) return false;
  const void* value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(list), CFSTR(kAUPresetDataKey));
  if (!value || CFGetTypeID(value) != CFDataGetTypeID()) return false;
  const auto data = static_cast<CFDataRef>(value);
  const CFIndex size = CFDataGetLength(data);
  if (size <= 0 || size > kMaxBody + 4096) return false;
  iplug::IByteChunk chunk;
  chunk.PutBytes(CFDataGetBytePtr(data), static_cast<int>(size));
  return StateIsValid(chunk, 0);
}
#endif

void Magmora::PullBlockParams() {
  for (int i = magmora::kNumParams; i < kAll; ++i) GetParam(i)->Set(mChain.getParameter(i));
}

void Magmora::LoadReferences() {  // mChainMutex held
  // Each slot's response file (an older session's series response file arrives here as slot 2's).
  for (int slot = 0; slot < 2; ++slot) {
    const std::string ir = mChain.cabinetIrReference(slot);
    mIrMessage[slot].clear();
    if (ir.empty()) {
      mChain.clearCabinetIr(slot);
      continue;
    }
    std::string error;
    if (!mChain.loadCabinetIr(slot, ir.c_str(), &error)) {
      mChain.clearCabinetIr(slot);
      mChain.setCabinetIrReference(slot, ir.c_str());  // kept, so that saving the session does not lose it
      mIrMessage[slot] = "Could not load " + ir + ": " + (error.empty() ? std::string("unreadable") : error);
    }
  }
  const std::string folder = mChain.cabinetPackReference(), mapping = mChain.cabinetMappingReference();
  mCabMessage.clear();
  mPackMissing = false;
  if (folder.empty()) {
    mChain.clearCabinetPack();
  } else {
    std::string error;
    const bool loaded = builtin_cab::IsReference(folder)
                            ? builtin_cab::Load(mChain, &error)
                            : mChain.loadCabinetPack(folder.c_str(), &error, mapping.empty() ? nullptr : mapping.c_str());
    if (loaded) {
      // The saved microphone names are found in the pack: their indexes may differ from the saved ones.
      GetParam(magmora::kMic1Type)->Set(mChain.getParameter(magmora::kMic1Type));
      GetParam(magmora::kMic2Type)->Set(mChain.getParameter(magmora::kMic2Type));
    } else {
      mChain.clearCabinetPack();
      mPackMissing = true;
      mMissingFolder = folder;
      mMissingMapping = mapping;
      mCabMessage = "Could not load the pack " + folder + ": " + error;
    }
  }
  // As in Reconfigure: an offline render that follows the state load starts with the session's cabinet. Audio may be
  // running, so the chain is not reset here: an offline render asks the audio thread to reset at its next block (the
  // response then plays from the first sample); in real time the new response crossfades in.
  if (mEngineReady.load() && mChain.blockOn(magmora::ChainBlock::Cabinet) && WaitForCabinet() && mOffline.load())
    mResetPending.store(true);
}

int Magmora::CircuitCount() const { return magmora::Engine::valueSetCount(); }

const char* Magmora::CircuitLabel(int index) const {
  const char* name = magmora::Engine::valueSetName(index);
  if (!name) return "";
  if (std::strcmp(name, "prototype") == 0) return "Revised";
  if (std::strcmp(name, "as-drawn") == 0) return "As drawn (1992)";
  return name;
}

void Magmora::ChooseCircuit(int index) {
  if (index < 0 || index >= magmora::Engine::valueSetCount()) return;
  mValueSet.store(index);
  {
    std::lock_guard<std::mutex> lock(mNoteMutex);
    mSessionNote.clear();
  }
  StructureMayHaveChanged();
}

std::string Magmora::SessionNote() const {
  std::lock_guard<std::mutex> lock(mNoteMutex);
  return mSessionNote;
}

int Magmora::PresetCount() const { return kNumPresets; }

const char* Magmora::PresetName(int index) const {
  return index >= 0 && index < kNumPresets ? kPresets[index].name : "";
}

const char* Magmora::PresetCategory(int index) const {
  return index >= 0 && index < kNumPresets ? kPresets[index].category : "";
}

int Magmora::PresetIndex() const { return std::min(std::max(mPresetIdx.load(), 0), kNumPresets - 1); }

// A preset sets the head's preset parameters and every parameter of the blocks it has an object for; the session's
// parameters and the blocks it does not mention stay as they are (Chain::applyPreset).
void Magmora::ApplyPresetValues(int index, bool withGestures) {
  if (index < 0 || index >= kNumPresets) return;
  for (int i = 0; i < kAll; ++i) {
    if (!kPresets[index].given[i]) continue;
    const double wanted = kPresets[index].values[i];
    if (GetParam(i)->Value() == wanted) continue;
    if (!withGestures) {
      GetParam(i)->Set(wanted);
      continue;
    }
    BeginInformHostOfParamChangeFromUI(i);
    SendParameterValueFromUI(i, GetParam(i)->ToNormalized(wanted));
    EndInformHostOfParamChangeFromUI(i);
  }
}

void Magmora::ChoosePreset(int index) {
  if (index < 0 || index >= kNumPresets) return;
  ClearUserPreset();
  ApplyPresetValues(index, true);
  mPresetIdx.store(index);
  SetCurrentPresetIdx(index);
  InformHostOfPresetChange();
}

std::vector<std::string> Magmora::CabSpeakers(int mic) const {
  const int index = static_cast<int>(std::lround(GetParam(mic == 0 ? magmora::kMic1Type : magmora::kMic2Type)->Value()));
  return mChain.cabinetSpeakers(index);
}

std::vector<std::string> Magmora::CabVariants(int mic, const std::string& speaker) const {
  const int index = static_cast<int>(std::lround(GetParam(mic == 0 ? magmora::kMic1Type : magmora::kMic2Type)->Value()));
  return mChain.cabinetVariants(index, speaker.c_str());
}

std::string Magmora::CabSource(int mic, bool variant) const {
  return variant ? mChain.cabinetMicVariant(mic) : mChain.cabinetMicSpeaker(mic);
}

void Magmora::SetCabSource(int mic, const std::string& speaker, const std::string& variant) {
  mChain.setCabinetMicSource(mic, speaker.c_str(), variant.c_str());
}

// The editor's cabinet calls (main thread). Those that change the chain's references take mChainMutex; the editor
// reads references and messages from mCabView.
std::string Magmora::LoadCabPack(const std::string& folder, const std::string& mapping) {
  std::lock_guard<std::mutex> lock(mChainMutex);
  std::string error;
  if (!mChain.loadCabinetPack(folder.c_str(), &error, mapping.empty() ? nullptr : mapping.c_str())) return error;
  mPackMissing = false;
  mCabMessage.clear();
  RefreshCabView();
  for (int id : {int(magmora::kMic1Type), int(magmora::kMic2Type)}) SetParameterFromUI(id, mChain.getParameter(id));
  return {};
}

std::string Magmora::BuiltinCabName() const { return builtin_cab::kEnabled ? builtin_cab::kName : ""; }

bool Magmora::CabIsBuiltin() const { return mCabView.builtin; }

std::string Magmora::LoadBuiltinCab() {
  std::lock_guard<std::mutex> lock(mChainMutex);
  std::string error;
  if (!builtin_cab::Load(mChain, &error)) return error.empty() ? "could not load the built-in cabinet" : error;
  mPackMissing = false;
  mCabMessage.clear();
  RefreshCabView();
  for (int id : {int(magmora::kMic1Type), int(magmora::kMic2Type)}) SetParameterFromUI(id, mChain.getParameter(id));
  return {};
}

void Magmora::ClearCabPack() {
  std::lock_guard<std::mutex> lock(mChainMutex);
  mChain.clearCabinetPack();
  mPackMissing = false;
  mCabMessage.clear();
  RefreshCabView();
}

std::string Magmora::CabMessage() const {
  const magmora::CabinetStatus status = mChain.cabinetStatus();
  return !status.error.empty() ? status.error : mCabView.message;
}

magmora::ui::CabIrInfo Magmora::CabIr(int slot) const {
  return slot == 0 || slot == 1 ? mCabView.ir[slot] : magmora::ui::CabIrInfo{};
}

void Magmora::RefreshCabView() {
  CabView v;
  v.builtin = mChain.cabinetPackLoaded() && builtin_cab::IsReference(mChain.cabinetPackReference());
  v.pack = mPackMissing ? mMissingFolder : std::string(mChain.cabinetPackReference());
  v.mapping = mPackMissing ? mMissingMapping : std::string(mChain.cabinetMappingReference());
  v.message = mCabMessage;
  for (int slot = 0; slot < 2; ++slot) {
    magmora::ui::CabIrInfo& info = v.ir[slot];
    info.loaded = mChain.cabinetIrLoaded(slot);
    info.reference = mChain.cabinetIrReference(slot);
    info.seconds = mChain.cabinetIrSeconds(slot);
    info.channels = mChain.cabinetIrChannels(slot);
    info.normalize = mChain.cabinetIrNormalize(slot);
    info.message = mIrMessage[slot];
  }
  mCabView = std::move(v);
}

std::string Magmora::LoadCabIr(int slot, const std::string& path) {
  if (slot < 0 || slot > 1) return "no such slot";
  std::lock_guard<std::mutex> lock(mChainMutex);
  std::string error;
  if (!mChain.loadCabinetIr(slot, path.c_str(), &error)) return error.empty() ? "could not read the file" : error;
  mIrMessage[slot].clear();
  mLastIrFolder = user_presets::ToUtf8(user_presets::FromUtf8(path).parent_path());
  RefreshCabView();
  return {};
}

void Magmora::ClearCabIr(int slot) {
  if (slot < 0 || slot > 1) return;
  std::lock_guard<std::mutex> lock(mChainMutex);
  mChain.clearCabinetIr(slot);
  mIrMessage[slot].clear();
  RefreshCabView();
}

void Magmora::SetCabIrNormalize(int slot, bool on) {
  if (slot < 0 || slot > 1) return;
  std::lock_guard<std::mutex> lock(mChainMutex);
  mChain.setCabinetIrNormalize(slot, on);
  RefreshCabView();
}

// A parameter the editor (or one of its late callbacks) sets: one gesture, so that the host records it.
void Magmora::SetParameterFromUI(int id, double plain) {
  if (id < 0 || id >= kAll || mDying.load() || GetParam(id)->Value() == plain) return;
  BeginInformHostOfParamChangeFromUI(id);
  SendParameterValueFromUI(id, GetParam(id)->ToNormalized(plain));
  EndInformHostOfParamChangeFromUI(id);
}

// ---- user presets ------------------------------------------------------------------------------------------------
//
// Files in the user's presets folder (plugin/UserPresets.h), in the format of magmora/Preset.h. Everything here runs
// on the main thread, from the editor; a state load on another thread only records the name and file (mUser), and
// OnIdle reads the file for the edit mark.

namespace {
std::string BaseName(const std::string& path) { return user_presets::ToUtf8(user_presets::FromUtf8(path).filename()); }

bool FactoryName(const std::string& name) {
  for (int i = 0; i < kNumPresets; ++i)
    if (user_presets::Lower(name) == user_presets::Lower(kPresets[i].name)) return true;
  return false;
}
}  // namespace

std::vector<magmora::ui::UserPresetInfo> Magmora::UserPresets() {
  std::vector<magmora::ui::UserPresetInfo> out;
  for (const user_presets::Entry& e : user_presets::Scan()) out.push_back({e.name, e.folder, e.path});
  return out;
}

std::vector<std::string> Magmora::UserPresetFolders() { return user_presets::Folders(); }

std::string Magmora::CurrentPresetName() const {
  {
    std::lock_guard<std::mutex> lock(mUserMutex);
    if (mUser.active) return mUser.name;
  }
  return PresetName(PresetIndex());
}

std::string Magmora::UserPresetPath() const {
  std::lock_guard<std::mutex> lock(mUserMutex);
  return mUser.active ? user_presets::Absolute(mUser.relative) : std::string();
}

bool Magmora::UserPresetLoaded() const {
  std::lock_guard<std::mutex> lock(mUserMutex);
  return mUser.active;
}

std::vector<std::string> Magmora::MissingPresetFiles() const {
  std::lock_guard<std::mutex> lock(mUserMutex);
  return mUser.active ? mUser.missing : std::vector<std::string>();
}

void Magmora::SetUserPreset(const std::string& name, const std::string& path, const magmora::UserPreset* compare) {
  std::lock_guard<std::mutex> lock(mUserMutex);
  mUser = UserState{};
  mUser.active = true;
  mUser.name = name;
  mUser.relative = user_presets::Relative(path);
  if (compare) {
    mUser.compare = true;
    mUser.preset = *compare;
  }
}

void Magmora::ClearUserPreset() {
  std::lock_guard<std::mutex> lock(mUserMutex);
  mUser = UserState{};
}

void Magmora::LoadUserComparison() {
  std::string relative;
  {
    std::lock_guard<std::mutex> lock(mUserMutex);
    if (!mUser.active) return;
    relative = mUser.relative;
  }
  magmora::UserPreset preset;
  const std::string path = user_presets::Absolute(relative);
  const bool read = !path.empty() && user_presets::ReadPreset(user_presets::FromUtf8(path), preset, nullptr);
  std::lock_guard<std::mutex> lock(mUserMutex);
  if (mUser.active && mUser.relative == relative) {
    mUser.compare = read;
    if (read) mUser.preset = std::move(preset);
  }
}

std::string Magmora::LastIrFolder() const { return mLastIrFolder; }

magmora::UserPreset Magmora::Snapshot() {
  magmora::UserPreset p;
  double values[kAll];
  for (int i = 0; i < kAll; ++i) values[i] = GetParam(i)->Value();
  magmora::setPresetSnapshot(p, values);
  p.hasCabFiles = true;
  std::lock_guard<std::mutex> lock(mChainMutex);
  RefreshCabView();
  const CabView& v = mCabView;
  if (!v.pack.empty()) {
    p.cab.pack.present = true;
    p.cab.pack.path = v.pack;
    p.cab.pack.name = builtin_cab::IsReference(v.pack) ? std::string(builtin_cab::kName) : BaseName(v.pack);
    p.cab.pack.mapping = v.mapping;
  }
  for (int m = 0; m < 2; ++m) {
    const int type = static_cast<int>(std::lround(GetParam(m == 0 ? magmora::kMic1Type : magmora::kMic2Type)->Value()));
    if (mChain.cabinetPackLoaded() && type < mChain.cabinetMicCount()) p.cab.mic[m] = mChain.cabinetMicName(type);
    p.cab.speaker[m] = mChain.cabinetMicSpeaker(m);
    p.cab.variant[m] = mChain.cabinetMicVariant(m);
    const magmora::ui::CabIrInfo& ir = v.ir[m];
    if (ir.reference.empty()) continue;
    p.cab.ir[m].present = true;
    p.cab.ir[m].path = ir.reference;
    p.cab.ir[m].name = BaseName(ir.reference);
    p.cab.ir[m].normalize = ir.normalize;
    p.cab.ir[m].bytes = user_presets::SizeOf(ir.reference);
  }
  return p;
}

// A preset's values, one gesture per changed parameter (as a factory preset's).
void Magmora::ApplyUserValues(const magmora::UserPreset& preset) {
  for (int i = 0; i < kAll; ++i)
    if (preset.given[i]) SetParameterFromUI(i, preset.values[i]);
}

// The pack, the microphones by name, then the slots' files, each found where it was saved, next to the preset, or in
// the last folder a file was loaded from. What cannot be found keeps its reference, shows as missing, and is written
// back by a later save. mChainMutex held.
void Magmora::ApplyCabFiles(const magmora::UserPreset& preset, const std::string& presetPath) {
  const std::string stem = user_presets::ToUtf8(user_presets::FromUtf8(presetPath).stem());
  std::vector<std::string> missing;
  const magmora::PresetFileRef& pack = preset.cab.pack;
  mCabMessage.clear();
  mPackMissing = false;
  if (!pack.present) {
    mChain.clearCabinetPack();
  } else if (builtin_cab::IsReference(pack.path)) {
    std::string error;
    if (!(mChain.cabinetPackLoaded() && builtin_cab::IsReference(mChain.cabinetPackReference())) &&
        !builtin_cab::Load(mChain, &error))
      mCabMessage = error;
  } else {
    const std::string folder = user_presets::Locate(pack.path, pack.name, presetPath, stem, "", true);
    std::string mapping = pack.mapping;
    if (!folder.empty() && !mapping.empty() && !user_presets::Exists(mapping)) {
      const std::string moved = user_presets::Locate("", BaseName(mapping), folder + "/x", "", "", false);
      if (!moved.empty()) mapping = moved;
    }
    std::string error;
    const bool already = mChain.cabinetPackLoaded() && folder == mChain.cabinetPackReference() &&
                         mapping == mChain.cabinetMappingReference();
    if (folder.empty() ||
        (!already && !mChain.loadCabinetPack(folder.c_str(), &error, mapping.empty() ? nullptr : mapping.c_str()))) {
      mChain.clearCabinetPack();
      mPackMissing = true;
      mMissingFolder = pack.path;
      mMissingMapping = pack.mapping;
      mCabMessage = "Could not load the pack " + pack.path + (error.empty() ? "" : ": " + error);
      missing.push_back(pack.name.empty() ? BaseName(pack.path) : pack.name);
    }
  }
  for (int m = 0; m < 2; ++m) {
    const int type = m == 0 ? magmora::kMic1Type : magmora::kMic2Type;
    if (mChain.cabinetPackLoaded() && !preset.cab.mic[m].empty())
      for (int i = 0; i < mChain.cabinetMicCount(); ++i)
        if (mChain.cabinetMicName(i) == preset.cab.mic[m]) {
          SetParameterFromUI(type, i);
          break;
        }
    mChain.setCabinetMicSource(m, preset.cab.speaker[m].c_str(), preset.cab.variant[m].c_str());

    const magmora::PresetFileRef& ir = preset.cab.ir[m];
    mIrMessage[m].clear();
    if (!ir.present) {
      mChain.clearCabinetIr(m);
      continue;
    }
    const std::string found = user_presets::Locate(ir.path, ir.name, presetPath, stem, mLastIrFolder, false);
    std::string error;
    const bool already = !found.empty() && mChain.cabinetIrLoaded(m) && found == mChain.cabinetIrReference(m);
    if (!found.empty() && (already || mChain.loadCabinetIr(m, found.c_str(), &error))) {
      mChain.setCabinetIrNormalize(m, ir.normalize);
      if (ir.bytes >= 0 && user_presets::SizeOf(found) != ir.bytes) mIrMessage[m] = "Size differs from the saved file";
      continue;
    }
    mChain.clearCabinetIr(m);
    mChain.setCabinetIrReference(m, ir.path.c_str());  // kept: a save writes it back
    mChain.setCabinetIrNormalize(m, ir.normalize);
    mIrMessage[m] = found.empty() ? "Missing: " + (ir.name.empty() ? BaseName(ir.path) : ir.name) : "Could not load " + found + ": " + error;
    missing.push_back(ir.name.empty() ? BaseName(ir.path) : ir.name);
  }
  RefreshCabView();
  std::lock_guard<std::mutex> lock(mUserMutex);
  mUser.missing = std::move(missing);
}

std::string Magmora::LoadUserPreset(const std::string& path) {
  const user_presets::fs::path file = user_presets::FromUtf8(path);
  if (!user_presets::Inside(file)) return "not in the presets folder";
  magmora::UserPreset preset;
  std::string error;
  if (!user_presets::ReadPreset(file, preset, &error)) return error;  // nothing has changed
  ApplyUserValues(preset);
  // The edit mark compares with the file as it resolved: the same values, and the references now in use.
  magmora::UserPreset compare = preset;
  SetUserPreset(preset.name, path, nullptr);
  if (preset.hasCabFiles) {
    std::lock_guard<std::mutex> lock(mChainMutex);
    ApplyCabFiles(preset, path);
    for (int m = 0; m < 2; ++m)
      if (compare.cab.ir[m].present) compare.cab.ir[m].path = mCabView.ir[m].reference;
  }
  {
    std::lock_guard<std::mutex> lock(mUserMutex);
    mUser.compare = true;
    mUser.preset = std::move(compare);
  }
  StructureMayHaveChanged();
  return {};
}

bool Magmora::PresetEdited() const {
  {
    std::lock_guard<std::mutex> lock(mUserMutex);
    if (mUser.active) {
      if (!mUser.compare) return true;  // its file is gone, or not read yet
      const magmora::UserPreset& p = mUser.preset;
      for (int i = 0; i < kAll; ++i)
        if (p.given[i] && std::fabs(GetParam(i)->Value() - p.values[i]) > 1e-4) return true;
      if (!p.hasCabFiles) return false;
      if (p.cab.pack.present != !mCabView.pack.empty()) return true;
      if (p.cab.pack.present && p.cab.pack.path != mCabView.pack && BaseName(mCabView.pack) != p.cab.pack.name) return true;
      for (int m = 0; m < 2; ++m) {
        const magmora::PresetFileRef& ir = p.cab.ir[m];
        const magmora::ui::CabIrInfo& now = mCabView.ir[m];
        if (ir.present != !now.reference.empty()) return true;
        if (!ir.present) continue;
        if ((ir.path != now.reference && BaseName(now.reference) != ir.name) || ir.normalize != now.normalize) return true;
      }
      return false;
    }
  }
  const PresetDef& preset = kPresets[PresetIndex()];
  for (int i = 0; i < kAll; ++i)
    if (preset.given[i] && std::fabs(GetParam(i)->Value() - preset.values[i]) > 1e-4) return true;  // AU has floats
  return false;
}

std::string Magmora::WriteUserPreset(const std::string& path, magmora::UserPreset preset) {
  std::string error;
  if (!user_presets::Inside(user_presets::FromUtf8(path))) return "not in the presets folder";
  if (!user_presets::WriteAtomic(user_presets::FromUtf8(path), magmora::writeUserPreset(preset), &error)) return error;
  SetUserPreset(preset.name, path, &preset);
  return {};
}

std::string Magmora::SaveUserPreset(const std::string& wanted, const std::string& folder, bool replace, bool* exists) {
  if (exists) *exists = false;
  const std::string name = magmora::cleanPresetName(wanted);
  if (name.empty()) return "Give the preset a name";
  if (FactoryName(name)) return "Factory names are reserved";
  const user_presets::fs::path root = user_presets::Root();
  if (root.empty()) return "no presets folder on this system";
  user_presets::fs::path dir = root;
  if (!folder.empty()) {
    const std::vector<std::string> folders = user_presets::Folders();
    if (std::find(folders.begin(), folders.end(), folder) == folders.end()) return "no such folder";
    dir /= user_presets::FromUtf8(folder);
  }
  const std::string path = user_presets::ToUtf8(dir / user_presets::FromUtf8(magmora::presetFileStem(name) + ".json"));
  if (user_presets::Exists(path) && !replace) {
    if (exists) *exists = true;
    return {};
  }
  magmora::UserPreset preset = Snapshot();
  preset.name = name;
  return WriteUserPreset(path, std::move(preset));
}

std::string Magmora::SaveCurrentUserPreset() {
  std::string name, path;
  {
    std::lock_guard<std::mutex> lock(mUserMutex);
    if (!mUser.active) return "a factory preset is not overwritten";
    name = mUser.name;
    path = user_presets::Absolute(mUser.relative);
  }
  if (path.empty()) return "the preset's file is not in the presets folder";
  magmora::UserPreset preset = Snapshot();
  preset.name = name;
  return WriteUserPreset(path, std::move(preset));
}

std::string Magmora::RenameUserPreset(const std::string& path, const std::string& wanted, bool replace, bool* exists) {
  if (exists) *exists = false;
  const user_presets::fs::path from = user_presets::FromUtf8(path);
  if (!user_presets::Inside(from)) return "not in the presets folder";
  const std::string name = magmora::cleanPresetName(wanted);
  if (name.empty()) return "Give the preset a name";
  if (FactoryName(name)) return "Factory names are reserved";
  magmora::UserPreset preset;
  std::string error;
  if (!user_presets::ReadPreset(from, preset, &error)) return error;
  const std::string to = user_presets::ToUtf8(from.parent_path() / user_presets::FromUtf8(magmora::presetFileStem(name) + ".json"));
  const bool same = user_presets::FromUtf8(to).lexically_normal() == from.lexically_normal();
  if (!same && user_presets::Exists(to) && !replace) {
    if (exists) *exists = true;
    return {};
  }
  preset.name = name;
  // The new file first, then the old one goes: a failure leaves the preset where it was.
  if (!user_presets::WriteAtomic(user_presets::FromUtf8(to), magmora::writeUserPreset(preset), &error)) return error;
  std::error_code ec;
  if (!same) user_presets::fs::remove(from, ec);
  std::lock_guard<std::mutex> lock(mUserMutex);
  if (mUser.active && user_presets::Absolute(mUser.relative) == user_presets::ToUtf8(from.lexically_normal())) {
    mUser.name = name;
    mUser.relative = user_presets::Relative(to);
    if (mUser.compare) mUser.preset.name = name;
  }
  return {};
}

std::string Magmora::DeleteUserPreset(const std::string& path) {
  const user_presets::fs::path file = user_presets::FromUtf8(path);
  if (!user_presets::Inside(file) || !user_presets::Exists(path)) return "not in the presets folder";
  std::error_code ec;
  if (!user_presets::fs::remove(file, ec) || ec) return "could not delete the file";
  // Deleting the preset in use keeps the sound: its name stays, marked as edited (nothing on disk holds it now).
  std::lock_guard<std::mutex> lock(mUserMutex);
  if (mUser.active && user_presets::Absolute(mUser.relative) == user_presets::ToUtf8(file.lexically_normal())) mUser.compare = false;
  return {};
}

std::string Magmora::ImportUserPreset(const std::string& file, std::string* imported) {
  std::string text, error;
  magmora::UserPreset preset;
  if (!user_presets::ReadText(user_presets::FromUtf8(file), text, &error) ||
      !magmora::readUserPreset(text.data(), text.size(), preset, &error))
    return error;
  const user_presets::fs::path root = user_presets::Root();
  if (root.empty()) return "no presets folder on this system";
  // A name of its own: never over a factory name or an existing file.
  std::string name = preset.name;
  for (int k = 2; FactoryName(name) ||
                  user_presets::Exists(user_presets::ToUtf8(root / user_presets::FromUtf8(magmora::presetFileStem(name) + ".json")));
       ++k)
    name = magmora::cleanPresetName(preset.name.substr(0, magmora::kMaxPresetNameBytes - 4)) + " " + std::to_string(k);
  preset.name = name;
  const user_presets::fs::path to = root / user_presets::FromUtf8(magmora::presetFileStem(name) + ".json");
  // Written in this version's format: the values as read (validated and clamped), unknown keys dropped.
  if (!user_presets::WriteAtomic(to, magmora::writeUserPreset(preset), &error)) return error;
  if (imported) *imported = name;
  return {};
}

std::string Magmora::UserPresetFolder(bool create) {
  const user_presets::fs::path root = user_presets::Root();
  std::error_code ec;
  if (create && !root.empty()) user_presets::fs::create_directories(root, ec);
  return user_presets::ToUtf8(root);
}

void Magmora::StepPreset(int by) {
  const std::vector<user_presets::Entry> users = user_presets::Scan();
  const int total = kNumPresets + static_cast<int>(users.size());
  int at = PresetIndex();
  {
    const std::string current = UserPresetPath();
    if (UserPresetLoaded()) {
      at = kNumPresets - 1;  // a deleted one: from the end of the factory list
      for (size_t i = 0; i < users.size(); ++i)
        if (users[i].path == current) at = kNumPresets + static_cast<int>(i);
    }
  }
  // A file that cannot be read any more is passed over.
  for (int tries = 0; tries < total; ++tries) {
    at = ((at + by) % total + total) % total;
    if (at < kNumPresets) {
      ChoosePreset(at);
      return;
    }
    if (LoadUserPreset(users[static_cast<size_t>(at - kNumPresets)].path).empty()) return;
  }
}

std::string Magmora::StepCabIr(int slot, int by) {
  if (slot < 0 || slot > 1) return "no such slot";
  const std::string current = mCabView.ir[slot].reference;
  if (current.empty()) return "no file loaded";
  const std::vector<std::string> files = user_presets::WavFiles(user_presets::FromUtf8(current).parent_path());
  if (files.empty()) return "no .wav files in its folder";
  int at = -1;
  for (size_t i = 0; i < files.size(); ++i)
    if (user_presets::FromUtf8(files[i]).lexically_normal() == user_presets::FromUtf8(current).lexically_normal()) at = static_cast<int>(i);
  const int n = static_cast<int>(files.size());
  // From a missing file: the first or last; then each one that will not load is passed over.
  if (at < 0) at = by > 0 ? -1 : n;
  for (int tries = 0; tries < n; ++tries) {
    at = ((at + by) % n + n) % n;
    if (LoadCabIr(slot, files[static_cast<size_t>(at)]).empty()) return {};
  }
  return "no file in the folder could be loaded";
}

// ---- tuner -------------------------------------------------------------------------------------------------------

void Magmora::SetTuner(bool on, bool mute) {
  mTunerOn.store(on);
  mTunerMute.store(on && mute);
}

bool Magmora::TunerSamples(float* out, int n, double& rate) {
  rate = mTuner.rate();
  return rate > 0. && mTuner.latest(out, n);
}

// ---- stand-alone ---------------------------------------------------------------------------------------------------

bool Magmora::CanShowAudioSettings() const {
#if defined APP_API && defined OS_MAC
  return true;
#else
  return false;
#endif
}

void Magmora::ShowAudioSettings() {
#if defined APP_API && defined OS_MAC
  // The app's own Preferences dialog (also in its menu), posted: it is modal, so it opens after this click is handled.
  if (gHWND) PostMessage(gHWND, WM_COMMAND, ID_PREFERENCES, 0);
#endif
}

// ---- editor ------------------------------------------------------------------------------------------------------

magmora::ui::Levels Magmora::TakeLevels() {
  magmora::ui::Levels levels;
  levels.inputPeak = mInputPeak.exchange(0.f, std::memory_order_relaxed);
  levels.outputPeak = mOutputPeak.exchange(0.f, std::memory_order_relaxed);
  levels.gateGain = mChain.meters().gateGain.load(std::memory_order_relaxed);
  levels.powerW = mChain.meters().platePowerW.load(std::memory_order_relaxed);
  return levels;
}

void Magmora::OnUIOpen() {
  iplug::Plugin::OnUIOpen();
  mUiAlive = std::make_shared<std::atomic<bool>>(true);  // a new token per editor: one closed stays closed
  std::lock_guard<std::mutex> lock(mChainMutex);
  RefreshCabView();
}

void Magmora::OnUIClose() {
  SetTuner(false, false);  // the tuner and its mute live only while the editor shows them
  if (mUiAlive) mUiAlive->store(false);  // file dialogs still open from this editor now do nothing
  if (auto* ui = GetUI()) {
    mScale.store(ui->GetDrawScale());
    magmora::ui::Closing(*ui);
  }
}

bool Magmora::OnHostRequestingAboutBox() {
  auto* ui = GetUI();
  if (ui) magmora::ui::ShowAbout(*ui);
  return ui != nullptr;
}

void Magmora::OnParentWindowResize(int width, int height) {
  auto* ui = GetUI();
  if (!ui || width <= 0 || height <= 0) return;
  float pixels = 1.f;
#if defined OS_WIN && defined VST3_API
  pixels = ui->GetScreenScale();
#endif
  const float w = static_cast<float>(width) / pixels, h = static_cast<float>(height) / pixels;
  // Whole points cannot express every scale exactly: ignore our own size coming back rounded.
  if (std::fabs(w - static_cast<float>(ui->WindowWidth())) <= 2.f && std::fabs(h - static_cast<float>(ui->WindowHeight())) <= 2.f)
    return;
  const float scale = std::min(w / PLUG_WIDTH, h / PLUG_HEIGHT);
  SetEditorSize(width, height);
  ui->Resize(PLUG_WIDTH, PLUG_HEIGHT, scale, false);
}

// ---- VST3 --------------------------------------------------------------------------------------------------------

#if defined VST3_API

using namespace Steinberg;

namespace {
// What the host sent, less the bypass parameter: iPlug2 must never take its own pass-through path.
class WithoutBypass final : public Vst::IParameterChanges {
public:
  explicit WithoutBypass(Vst::IParameterChanges* all) : mAll(all) {}

  tresult PLUGIN_API queryInterface(const TUID, void** object) override {
    if (object) *object = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }

  int32 PLUGIN_API getParameterCount() override { return mAll->getParameterCount(); }
  Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
    Vst::IParamValueQueue* queue = mAll->getParameterData(index);
    return queue && queue->getParameterId() == static_cast<Vst::ParamID>(iplug::kBypassParam) ? nullptr : queue;
  }
  Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id, int32& index) override {
    return mAll->addParameterData(id, index);
  }

private:
  Vst::IParameterChanges* mAll;
};
}  // namespace

tresult PLUGIN_API Magmora::initialize(FUnknown* context) {
  const tresult result = IPlugVST3::initialize(context);
  if (result != kResultOk) return result;
  // Parameters kept only for compatibility (Chain::parameterHidden): hidden from the host, never written by it.
  for (int id = 0; id < kAll; ++id)
    if (magmora::Chain::parameterHidden(id))
      if (Vst::Parameter* p = parameters.getParameter(static_cast<Vst::ParamID>(id)))
        p->getInfo().flags = (p->getInfo().flags & ~Vst::ParameterInfo::kCanAutomate) | Vst::ParameterInfo::kIsHidden |
                             Vst::ParameterInfo::kIsReadOnly;
  return result;
}

tresult PLUGIN_API Magmora::process(Vst::ProcessData& data) {
  Vst::ProcessData passed = data;
  WithoutBypass changes(data.inputParameterChanges);
  if (data.inputParameterChanges) {
    for (int32 i = 0, n = data.inputParameterChanges->getParameterCount(); i < n; ++i) {
      Vst::IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
      if (!queue || queue->getParameterId() != static_cast<Vst::ParamID>(iplug::kBypassParam)) continue;
      int32 offset = 0;
      Vst::ParamValue value = 0.;
      const int32 points = queue->getPointCount();
      if (points > 0 && queue->getPoint(points - 1, offset, value) == kResultTrue) mBypass.store(value > 0.5);
    }
    passed.inputParameterChanges = &changes;
  }
  if (data.numSamples <= 0) {
    // A parameter flush: hosts send it without buffers, which iPlug2 would dereference.
    passed.numInputs = 0;
    passed.numOutputs = 0;
  }
  mNoInput = passed.numInputs <= 0 || !passed.inputs || passed.inputs[0].numChannels <= 0;
  for (int32 bus = 0; bus < passed.numOutputs; ++bus) passed.outputs[bus].silenceFlags = 0;
  return IPlugVST3::process(passed);
}

tresult PLUGIN_API Magmora::setupProcessing(Vst::ProcessSetup& setup) {
  // iPlug2 learns of offline rendering only inside process(); Ultra must be in place before the first block.
  SetRenderingOffline(setup.processMode == Vst::kOffline);
  mOffline.store(setup.processMode == Vst::kOffline);
  return IPlugVST3::setupProcessing(setup);
}

tresult PLUGIN_API Magmora::setParamNormalized(Vst::ParamID tag, Vst::ParamValue v) {
  if (tag < static_cast<Vst::ParamID>(kAll)) {
    // iPlug2 stores the raw value for the host and the stepped one for itself.
    if (Info(static_cast<int>(tag)).taper == magmora::Taper::Stepped)
      v = GetParam(static_cast<int>(tag))->ToNormalized(GetParam(static_cast<int>(tag))->FromNormalized(v));
  } else if (tag == static_cast<Vst::ParamID>(iplug::kBypassParam)) {
    mBypass.store(v > 0.5);
  }
  return IPlugVST3::setParamNormalized(tag, v);
}

tresult PLUGIN_API Magmora::getState(IBStream* stream) {
  if (!stream) return kInvalidArgument;
  iplug::IByteChunk chunk;
  if (!SerializeState(chunk)) return kResultFalse;
  const int32 bypass = mBypass.load() ? 1 : 0;
  chunk.Put(&bypass);
  int32 written = 0;
  if (stream->write(chunk.GetData(), chunk.Size(), &written) != kResultOk || written != chunk.Size()) return kResultFalse;
  return kResultOk;
}

tresult PLUGIN_API Magmora::setState(IBStream* stream) {
  if (!stream) return kInvalidArgument;
  iplug::IByteChunk chunk;
  for (;;) {
    char buffer[512];
    int32 got = 0;
    // The bytes delivered count, not the status: WaveLab's streams report a non-OK status with data (iPlug2's own
    // IPlugVST3State::SetState makes the same allowance). The end is a read that delivers nothing.
    stream->read(buffer, sizeof buffer, &got);
    if (got <= 0 || got > static_cast<int32>(sizeof buffer)) break;
    chunk.PutBytes(buffer, got);
    if (chunk.Size() > kMaxBody) break;  // no state is this large: what was read is refused below
  }
  const int end = UnserializeState(chunk, 0);
  if (end <= 0) return kResultFalse;
  int32 bypass = 0;
  Reader tail(chunk, end);
  if (!tail.Get(bypass)) bypass = 0;
  mBypass.store(bypass != 0);
  UpdateParams(this, bypass != 0);
  return kResultOk;
}

class MagmoraView final : public IPlugVST3View<iplug::IPlugVST3> {
  using Base = IPlugVST3View<iplug::IPlugVST3>;

public:
  explicit MagmoraView(Magmora& plug) : Base(plug), mPlug(plug) {}
  ~MagmoraView() { Forget(); }

  tresult PLUGIN_API attached(void* parent, FIDString type) override {
    mPlug.mEditorView = this;  // opening may already ask for a resize
    return Base::attached(parent, type);
  }

  tresult PLUGIN_API removed() override {
    const tresult result = Base::removed();
    Forget();
    return result;
  }

  tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override {
    if (!rect) return kInvalidArgument;
    float pixels = 1.f;
#if defined OS_WIN
    if (auto* ui = mPlug.GetUI()) pixels = ui->GetScreenScale();
#endif
    const float baseW = PLUG_WIDTH * pixels, baseH = PLUG_HEIGHT * pixels;
    const auto width = static_cast<float>(rect->getWidth()), height = static_cast<float>(rect->getHeight());
    const float byWidth = width / baseW, byHeight = height / baseH;
    // The edge being dragged leads, so that either can be.
    const float movedW = std::fabs(width - static_cast<float>(mPlug.GetEditorWidth())) / baseW;
    const float movedH = std::fabs(height - static_cast<float>(mPlug.GetEditorHeight())) / baseH;
    const float scale = std::min(std::max(movedW >= movedH ? byWidth : byHeight, magmora::ui::kMinScale),
                                 magmora::ui::kMaxScale);
    rect->right = rect->left + static_cast<int32>(std::lround(baseW * scale));
    rect->bottom = rect->top + static_cast<int32>(std::lround(baseH * scale));
    return kResultTrue;
  }

  void RequestSize(int width, int height) {
    if (!plugFrame) return;
    ViewRect size(0, 0, width, height);
    plugFrame->resizeView(this, &size);
  }

private:
  void Forget() {
    if (mPlug.mEditorView == this) mPlug.mEditorView = nullptr;
  }

  Magmora& mPlug;
};

IPlugView* PLUGIN_API Magmora::createView(const char* name) {
  return HasUI() && name && std::strcmp(name, "editor") == 0 ? new MagmoraView(*this) : nullptr;
}

bool Magmora::EditorResize(int viewWidth, int viewHeight) {
  const bool changed = viewWidth != GetEditorWidth() || viewHeight != GetEditorHeight();
  SetEditorSize(viewWidth, viewHeight);
  if (changed && mEditorView) mEditorView->RequestSize(viewWidth, viewHeight);
  return true;
}

#endif  // VST3_API
