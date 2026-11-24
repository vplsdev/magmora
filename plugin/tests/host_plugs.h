// Two minimal hosts for the test harness, VST3 and AUv2, behind one interface. Nothing is installed: the AU is
// registered with AudioComponentRegister, which lasts as long as this process.
#pragma once

#import <AppKit/AppKit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include <magmora/Chain.h>

extern "C" void magmora_test_rendering(int on);

namespace host {

// While set, allocations inside the plug-in's own render call are counted (no_cursor.c); the harness's are not.
inline bool gCountAllocations = false;

inline const magmora::ParamInfo& PI(int id) { return *magmora::Chain::parameterInfo(id); }
constexpr int kAllParams = magmora::kNumChainParams;

using Bytes = std::vector<char>;
using Audio = std::vector<float>;
constexpr int kBypassId = -1;

struct Event {
  char what;  // 'b'egin, 'v'alue, 'e'nd
  int id;
  double value;  // normalised
};

struct Format {
  double rate = 48000.;
  int block = 64, in = 1, out = 2;
  bool offline = false;
};

[[noreturn]] inline void Die(const std::string& why) {
  fprintf(stderr, "harness: %s\n", why.c_str());
  exit(2);
}

class Plug {
public:
  virtual ~Plug() = default;
  virtual bool Setup(const Format&) = 0;
  virtual void Stop() = 0;
  virtual void Reset() = 0;
  // Normalised. `onlyInProcess`: automation that reaches the processor alone, as some hosts send it.
  virtual void Set(int id, double v, bool onlyInProcess = false) = 0;
  virtual double Get(int id) = 0;
  virtual std::string Text(int id, double v) = 0;
  virtual bool Automatable(int id) = 0;
  virtual void Render(const float* const* in, float* const* out, int n, bool inPlace = false) = 0;
  virtual int Latency() = 0;
  virtual int Tail() = 0;  // samples
  virtual bool Save(Bytes&) = 0;
  virtual bool Load(const Bytes&) = 0;
  // A state as particular hosts hand it over. VST3: through a stream that reports a non-OK status although it delivers
  // the bytes (WaveLab). AU: in a dictionary that names a preset (`name`; the factory presets' names are iPlug2's).
  virtual bool LoadQuirky(const Bytes& b, const char* name = nullptr) = 0;
  virtual NSView* OpenView(NSView* parent) = 0;
  virtual void CloseView() = 0;

  std::vector<Event> events;
  int latencyNotices = 0;
  Format format;
  std::function<void(int, int)> resized;

  // Plain units through the format's own mapping (VST3 normalises frequencies and Qs logarithmically).
  virtual double ToPlain(int id, double normalised) {
    const magmora::ParamInfo& p = PI(id);
    return p.min + normalised * (p.max - p.min);
  }
  virtual double ToNormalised(int id, double plain) {
    const magmora::ParamInfo& p = PI(id);
    return (plain - p.min) / (p.max - p.min);
  }
  double Plain(int id) { return ToPlain(id, Get(id)); }
  void SetPlain(int id, double v, bool onlyInProcess = false) { Set(id, ToNormalised(id, v), onlyInProcess); }

  // All of `in` through the plug-in in blocks of the prepared size.
  std::vector<Audio> Run(const std::vector<Audio>& in, int block = 0, bool inPlace = false,
                         const std::function<void(int)>& beforeBlock = nullptr) {
    const int total = static_cast<int>(in[0].size()), size = block ? block : format.block;
    std::vector<Audio> out(format.out, Audio(total));
    for (int at = 0; at < total; at += size) {
      const float* i[2] = {in[0].data() + at, (in.size() > 1 ? in[1] : in[0]).data() + at};
      float* o[2] = {out[0].data() + at, out[format.out > 1 ? 1 : 0].data() + at};
      if (beforeBlock) beforeBlock(at);
      Render(i, o, std::min(size, total - at), inPlace);
    }
    return out;
  }
};

// ---- VST3 ------------------------------------------------------------------------------------------------------

using namespace Steinberg;

class V3 final : public Plug, public Vst::IComponentHandler, public IPlugFrame {
public:
  explicit V3(const std::string& bundle) {
    static CFBundleRef loaded = nullptr;
    static IPluginFactory* factory = nullptr;
    if (!loaded) {
      CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(bundle.c_str()),
                                                             bundle.size(), true);
      loaded = CFBundleCreate(nullptr, url);
      if (!loaded || !CFBundleLoadExecutable(loaded)) Die("cannot load " + bundle);
      auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(CFBundleGetFunctionPointerForName(loaded, CFSTR("bundleEntry")));
      auto get = reinterpret_cast<IPluginFactory* (*)()>(CFBundleGetFunctionPointerForName(loaded, CFSTR("GetPluginFactory")));
      if (!entry || !entry(loaded) || !get) Die("not a VST3");
      factory = get();
    }
    PClassInfo info;
    for (int32 i = 0; i < factory->countClasses() && !mComponent; ++i) {
      factory->getClassInfo(i, &info);
      if (std::strcmp(info.category, kVstAudioEffectClass) == 0)
        factory->createInstance(info.cid, Vst::IComponent::iid, reinterpret_cast<void**>(&mComponent));
    }
    if (!mComponent || mComponent->initialize(nullptr) != kResultOk) Die("cannot create the component");
    mComponent->queryInterface(Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&mProcessor));
    mComponent->queryInterface(Vst::IEditController::iid, reinterpret_cast<void**>(&mController));
    if (!mProcessor || !mController) Die("not a single component effect");
    mController->setComponentHandler(static_cast<Vst::IComponentHandler*>(this));
    for (int32 i = 0; i < mController->getParameterCount(); ++i) {
      Vst::ParameterInfo p{};
      mController->getParameterInfo(i, p);
      mInfo[p.id] = p;
      if (p.flags & Vst::ParameterInfo::kIsBypass) mBypassId = p.id;
    }
  }

  ~V3() override {
    CloseView();
    Stop();
    mController->setComponentHandler(nullptr);
    mComponent->terminate();
    mProcessor->release();
    mController->release();
    mComponent->release();
  }

  bool Setup(const Format& f) override {
    Stop();
    format = f;
    Vst::SpeakerArrangement in = f.in == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
    Vst::SpeakerArrangement out = f.out == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
    if (mProcessor->setBusArrangements(&in, 1, &out, 1) != kResultTrue) return false;
    Vst::ProcessSetup setup{f.offline ? Vst::kOffline : Vst::kRealtime, Vst::kSample32, f.block, f.rate};
    if (mProcessor->setupProcessing(setup) != kResultOk) return false;
    mComponent->activateBus(Vst::kAudio, Vst::kInput, 0, true);
    mComponent->activateBus(Vst::kAudio, Vst::kOutput, 0, true);
    if (mComponent->setActive(true) != kResultOk) return false;
    mProcessor->setProcessing(true);
    mActive = true;
    for (auto& s : mScratch) s.assign(f.block, 0.f);
    return true;
  }

  void Stop() override {
    if (!mActive) return;
    mProcessor->setProcessing(false);
    mComponent->setActive(false);
    mActive = false;
  }

  void Reset() override {
    mProcessor->setProcessing(false);
    mProcessor->setProcessing(true);
  }

  void Set(int id, double v, bool onlyInProcess) override {
    const Vst::ParamID pid = id == kBypassId ? mBypassId : id;
    mPending.emplace_back(pid, v);
    if (!onlyInProcess) mController->setParamNormalized(pid, v);
  }

  double Get(int id) override { return mController->getParamNormalized(id == kBypassId ? mBypassId : id); }

  std::string Text(int id, double v) override {
    Vst::String128 s{};
    mController->getParamStringByValue(id, v, s);
    return [[NSString stringWithCharacters:reinterpret_cast<const unichar*>(s) length:Length(s)] UTF8String];
  }

  bool Automatable(int id) override { return mInfo[id].flags & Vst::ParameterInfo::kCanAutomate; }
  bool Hidden(int id) { return mInfo[id].flags & Vst::ParameterInfo::kIsHidden; }
  double ToPlain(int id, double normalised) override { return mController->normalizedParamToPlain(id, normalised); }
  double ToNormalised(int id, double plain) override { return mController->plainParamToNormalized(id, plain); }

  void Render(const float* const* in, float* const* out, int n, bool inPlace) override {
    Vst::ParameterChanges changes(32);
    for (const auto& change : mPending) {
      int32 index = 0, point = 0;
      const Vst::ParamID id = change.first;
      if (auto* queue = changes.addParameterData(id, index)) queue->addPoint(0, change.second, point);
    }
    mPending.clear();
    float* i[2] = {const_cast<float*>(in[0]), const_cast<float*>(in[format.in > 1 ? 1 : 0])};
    float* o[2] = {out[0], out[format.out > 1 ? 1 : 0]};
    if (inPlace) {
      for (int c = 0; c < format.in && c < format.out; ++c) {
        std::copy(i[c], i[c] + n, o[c]);
        i[c] = o[c];
      }
    }
    Vst::AudioBusBuffers inBus, outBus;
    inBus.numChannels = format.in;
    inBus.channelBuffers32 = i;
    outBus.numChannels = format.out;
    outBus.channelBuffers32 = o;
    outBus.silenceFlags = 3;
    Vst::ProcessData data;
    data.processMode = format.offline ? Vst::kOffline : Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = n;
    if (n) {
      data.numInputs = data.numOutputs = 1;
      data.inputs = &inBus;
      data.outputs = &outBus;
    }
    data.inputParameterChanges = &changes;
    if (gCountAllocations) magmora_test_rendering(1);
    mProcessor->process(data);
    if (gCountAllocations) magmora_test_rendering(0);
  }

  int Latency() override { return static_cast<int>(mProcessor->getLatencySamples()); }
  int Tail() override { return static_cast<int>(mProcessor->getTailSamples()); }

  bool Save(Bytes& b) override {
    MemoryStream s;
    if (mComponent->getState(&s) != kResultOk) return false;
    b.assign(s.getData(), s.getData() + s.getSize());
    return true;
  }

  bool Load(const Bytes& b) override {
    MemoryStream s(const_cast<char*>(b.data()), static_cast<TSize>(b.size()));
    if (mComponent->setState(&s) != kResultOk) return false;
    s.seek(0, IBStream::kIBSeekSet, nullptr);
    mController->setComponentState(&s);
    return true;
  }

  bool LoadQuirky(const Bytes& b, const char*) override {
    struct Quirky final : MemoryStream {
      using MemoryStream::MemoryStream;
      tresult PLUGIN_API read(void* buffer, int32 n, int32* got) override {
        const tresult r = MemoryStream::read(buffer, n, got);
        return r == kResultOk && got && *got > 0 ? kResultFalse : r;  // the data, and a non-OK status
      }
    };
    Quirky s(const_cast<char*>(b.data()), static_cast<TSize>(b.size()));
    if (mComponent->setState(&s) != kResultOk) return false;
    MemoryStream plain(const_cast<char*>(b.data()), static_cast<TSize>(b.size()));
    mController->setComponentState(&plain);
    return true;
  }

  NSView* OpenView(NSView* parent) override {
    mView = mController->createView("editor");
    if (!mView) return nil;
    mView->setFrame(static_cast<IPlugFrame*>(this));
    if (mView->attached((__bridge void*)parent, kPlatformTypeNSView) != kResultTrue) return nil;
    return parent.subviews.lastObject;
  }

  void CloseView() override {
    if (!mView) return;
    mView->removed();
    mView->setFrame(nullptr);
    mView->release();
    mView = nullptr;
  }

  IPlugView* View() { return mView; }

  // IComponentHandler: what a host records
  tresult PLUGIN_API beginEdit(Vst::ParamID id) override {
    events.push_back({'b', static_cast<int>(id), 0.});
    return kResultOk;
  }
  tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue v) override {
    events.push_back({'v', static_cast<int>(id), v});
    mPending.emplace_back(id, v);
    return kResultOk;
  }
  tresult PLUGIN_API endEdit(Vst::ParamID id) override {
    events.push_back({'e', static_cast<int>(id), 0.});
    return kResultOk;
  }
  tresult PLUGIN_API restartComponent(int32 flags) override {
    if (flags & Vst::kLatencyChanged) ++latencyNotices;
    return kResultOk;
  }

  // IPlugFrame
  tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
    if (resized) resized(r->getWidth(), r->getHeight());
    view->onSize(r);
    return kResultTrue;
  }

  tresult PLUGIN_API queryInterface(const TUID id, void** obj) override {
    if (FUnknownPrivate::iidEqual(id, Vst::IComponentHandler::iid))
      *obj = static_cast<Vst::IComponentHandler*>(this);
    else if (FUnknownPrivate::iidEqual(id, IPlugFrame::iid))
      *obj = static_cast<IPlugFrame*>(this);
    else if (FUnknownPrivate::iidEqual(id, FUnknown::iid))
      *obj = static_cast<Vst::IComponentHandler*>(this);
    else {
      *obj = nullptr;
      return kNoInterface;
    }
    return kResultOk;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }

private:
  static NSUInteger Length(const Vst::TChar* s) {
    NSUInteger n = 0;
    while (n < 128 && s[n]) ++n;
    return n;
  }

  Vst::IComponent* mComponent = nullptr;
  Vst::IAudioProcessor* mProcessor = nullptr;
  Vst::IEditController* mController = nullptr;
  IPlugView* mView = nullptr;
  std::map<Vst::ParamID, Vst::ParameterInfo> mInfo;
  std::vector<std::pair<Vst::ParamID, double>> mPending;
  Vst::ParamID mBypassId = 0;
  std::vector<float> mScratch[2];
  bool mActive = false;
};

// ---- AUv2 -------------------------------------------------------------------------------------------------------

class Au final : public Plug {
public:
  explicit Au(const std::string& bundle) {
    static AudioComponent component = nullptr;
    if (!component) {
      NSBundle* b = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundle.c_str()]];
      NSError* error = nil;
      if (!b || ![b loadAndReturnError:&error]) Die("cannot load " + bundle);
      NSDictionary* c = [b.infoDictionary[@"AudioComponents"] firstObject];
      void* so = dlopen(b.executablePath.fileSystemRepresentation, RTLD_NOW);
      auto factory = reinterpret_cast<AudioComponentFactoryFunction>(dlsym(so, [c[@"factoryFunction"] UTF8String]));
      if (!factory) Die("no factory");
      const auto code = [](NSString* s) -> OSType {
        const char* p = s.UTF8String;
        return (OSType(p[0]) << 24) | (OSType(p[1]) << 16) | (OSType(p[2]) << 8) | OSType(p[3]);
      };
      AudioComponentDescription d{code(c[@"type"]), code(c[@"subtype"]), code(c[@"manufacturer"]), 0, 0};
      component = AudioComponentRegister(&d, (__bridge CFStringRef)c[@"name"], [c[@"version"] unsignedIntValue], factory);
      if (!component) Die("cannot register in this process");
    }
    Check(AudioComponentInstanceNew(component, &mUnit), "instantiate");
    AUEventListenerCreate(&OnEvent, this, CFRunLoopGetMain(), kCFRunLoopCommonModes, 0.f, 0.f, &mListener);
    for (int id = 0; id < kAllParams; ++id) {
      AudioUnitEvent e{};
      e.mArgument.mParameter = {mUnit, static_cast<AudioUnitParameterID>(id), kAudioUnitScope_Global, 0};
      for (AudioUnitEventType t : {kAudioUnitEvent_BeginParameterChangeGesture, kAudioUnitEvent_ParameterValueChange,
                                   kAudioUnitEvent_EndParameterChangeGesture}) {
        e.mEventType = t;
        AUEventListenerAddEventType(mListener, this, &e);
      }
    }
    AudioUnitAddPropertyListener(mUnit, kAudioUnitProperty_Latency, &OnLatency, this);
  }

  ~Au() override {
    CloseView();
    Stop();
    AUListenerDispose(mListener);
    AudioComponentInstanceDispose(mUnit);
  }

  bool Setup(const Format& f) override {
    Stop();
    AudioUnitRemovePropertyListenerWithUserData(mUnit, kAudioUnitProperty_Latency, &OnLatency, this);
    AudioUnitAddPropertyListener(mUnit, kAudioUnitProperty_Latency, &OnLatency, this);
    format = f;
    AudioStreamBasicDescription d{};
    d.mSampleRate = f.rate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    d.mBitsPerChannel = 32;
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = d.mBytesPerPacket = 4;
    d.mChannelsPerFrame = f.in;
    if (AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &d, sizeof d)) return false;
    d.mChannelsPerFrame = f.out;
    if (AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &d, sizeof d)) return false;
    const UInt32 offline = f.offline;
    if (AudioUnitSetProperty(mUnit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &offline, sizeof offline)) return false;
    const UInt32 frames = f.block;
    if (AudioUnitSetProperty(mUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &frames, sizeof frames)) return false;
    AURenderCallbackStruct pull{&OnPull, this};
    if (AudioUnitSetProperty(mUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &pull, sizeof pull)) return false;
    if (AudioUnitInitialize(mUnit)) return false;
    mActive = true;
    return true;
  }

  void Stop() override {
    if (mActive) AudioUnitUninitialize(mUnit);
    mActive = false;
  }

  void Reset() override { AudioUnitReset(mUnit, kAudioUnitScope_Global, 0); }

  void Set(int id, double v, bool) override {
    if (id == kBypassId) {
      const UInt32 on = v > 0.5;
      Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &on, sizeof on), "bypass");
      return;
    }
    const magmora::ParamInfo& p = PI(id);
    AudioUnitSetParameter(mUnit, id, kAudioUnitScope_Global, 0, static_cast<float>(p.min + v * (p.max - p.min)), 0);
  }

  double Get(int id) override {
    if (id == kBypassId) {
      UInt32 on = 0, size = sizeof on;
      AudioUnitGetProperty(mUnit, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &on, &size);
      return on;
    }
    float v = 0.f;
    AudioUnitGetParameter(mUnit, id, kAudioUnitScope_Global, 0, &v);
    const magmora::ParamInfo& p = PI(id);
    return (v - p.min) / (p.max - p.min);
  }

  std::string Text(int id, double v) override {
    const magmora::ParamInfo& p = PI(id);
    float plain = static_cast<float>(p.min + v * (p.max - p.min));
    AudioUnitParameterStringFromValue q{static_cast<AudioUnitParameterID>(id), &plain, nullptr};
    UInt32 size = sizeof q;
    if (AudioUnitGetProperty(mUnit, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0, &q, &size)) return "?";
    return [(__bridge_transfer NSString*)q.outString UTF8String];
  }

  bool Automatable(int id) override {
    AudioUnitParameterInfo info{};
    UInt32 size = sizeof info;
    AudioUnitGetProperty(mUnit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &size);
    return !(info.flags & kAudioUnitParameterFlag_NonRealTime);
  }

  void Render(const float* const* in, float* const* out, int n, bool inPlace) override {
    if (!n) return;
    mIn[0] = in[0];
    mIn[1] = in[format.in > 1 ? 1 : 0];
    struct {
      AudioBufferList list;
      AudioBuffer more[1];
    } b{};
    b.list.mNumberBuffers = format.out;
    for (int c = 0; c < format.out; ++c) {
      // "In place" for an AU: the host supplies no output memory.
      b.list.mBuffers[c] = {1, static_cast<UInt32>(n * 4), inPlace ? nullptr : out[c]};
    }
    AudioTimeStamp when{};
    when.mFlags = kAudioTimeStampSampleTimeValid;
    when.mSampleTime = mTime;
    mTime += n;
    AudioUnitRenderActionFlags flags = 0;
    if (gCountAllocations) magmora_test_rendering(1);
    const OSStatus rendered = AudioUnitRender(mUnit, &flags, &when, 0, n, &b.list);
    if (gCountAllocations) magmora_test_rendering(0);
    Check(rendered, "render");
    if (inPlace)
      for (int c = 0; c < format.out; ++c) std::memcpy(out[c], b.list.mBuffers[c].mData, n * 4);
  }

  int Latency() override {
    Float64 seconds = 0;
    UInt32 size = sizeof seconds;
    AudioUnitGetProperty(mUnit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds, &size);
    return static_cast<int>(std::lround(seconds * format.rate));
  }

  int Tail() override {
    Float64 seconds = 0;
    UInt32 size = sizeof seconds;
    AudioUnitGetProperty(mUnit, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &seconds, &size);
    return static_cast<int>(std::lround(seconds * format.rate));
  }

  bool Save(Bytes& b) override {
    CFPropertyListRef list = nullptr;
    UInt32 size = sizeof list;
    if (AudioUnitGetProperty(mUnit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &list, &size)) return false;
    NSMutableDictionary* d = [(__bridge_transfer NSDictionary*)list mutableCopy];
    mEnvelope = d;
    NSData* data = d[@kAUPresetDataKey];
    b.assign(static_cast<const char*>(data.bytes), static_cast<const char*>(data.bytes) + data.length);
    return true;
  }

  // A name that is no factory preset, so that only the data decides.
  bool Load(const Bytes& b) override { return LoadQuirky(b, "-"); }

  bool LoadQuirky(const Bytes& b, const char* name) override {
    if (!mEnvelope) {
      Bytes ignore;
      Save(ignore);
    }
    NSMutableDictionary* d = [mEnvelope mutableCopy];
    d[@kAUPresetDataKey] = [NSData dataWithBytes:b.data() length:b.size()];
    d[@kAUPresetNameKey] = @(name ? name : "-");
    CFPropertyListRef list = (__bridge CFPropertyListRef)d;
    return AudioUnitSetProperty(mUnit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &list, sizeof list) == noErr;
  }

  NSView* OpenView(NSView* parent) override {
    UInt32 size = 0;
    Boolean writable = false;
    Check(AudioUnitGetPropertyInfo(mUnit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable), "ui info");
    AudioUnitCocoaViewInfo info{};
    Check(AudioUnitGetProperty(mUnit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &info, &size), "ui");
    NSBundle* b = [NSBundle bundleWithURL:(__bridge_transfer NSURL*)info.mCocoaAUViewBundleLocation];
    NSString* name = (__bridge_transfer NSString*)info.mCocoaAUViewClass[0];
    id<AUCocoaUIBase> maker = [[[b classNamed:name] alloc] init];
    NSView* view = [maker uiViewForAudioUnit:mUnit withSize:NSMakeSize(1280, 620)];
    if (!view) return nil;
    [parent addSubview:view];
    mView = view;
    return view;
  }

  void CloseView() override {
    [mView removeFromSuperview];
    mView = nil;
  }

private:
  static void Check(OSStatus s, const char* what) {
    if (s) Die(std::string("AU ") + what + " failed: " + std::to_string(s));
  }

  static OSStatus OnPull(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* io) {
    auto* au = static_cast<Au*>(self);
    for (UInt32 c = 0; c < io->mNumberBuffers; ++c) std::memcpy(io->mBuffers[c].mData, au->mIn[std::min<UInt32>(c, 1)], n * 4);
    return noErr;
  }

  static void OnEvent(void* self, void*, const AudioUnitEvent* e, UInt64, Float32 v) {
    const int id = static_cast<int>(e->mArgument.mParameter.mParameterID);
    const magmora::ParamInfo& p = PI(id);
    const char what = e->mEventType == kAudioUnitEvent_BeginParameterChangeGesture ? 'b'
                      : e->mEventType == kAudioUnitEvent_EndParameterChangeGesture ? 'e' : 'v';
    static_cast<Au*>(self)->events.push_back({what, id, (v - p.min) / (p.max - p.min)});
  }

  static void OnLatency(void* self, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement) {
    ++static_cast<Au*>(self)->latencyNotices;
  }

  AudioUnit mUnit = nullptr;
  AUEventListenerRef mListener = nullptr;
  NSMutableDictionary* mEnvelope = nil;
  __weak NSView* mView = nil;
  const float* mIn[2] = {};
  double mTime = 0.;
  bool mActive = false;
};

inline std::unique_ptr<Plug> Make(const std::string& bundle) {
  if (bundle.find(".component") != std::string::npos) return std::make_unique<Au>(bundle);
  return std::make_unique<V3>(bundle);
}

}  // namespace host
