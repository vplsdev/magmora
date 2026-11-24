// The chain shell (XP0): the head plus pass-through stubs, one parameter space, state sections and presets.
#include <magmora/Chain.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "chain/Blocks.h"
#include "chain/Bytes.h"
#include "chain/Json.h"
#include "cab/UserIr.h"
#include "cab/VirtualMic.h"
#include "engine/Calibration.h"
#include "engine/DenormalGuard.h"
#include "eq/Eq.h"
#include "pedal/PedalStage.h"
#include "transpose/Transposer.h"

namespace magmora {
namespace {

constexpr int kChainCount = static_cast<int>(kNumChainParams) - static_cast<int>(kNumParams);

// A chain parameter id's index into the blocks' values.
constexpr size_t slotOf(int id) { return static_cast<size_t>(id - static_cast<int>(kNumParams)); }

// The head state chunk, as the plug-in has written it since 0.1.0 (plugin/Magmora.cpp, "the state chunk"):
//   u32 magic, i32 version, i32 size of the body, body: i32 flags, i32 count, count x (string key, f64 value),
//   string preset name, f32 UI scale, and from version 2: string value set name, string its model hash.
constexpr uint32_t kHeadMagic = 0x41524353;  // "SCRA"
constexpr int32_t kHeadVersion = 2;
constexpr int32_t kHeadPresetFlag = 1;       // a preset's parameters only; the session's own are left alone
constexpr const char* kHeadVersion1ValueSet = "prototype";
constexpr uint32_t kChainMagic = 0x48434353;  // "SCCH"

// The session's parameters: a preset never carries them (plugin/scripts/generate.py SESSION_KEYS). The transposer's
// two belong to the player and the session, as the input trim does (added 2026-11-24).
constexpr int kSessionParams[] = {kInputTrim, kLoad, kQuality, kPhaseMode, kChannelMode, kTranspose, kTransposeMode};

struct SectionInfo {
  char tag[5];
  ChainBlock block;
  int first, last;  // chain parameter ids, inclusive
  const char* presetKey;  // null: a session section, never in a preset
  int extraFirst = -1, extraLast = -1;  // ids appended later that belong to the block (the cabinet's sources)
  bool has(int id) const { return (id >= first && id <= last) || (id >= extraFirst && id <= extraLast); }
};
constexpr SectionInfo kSections[] = {
    {"pedl", ChainBlock::Pedal, kPedalOn, kPedalLevel, "pedal"},
    {"eq  ", ChainBlock::Eq, kEqOn, kEqBand4Type, "eq"},
    {"cab ", ChainBlock::Cabinet, kCabOn, kMicOffset, "cab", kMic1Source, kMic2Source},
    {"uir ", ChainBlock::UserIr, kUserIrOn, kUserIrLevel, "user_ir"},
    {"fx  ", ChainBlock::FxSlot, kFxOn, kFxOn, "fx"},
    // The head's own on/off. A section of its own, so that a state without it (an older one) loads with the head
    // on (its default); in a preset it sits in "params", with the head's other values.
    {"amp ", ChainBlock::Amp, kAmpOn, kAmpOn, "params"},
    // The transposer (added 2026-11-24): session settings, so no preset key. A state without it loads at 0 st, Tight.
    {"trns", ChainBlock::Count, kTranspose, kTransposeMode, nullptr},
};
constexpr int32_t kBlockSectionVersion = 1;
constexpr int32_t kCabPackSectionVersion = 2;
constexpr double kCentreCompensationDb = 3.010299956639812;  // 20 log10(sqrt 2): constant-power centre to unity  // 2: the slots' response files
constexpr int kOnParam[] = {kPedalOn, kEqOn, kCabOn, kUserIrOn, kFxOn, kAmpOn};  // by ChainBlock
static_assert(std::size(kOnParam) == static_cast<size_t>(ChainBlock::Count), "one on parameter per block");

const ParamInfo& info(int id) { return id < kNumParams ? kParams[id] : kChainParams[id - kNumParams]; }

double clampToRange(const ParamInfo& p, double value) {
  value = std::clamp(value, p.min, p.max);
  return p.taper == Taper::Stepped ? std::round(value) : value;
}

struct HeadState {
  bool preset = false;
  double values[kNumParams];
  bool given[kNumParams] = {};
  std::string presetName, setName = kHeadVersion1ValueSet, setHash;
  float scale = 0.f;
};

bool readHead(const uint8_t* data, size_t size, HeadState& out, size_t* used) {
  chain::Reader in(data, size);
  uint32_t magic = 0;
  int32_t version = 0, body = 0, flags = 0, count = 0;
  if (!in.get(magic) || magic != kHeadMagic || !in.get(version) || version < 1) return false;
  if (!in.get(body) || body < 0 || body > chain::kMaxBody || !in.limit(static_cast<size_t>(body))) return false;
  if (!in.get(flags) || !in.get(count) || count < 0 || count > chain::kMaxEntries) return false;
  out.preset = (flags & kHeadPresetFlag) != 0;
  for (int32_t n = 0; n < count; ++n) {
    std::string key;
    double value = 0.0;
    if (!in.string(key) || !in.get(value)) return false;
    if (!std::isfinite(value)) continue;
    for (const ParamInfo& p : kParams)
      if (key == p.key) {
        out.values[p.id] = value;
        out.given[p.id] = true;
      }
  }
  if (!in.string(out.presetName) || !in.get(out.scale)) return false;
  if (version >= 2 && (!in.string(out.setName) || !in.string(out.setHash))) return false;
  if (used) *used = 12 + static_cast<size_t>(body);  // later versions may append to the body: skipped
  return true;
}

// The EQ in the head's oversampled loop, through the engine's internal-stage hook.
struct EqStage final : engine::InternalStage {
  eq::Eq eq;
  bool active() const override { return !eq.idle(); }
  void process(double* const* lanes, int frames) override { eq.process(lanes[0], lanes[1], frames); }
};

// The pedal's own rate relative to the head's internal rate. MAGMORA_PEDAL_OVERSAMPLE=1|2 overrides it, for
// measurement only.
int pedalOversampling(double internalRate) {
  if (const char* e = std::getenv("MAGMORA_PEDAL_OVERSAMPLE")) {
    const int v = std::atoi(e);
    return v == 2 || v == pedal::PedalStage::kMiddle2x ? v : 1;
  }
  // O2 and O3 at 2x up to 400 kHz inside (Standard and High; Ultra stays at 1x), O1 and O4 at the head's rate:
  // measured at 48 kHz, drive 10, the 10 kHz band's aliasing went from -13.1 to -56.1 dB at Standard and from -31.1
  // to -41.6 dB at High with the whole pedal at 2x; the 2x middle alone gives the same, at less cost.
  return internalRate <= 400000.0 ? pedal::PedalStage::kMiddle2x : 1;
}

// The transposer's paths: out of the signal (kDirect) or in, in one of its modes (the TransposeMode's value).
constexpr double kPi = 3.14159265358979323846;
constexpr int kDirect = -1;
constexpr int kNoPath = -2;  // no outgoing path: not switching
constexpr int kTransposeCores = 2;
constexpr int kTransposeModes = 2;

int transposePath(double semitones, double mode) {
  return std::lround(semitones) != 0 ? std::clamp(static_cast<int>(std::lround(mode)), 0, kTransposeModes - 1) : kDirect;
}

// A section this version does not know: kept and written back as it came.
struct Foreign {
  char tag[4];
  int32_t version;
  std::string hash;
  std::vector<uint8_t> payload;
};

}  // namespace

struct Chain::Impl {
  Engine engine;
  std::array<std::atomic<double>, kChainCount> values;
  pedal::PedalStage pedal;
  EqStage eqStage;
  cab::VirtualMic cabinet;  // XP6: responses built on its worker, convolved here
  chain::FxSlotStub fx;
  int inputs = 2;
  double rate = 0.0;
  int maxBlock = 0;
  std::string presetName;
  float scale = 1.f;
  std::string cabFolder, cabMapping, cabMicNames[2];  // the "cabp" section
  std::string cabIrReference[2];                      // each slot's response file
  std::vector<Foreign> unknown;  // sections this version does not know, written back as they came

  // The transposer at the input (added 2026-11-24): one per amp core and mode, each prepared for its mode, so that a
  // change of path never configures or allocates on the audio thread and the old path can play while the new one fills.
  Transposer transposers[kTransposeCores][kTransposeModes];
  bool transposersReady = false;
  ChannelMode route = ChannelMode::MonoLeft;  // as the engine was prepared with (main thread, audio stopped)
  std::atomic<int> wantedPath{kDirect};       // main thread: applyTransposeStructure
  std::atomic<int> pathLatency{0};            // the transposer's share of latencySamples()
  // audio thread
  int curPath = kDirect, prevPath = kNoPath;  // the path heard, and the one it is fading from
  int fadeWait = 0, fadePos = 0;              // samples until the fade starts (the new path's delay), samples into it
  std::vector<float> fadeCurve;               // raised cosine, kTransposeFadeMs
  std::vector<float> scratchIn[kTransposeCores], scratchCur[kTransposeCores], scratchPrev[kTransposeCores];
  std::vector<float> feedF[kTransposeCores];
  std::vector<double> feedD[kTransposeCores];

  Impl() {
    for (int k = 0; k < kChainCount; ++k) values[static_cast<size_t>(k)].store(kChainParams[k].def, std::memory_order_relaxed);
  }

  Transposer& transposer(int core, int path) { return transposers[core][path]; }
  int transposeCoresInUse() const { return inputs == 2 && route == ChannelMode::DualMono ? 2 : 1; }

  // Main thread, audio stopped: the path heard is the one wanted, at once, from silence.
  void snapTransposePath() {
    prevPath = kNoPath;
    fadeWait = fadePos = 0;
    curPath = wantedPath.load(std::memory_order_acquire);
    restartPath(curPath);
  }

  // Audio thread (or with it stopped): the path starts from silence at the current shift. Bounded work, no allocation.
  void restartPath(int path) {
    if (path < 0 || !transposersReady) return;
    const int semitones = static_cast<int>(std::lround(values[slotOf(kTranspose)].load(std::memory_order_relaxed)));
    for (int c = 0; c < kTransposeCores; ++c) {
      transposers[c][path].setSemitones(semitones);
      transposers[c][path].reset();
    }
  }

  float* feedOf(int core, const float*) { return feedF[core].data(); }
  double* feedOf(int core, const double*) { return feedD[core].data(); }

  // The signal the head is fed: the input itself while the transposer is out (the same pointers, so the head's output
  // is bit for bit as before), else the transposer's output, crossfaded from the old path while the path changes.
  template <class Sample>
  const Sample* const* transposeInput(const Sample* const* in, const Sample** feed, int frames) {
    if (!transposersReady) return in;
    const int wanted = wantedPath.load(std::memory_order_acquire);
    if (wanted != curPath && prevPath == kNoPath) {  // a change waits for the one under way to finish
      prevPath = curPath;
      curPath = wanted;
      restartPath(curPath);
      // A path that starts from silence plays the onset of its history until that has left its analysis window:
      // the fade waits that long (43 ms at 44.1 / 48 kHz), the old path playing meanwhile.
      fadeWait = curPath >= 0 ? transposers[0][curPath].analysisSamples() : 0;
      fadePos = 0;
    }
    if (curPath == kDirect && prevPath == kNoPath) return in;

    const int semitones = static_cast<int>(std::lround(values[slotOf(kTranspose)].load(std::memory_order_relaxed)));
    const int cores = transposeCoresInUse();
    const bool sum = inputs == 2 && route == ChannelMode::MonoSum;
    const int fadeLength = static_cast<int>(fadeCurve.size());
    int wait = fadeWait, pos = fadePos;
    for (int c = 0; c < cores; ++c) {
      float* x = scratchIn[c].data();
      const Sample* src = in[c];
      for (int n = 0; n < frames; ++n)
        x[n] = static_cast<float>(sum ? 0.5 * (static_cast<double>(in[0][n]) + static_cast<double>(in[1][n])) : static_cast<double>(src[n]));
      const auto run = [&](int path, float* y) {
        Transposer& t = transposers[c][path];
        t.setSemitones(semitones);
        t.process(x, y, frames);
      };
      if (curPath >= 0) run(curPath, scratchCur[c].data());
      if (prevPath >= 0) run(prevPath, scratchPrev[c].data());
      Sample* y = feedOf(c, static_cast<const Sample*>(nullptr));
      const auto direct = [&](int n) -> double {
        return sum ? 0.5 * (static_cast<double>(in[0][n]) + static_cast<double>(in[1][n])) : static_cast<double>(src[n]);
      };
      const auto pathAt = [&](int path, const float* out, int n) -> double {
        return path >= 0 ? static_cast<double>(out[n]) : direct(n);
      };
      wait = fadeWait;
      pos = fadePos;
      for (int n = 0; n < frames; ++n) {
        const double now = pathAt(curPath, scratchCur[c].data(), n);
        if (prevPath == kNoPath || pos >= fadeLength) {
          y[n] = static_cast<Sample>(now);
          continue;
        }
        const double before = pathAt(prevPath, scratchPrev[c].data(), n);
        if (wait > 0) {
          --wait;
          y[n] = static_cast<Sample>(before);
          continue;
        }
        const double w = fadeCurve[static_cast<size_t>(pos++)];
        y[n] = static_cast<Sample>(before + w * (now - before));
      }
      feed[c] = y;
    }
    if (cores == 1 && inputs == 2) feed[1] = feed[0];  // mono routing: the engine reads one channel, or sums two equal ones
    if (prevPath != kNoPath) {
      fadeWait = wait;
      fadePos = pos;
      if (pos >= fadeLength) {  // faded: the old path stops (no CPU)
        prevPath = kNoPath;
        fadePos = fadeWait = 0;
      }
    }
    return feed;
  }

  template <class Sample>
  void process(const Sample* const* in, Sample* const* out, int frames) {
    if (frames <= 0) return;
    if (frames > maxBlock && maxBlock > 0) {  // not within the contract; kept safe, in pieces the scratch holds
      for (int at = 0; at < frames; at += maxBlock) {
        const int n = std::min(maxBlock, frames - at);
        const Sample* i[2] = {in[0] + at, inputs == 2 ? in[1] + at : nullptr};
        Sample* o[2] = {out[0] + at, out[1] + at};
        process(i, o, n);
      }
      return;
    }
    const engine::DenormalGuard denormals;  // the whole chain: the cabinet's convolution decays to silence too
    double eqControls[eq::kControls];
    for (int k = 0; k < eq::kControls; ++k) eqControls[k] = values[slotOf(kEqOn + k)].load(std::memory_order_relaxed);
    // eq_position has no effect in this release: every block after the head is linear and time-invariant, so
    // "after the cabinet" is the same signal
    eqStage.eq.setControls(eqControls, eqControls[0] >= 0.5);
    auto chainValue = [&](int id) { return values[slotOf(id)].load(std::memory_order_relaxed); };
    pedal.setControls(chainValue(kPedalOn) >= 0.5, chainValue(kPedalDrive), chainValue(kPedalTone), chainValue(kPedalLevel));
    engine.setAmpOn(chainValue(kAmpOn) >= 0.5);  // off, the pedal and the EQ pass without the amp
    // The transposer, ahead of the head's input trim and gate (a gain before or after it is the same: the
    // transposer is linear in its input). Out of the path, the head is fed the input pointers themselves.
    const Sample* feed[2] = {nullptr, nullptr};
    const Sample* const* headIn = transposeInput(in, feed, frames);
    engine.process(headIn, out, frames);  // the EQ runs inside, at the internal rate
    if (inputs == 1) std::copy_n(out[0], frames, out[1]);
    // pedal: inside the head (XP2); then the blocks after it
    cabinet.process(out[0], out[1], frames);
    fx.process(out[0], out[1], frames);
    // The engine's last line of defence, again after the blocks behind it: a response file with huge taps and
    // normalisation off can overflow the convolution, and nothing may leave non-finite or past the clamp.
    for (int c = 0; c < 2; ++c)
      for (int n = 0; n < frames; ++n) {
        const double v = static_cast<double>(out[c][n]);
        out[c][n] = static_cast<Sample>(std::isfinite(v) ? std::clamp(v, -engine::kOutputClamp, engine::kOutputClamp) : 0.0);
      }
  }

  void writeHead(chain::Writer& w) const {
    std::vector<uint8_t> bodyBytes;
    chain::Writer body(bodyBytes);
    body.put(int32_t{0});
    body.put(int32_t{kNumParams});
    for (int i = 0; i < kNumParams; ++i) {
      body.string(kParams[i].key);
      body.put(engine.getParam(static_cast<ParamId>(i)));
    }
    body.string(presetName.c_str());
    body.put(scale);
    const int set = engine.valueSet();
    body.string(Engine::valueSetName(set));
    body.string(Engine::valueSetModelHash(set));
    w.put(kHeadMagic);
    w.put(kHeadVersion);
    w.put(static_cast<int32_t>(bodyBytes.size()));
    w.bytes(bodyBytes.data(), bodyBytes.size());
  }

  // Everything is checked before anything is changed.
  void applyHead(const HeadState& h, bool session) {
    for (int i = 0; i < kNumParams; ++i) {
      const bool sessionParam = Chain::isSessionParameter(i);
      if (h.preset && sessionParam) continue;
      engine.setParam(static_cast<ParamId>(i), h.given[i] ? h.values[i] : kParams[i].def);
    }
    if (!session || h.preset) return;
    presetName = h.presetName;
    if (std::isfinite(h.scale) && h.scale > 0.f) scale = h.scale;
    const int set = Engine::findValueSet(h.setName.c_str());
    engine.setValueSet(set < 0 ? Engine::defaultValueSet() : set);
  }
};

Chain::Chain() : impl_(std::make_unique<Impl>()) {}
Chain::~Chain() = default;

bool Chain::prepare(double sampleRate, int maxBlockSize, int numInputChannels) {
  if (numInputChannels != 1 && numInputChannels != 2) return false;
  if (!impl_->engine.prepare(sampleRate, maxBlockSize, numInputChannels)) return false;
  impl_->inputs = numInputChannels;
  impl_->eqStage.eq.prepare(impl_->engine.internalSampleRate());
  impl_->engine.setInternalStage(&impl_->eqStage);
  impl_->pedal.prepare(impl_->engine.internalSampleRate(), sampleRate, impl_->engine.ampCores(),
                       pedalOversampling(impl_->engine.internalSampleRate()));
  impl_->engine.setPreStage(&impl_->pedal);
  impl_->cabinet.prepare(sampleRate, maxBlockSize, impl_->engine.ampCores() == 2, &impl_->values[slotOf(kCabOn)],
                         &impl_->values[slotOf(kMic1Source)]);
  impl_->rate = sampleRate;
  impl_->maxBlock = maxBlockSize;
  impl_->fx.prepare(sampleRate, maxBlockSize);
  // The transposer: every core and mode, so that neither a change of path nor of routing allocates later.
  for (int c = 0; c < kTransposeCores; ++c) {
    for (int m = 0; m < kTransposeModes; ++m) {
      Transposer& t = impl_->transposers[c][m];
      t.prepare(sampleRate, maxBlockSize);
      t.setMode(m == static_cast<int>(TransposeMode::Tight) ? Transposer::Mode::Tight : Transposer::Mode::Standard);
    }
    for (auto* v : {&impl_->scratchIn[c], &impl_->scratchCur[c], &impl_->scratchPrev[c], &impl_->feedF[c]})
      v->assign(static_cast<size_t>(maxBlockSize), 0.0f);
    impl_->feedD[c].assign(static_cast<size_t>(maxBlockSize), 0.0);
  }
  const int fade = std::max(1, static_cast<int>(std::lround(kTransposeFadeMs * 1e-3 * sampleRate)));
  impl_->fadeCurve.resize(static_cast<size_t>(fade));
  for (int n = 0; n < fade; ++n)
    impl_->fadeCurve[static_cast<size_t>(n)] = static_cast<float>(0.5 - 0.5 * std::cos(kPi * (n + 1) / (fade + 1)));
  impl_->transposersReady = true;
  impl_->route = static_cast<ChannelMode>(std::clamp(static_cast<int>(std::lround(getParameter(kChannelMode))), 0, 2));
  applyTransposeStructure();
  impl_->snapTransposePath();
  return true;
}

void Chain::setCalibration(const Calibration& c) { impl_->engine.setCalibration(c); }
// Every block but the transposer adds zero; the transposer adds its mode's delay while it is in the path.
int Chain::latencySamples() const {
  return impl_->engine.latencySamples() + impl_->pathLatency.load(std::memory_order_acquire);
}
void Chain::applyTransposeStructure() {
  const int path = transposePath(getParameter(kTranspose), getParameter(kTransposeMode));
  const int delay = path >= 0 && impl_->transposersReady ? impl_->transposers[0][path].latencySamples() : 0;
  impl_->pathLatency.store(delay, std::memory_order_release);
  impl_->wantedPath.store(path, std::memory_order_release);
}
bool Chain::transposeInPath() const { return impl_->wantedPath.load(std::memory_order_acquire) >= 0; }
int Chain::transposeLatencySamples() const { return impl_->pathLatency.load(std::memory_order_acquire); }
bool Chain::transposeSwitching() const {
  return impl_->prevPath != kNoPath || impl_->wantedPath.load(std::memory_order_acquire) != impl_->curPath;
}
void Chain::applyStructuralParams() {
  impl_->engine.applyStructuralParams();
  impl_->route = static_cast<ChannelMode>(std::clamp(static_cast<int>(std::lround(getParameter(kChannelMode))), 0, 2));
  applyTransposeStructure();
  impl_->snapTransposePath();  // audio is stopped, and the head restarts too
  if (impl_->engine.internalSampleRate() > 0.0) {
    impl_->eqStage.eq.prepare(impl_->engine.internalSampleRate());
    impl_->pedal.prepare(impl_->engine.internalSampleRate(), impl_->rate, impl_->engine.ampCores(),
                         pedalOversampling(impl_->engine.internalSampleRate()));
    impl_->cabinet.prepare(impl_->rate, impl_->maxBlock, impl_->engine.ampCores() == 2,  // routing may have changed
                           &impl_->values[slotOf(kCabOn)], &impl_->values[slotOf(kMic1Source)]);
  }
}
Engine& Chain::engine() { return impl_->engine; }
const Engine& Chain::engine() const { return impl_->engine; }

void Chain::setParameter(int id, double value) {
  if (id < 0 || id >= kNumChainParams || !std::isfinite(value)) return;
  if (id < kNumParams) {
    impl_->engine.setParam(static_cast<ParamId>(id), value);
    return;
  }
  impl_->values[slotOf(id)].store(clampToRange(info(id), value), std::memory_order_relaxed);
  if (id == kAmpOn) impl_->engine.setAmpOn(getParameter(kAmpOn) >= 0.5);  // so that a reset snaps to it
}

double Chain::getParameter(int id) const {
  if (id < 0 || id >= kNumChainParams) return 0.0;
  if (id < kNumParams) return impl_->engine.getParam(static_cast<ParamId>(id));
  return impl_->values[slotOf(id)].load(std::memory_order_relaxed);
}

int Chain::parameterCount() { return kNumChainParams; }
const ParamInfo* Chain::parameterInfo(int id) { return id < 0 || id >= kNumChainParams ? nullptr : &info(id); }
int Chain::findParameter(const char* key) {
  if (!key) return -1;
  for (int id = 0; id < kNumChainParams; ++id)
    if (std::strcmp(info(id).key, key) == 0) return id;
  return -1;
}
bool Chain::blockOn(ChainBlock b) const {
  if (b == ChainBlock::UserIr)  // now: a cabinet slot that is on with its file as the source
    return getParameter(kCabOn) >= 0.5 && ((getParameter(kMic1On) >= 0.5 && getParameter(kMic1Source) >= 0.5) ||
                                           (getParameter(kMic2On) >= 0.5 && getParameter(kMic2Source) >= 0.5));
  const int k = static_cast<int>(b);
  return k >= 0 && k < static_cast<int>(ChainBlock::Count) && getParameter(kOnParam[k]) >= 0.5;
}
bool Chain::isSessionParameter(int id) {
  return std::find(std::begin(kSessionParams), std::end(kSessionParams), id) != std::end(kSessionParams);
}

void Chain::reset() {
  impl_->engine.setAmpOn(getParameter(kAmpOn) >= 0.5);  // the head starts on or off, without a fade
  impl_->engine.reset();
  impl_->pedal.reset();
  impl_->eqStage.eq.reset();
  impl_->cabinet.reset();
  impl_->fx.reset();
  impl_->snapTransposePath();
}
void Chain::process(const float* const* in, float* const* out, int frames) { impl_->process(in, out, frames); }
void Chain::process(const double* const* in, double* const* out, int frames) { impl_->process(in, out, frames); }
const Meters& Chain::meters() const { return impl_->engine.meters(); }

void Chain::setSessionInfo(const char* name, float uiScale) {
  impl_->presetName = name ? name : "";
  impl_->scale = uiScale;
}
const char* Chain::presetName() const { return impl_->presetName.c_str(); }
float Chain::uiScale() const { return impl_->scale; }
// The XP10 calls, kept: the response file is now the source of cabinet slot 2.
void Chain::setUserIrReference(const char* reference) { setCabinetIrReference(1, reference); }
const char* Chain::userIrReference() const { return cabinetIrReference(1); }
bool Chain::loadUserIr(const char* path, std::string* error) { return loadCabinetIr(1, path, error); }
bool Chain::loadUserIrSamples(const float* const* channels, int numChannels, int frames, double sampleRate,
                              std::string* error) {
  return loadCabinetIrSamples(1, channels, numChannels, frames, sampleRate, error);
}
void Chain::clearUserIr() { clearCabinetIr(1); }
bool Chain::userIrLoaded() const { return cabinetIrLoaded(1); }
double Chain::userIrSeconds() const { return cabinetIrSeconds(1); }
int Chain::userIrChannels() const { return cabinetIrChannels(1); }
void Chain::setUserIrNormalize(bool on) { setCabinetIrNormalize(1, on); }
bool Chain::userIrNormalize() const { return cabinetIrNormalize(1); }

bool Chain::loadCabinetIr(int slot, const char* path, std::string* error) {
  if (slot < 0 || slot > 1 || !path) {
    if (error) *error = "no slot or no file";
    return false;
  }
  cab::Audio a;
  if (!cab::readWavFile(path, a, error) || !impl_->cabinet.setFile(slot, a, path, error)) return false;
  impl_->cabIrReference[slot] = path;
  return true;
}
bool Chain::loadCabinetIrSamples(int slot, const float* const* channels, int numChannels, int frames,
                                 double sampleRate, std::string* error) {
  if (slot < 0 || slot > 1 || !channels || numChannels < 1 || frames < 1) {
    if (error) *error = "empty response";
    return false;
  }
  cab::Audio a;
  a.rate = sampleRate;
  for (int c = 0; c < std::min(numChannels, 2); ++c) a.channels.emplace_back(channels[c], channels[c] + frames);
  return impl_->cabinet.setFile(slot, a, "", error);
}
void Chain::clearCabinetIr(int slot) {
  if (slot < 0 || slot > 1) return;
  impl_->cabinet.clearFile(slot);
  impl_->cabIrReference[slot].clear();
}
bool Chain::waitForCabinet(double timeoutSeconds) { return impl_->cabinet.waitUntilBuilt(timeoutSeconds); }
double Chain::cabinetResponseSeconds() const { return impl_->cabinet.responseSeconds(); }
bool Chain::cabinetIrLoaded(int slot) const { return impl_->cabinet.fileLoaded(slot); }
double Chain::cabinetIrSeconds(int slot) const { return impl_->cabinet.fileSeconds(slot); }
int Chain::cabinetIrChannels(int slot) const { return impl_->cabinet.fileChannels(slot); }
void Chain::setCabinetIrNormalize(int slot, bool on) { impl_->cabinet.setFileNormalize(slot, on); }
bool Chain::cabinetIrNormalize(int slot) const { return impl_->cabinet.fileNormalize(slot); }
void Chain::setCabinetIrRaw(int slot, bool raw) { impl_->cabinet.setFileRaw(slot, raw); }
bool Chain::cabinetIrRaw(int slot) const { return impl_->cabinet.fileRaw(slot); }
void Chain::setCabinetIrReference(int slot, const char* reference) {
  if (slot == 0 || slot == 1) impl_->cabIrReference[slot] = reference ? reference : "";
}
const char* Chain::cabinetIrReference(int slot) const {
  return slot == 0 || slot == 1 ? impl_->cabIrReference[slot].c_str() : "";
}
void Chain::idle() {}  // the cabinet's worker frees what the audio thread let go of

namespace {
int micIndex(const cab::Pack& pack, const std::string& label) {
  const auto& mics = pack.mics();
  for (size_t k = 0; k < mics.size(); ++k)
    if (label == cab::Pack::micLabel(mics[k])) return static_cast<int>(k);
  return -1;
}
}  // namespace

bool Chain::loadCabinetPack(const char* folder, std::string* error, const char* mappingFile) {
  if (!folder) {
    if (error) *error = "no folder";
    return false;
  }
  if (!impl_->cabinet.loadPack(folder, mappingFile ? mappingFile : "", error)) return false;
  impl_->cabFolder = folder;
  impl_->cabMapping = mappingFile ? mappingFile : "";
  if (const auto pack = impl_->cabinet.pack())  // a saved microphone choice is by name: find it in this pack
    for (int m = 0; m < 2; ++m)
      if (const int k = micIndex(*pack, impl_->cabMicNames[m]); k >= 0)
        setParameter(m == 0 ? kMic1Type : kMic2Type, static_cast<double>(k));
  return true;
}
bool Chain::loadCabinetPackMemory(const char* reference, const CabinetPackResponse* responses, int count,
                                  const char* mappingJson, std::string* error) {
  if (!reference || !*reference || !responses || count <= 0) {
    if (error) *error = "no responses";
    return false;
  }
  std::vector<cab::PackFile> files(static_cast<size_t>(count));
  for (int k = 0; k < count; ++k) {
    const CabinetPackResponse& r = responses[k];
    cab::PackFile& f = files[static_cast<size_t>(k)];
    f.name = r.name ? r.name : "";
    f.rate = r.sampleRate;
    if (r.samples && r.frames > 0) f.samples.assign(r.samples, r.samples + r.frames);
  }
  if (!impl_->cabinet.loadPackMemory(std::move(files), mappingJson ? mappingJson : "", reference, error)) return false;
  impl_->cabFolder = reference;
  impl_->cabMapping.clear();
  if (const auto pack = impl_->cabinet.pack())  // a saved microphone choice is by name: find it in this pack
    for (int m = 0; m < 2; ++m)
      if (const int k = micIndex(*pack, impl_->cabMicNames[m]); k >= 0)
        setParameter(m == 0 ? kMic1Type : kMic2Type, static_cast<double>(k));
  return true;
}
void Chain::clearCabinetPack() {
  impl_->cabinet.clearPack();
  impl_->cabFolder.clear();
  impl_->cabMapping.clear();
}
bool Chain::cabinetPackLoaded() const { return impl_->cabinet.packLoaded(); }
const char* Chain::cabinetPackReference() const { return impl_->cabFolder.c_str(); }
const char* Chain::cabinetMappingReference() const { return impl_->cabMapping.c_str(); }
int Chain::cabinetMicCount() const {
  const auto pack = impl_->cabinet.pack();
  return pack ? static_cast<int>(pack->mics().size()) : 0;
}
std::string Chain::cabinetMicName(int index) const {
  const auto pack = impl_->cabinet.pack();
  if (!pack || index < 0 || index >= static_cast<int>(pack->mics().size())) return {};
  return cab::Pack::micLabel(pack->mics()[static_cast<size_t>(index)]);
}
std::vector<std::string> Chain::cabinetSpeakers(int micIndex) const {
  const auto pack = impl_->cabinet.pack();
  return pack ? pack->speakers(micIndex) : std::vector<std::string>{};
}
std::vector<std::string> Chain::cabinetVariants(int micIndex, const char* speaker) const {
  const auto pack = impl_->cabinet.pack();
  return pack ? pack->variants(micIndex, speaker ? speaker : "") : std::vector<std::string>{};
}
std::vector<double> Chain::cabinetAngles(int micIndex, const char* speaker, const char* variant) const {
  const auto pack = impl_->cabinet.pack();
  return pack ? pack->angles(micIndex, speaker ? speaker : "", variant ? variant : "") : std::vector<double>{};
}
void Chain::setCabinetMicSource(int mic, const char* speaker, const char* variant) {
  impl_->cabinet.setSource(mic, speaker ? speaker : "", variant ? variant : "");
}
std::string Chain::cabinetMicSpeaker(int mic) const { return impl_->cabinet.speaker(mic); }
std::string Chain::cabinetMicVariant(int mic) const { return impl_->cabinet.variant(mic); }
CabinetStatus Chain::cabinetStatus() const { return impl_->cabinet.status(); }
bool Chain::parameterHidden(int id) { return id == kCabFloor || id == kUserIrOn || id == kUserIrLevel; }

void Chain::getState(std::vector<uint8_t>& out) const {
  out.clear();
  chain::Writer w(out);
  w.put(kChainMagic);
  w.put(int32_t{kStateVersion});
  w.put(static_cast<int32_t>(2 + std::size(kSections) + impl_->unknown.size()));
  auto section = [&](const char* tag, int32_t version, const char* hash, const std::vector<uint8_t>& payload) {
    w.bytes(tag, 4);
    w.put(version);
    w.string(hash);
    w.put(static_cast<int32_t>(payload.size()));
    w.bytes(payload.data(), payload.size());
  };
  std::vector<uint8_t> payload;
  {
    chain::Writer head(payload);
    impl_->writeHead(head);
    section("head", kHeadVersion, Engine::valueSetModelHash(impl_->engine.valueSet()), payload);
  }
  for (const SectionInfo& s : kSections) {
    payload.clear();
    chain::Writer body(payload);
    const int extra = s.extraFirst >= 0 ? s.extraLast - s.extraFirst + 1 : 0;
    body.put(static_cast<int32_t>(s.last - s.first + 1 + extra));
    for (int id = s.first; id <= s.last; ++id) {
      body.string(info(id).key);
      body.put(getParameter(id));
    }
    for (int id = s.extraFirst; extra > 0 && id <= s.extraLast; ++id) {
      body.string(info(id).key);
      body.put(getParameter(id));
    }
    if (s.block == ChainBlock::UserIr) {  // the old series file's reference and normalisation switch, now empty (the
      body.string("");                    // file is a cabinet slot's, in "cabp" version 2)
      body.put(int32_t{1});
    }
    section(s.tag, kBlockSectionVersion, s.block == ChainBlock::Pedal ? pedal::modelHash() : "", payload);
  }
  {  // the cabinet's pack (XP6): folder, mapping file, then per microphone its name, speaker and variant
    payload.clear();
    chain::Writer body(payload);
    body.string(impl_->cabFolder.c_str());
    body.string(impl_->cabMapping.c_str());
    for (int m = 0; m < 2; ++m) {
      std::string name = cabinetMicName(static_cast<int>(std::lround(getParameter(m == 0 ? kMic1Type : kMic2Type))));
      if (name.empty()) name = impl_->cabMicNames[m];  // no pack loaded: what the session had
      body.string(name.c_str());
      body.string(impl_->cabinet.speaker(m).c_str());
      body.string(impl_->cabinet.variant(m).c_str());
    }
    for (int m = 0; m < 2; ++m) {  // version 2: each slot's response file, normalisation, declared raw
      body.string(impl_->cabIrReference[m].c_str());
      body.put(static_cast<int32_t>(impl_->cabinet.fileNormalize(m) ? 1 : 0));
      body.put(static_cast<int32_t>(impl_->cabinet.fileRaw(m) ? 1 : 0));
    }
    section("cabp", kCabPackSectionVersion, "", payload);
  }
  for (const Foreign& f : impl_->unknown) {
    char tag[5] = {f.tag[0], f.tag[1], f.tag[2], f.tag[3], 0};
    section(tag, f.version, f.hash.c_str(), f.payload);
  }
}

namespace {
// A whole state as read, before anything is applied.
struct ParsedState {
  HeadState head;
  double block[kChainCount];
  std::string reference;
  bool normalize = true;
  std::vector<Foreign> unknown;
  std::string cabFolder, cabMapping, cabNames[2], cabSpeakers[2], cabVariants[2];
  std::string cabIr[2];
  bool cabIrNormalize[2] = {true, true}, cabIrRaw[2] = {false, false}, slotsGiven = false;
};

bool parseState(const uint8_t* data, size_t size, ParsedState& out) {
  if (!data) return false;
  HeadState& head = out.head;
  for (int i = 0; i < kNumParams; ++i) head.values[i] = kParams[i].def;
  double* block = out.block;
  for (int k = 0; k < kChainCount; ++k) block[k] = kChainParams[k].def;  // a missing section: that block off

  uint32_t magic = 0;
  if (size >= 4) std::memcpy(&magic, data, 4);
  if (magic == kHeadMagic) {  // a session from before the chain
    if (!readHead(data, size, head, nullptr)) return false;
  } else {
    chain::Reader in(data, size);
    int32_t version = 0, count = 0;
    if (!in.get(magic) || magic != kChainMagic || !in.get(version) || version < 1) return false;
    if (!in.get(count) || count < 0 || count > chain::kMaxSections) return false;
    bool haveHead = false;
    for (int32_t n = 0; n < count; ++n) {
      char tag[4];
      int32_t sectionVersion = 0, bytes = 0;
      std::string hash;
      if (!in.bytes(tag, 4) || !in.get(sectionVersion) || !in.string(hash) || !in.get(bytes) || bytes < 0 ||
          static_cast<size_t>(bytes) > in.left())
        return false;
      chain::Reader body(in.here(), static_cast<size_t>(bytes));
      if (std::memcmp(tag, "cabp", 4) == 0) {
        if (!body.string(out.cabFolder) || !body.string(out.cabMapping)) return false;
        for (int m = 0; m < 2; ++m)
          if (!body.string(out.cabNames[m]) || !body.string(out.cabSpeakers[m]) || !body.string(out.cabVariants[m]))
            return false;
        if (sectionVersion >= 2) {
          for (int m = 0; m < 2; ++m) {
            int32_t norm = 1, rawFlag = 0;
            if (!body.string(out.cabIr[m]) || !body.get(norm) || !body.get(rawFlag)) return false;
            out.cabIrNormalize[m] = norm != 0;
            out.cabIrRaw[m] = rawFlag != 0;
          }
          out.slotsGiven = true;
        }
      } else if (std::memcmp(tag, "head", 4) == 0) {
        if (haveHead || !readHead(in.here(), static_cast<size_t>(bytes), head, nullptr) || head.preset) return false;
        haveHead = true;
      } else {
        const SectionInfo* known = nullptr;
        for (const SectionInfo& s : kSections)
          if (std::memcmp(tag, s.tag, 4) == 0) known = &s;
        if (!known) {
          Foreign f;
          std::memcpy(f.tag, tag, 4);
          f.version = sectionVersion;
          f.hash = hash;
          f.payload.assign(in.here(), in.here() + bytes);
          out.unknown.push_back(std::move(f));
        } else {
          int32_t entries = 0;
          if (!body.get(entries) || entries < 0 || entries > chain::kMaxEntries) return false;
          for (int32_t e = 0; e < entries; ++e) {
            std::string key;
            double value = 0.0;
            if (!body.string(key) || !body.get(value)) return false;
            const int id = Chain::findParameter(key.c_str());
            if (id >= 0 && known->has(id) && std::isfinite(value)) block[slotOf(id)] = clampToRange(info(id), value);
          }
          if (known->block == ChainBlock::UserIr) {
            if (!body.string(out.reference)) return false;
            int32_t flag = 1;
            if (body.get(flag)) out.normalize = flag != 0;  // absent in the first chain states: on
          }
        }
      }
      in.skip(static_cast<size_t>(bytes));
    }
    if (!haveHead) return false;
  }
  return true;
}
}  // namespace

bool Chain::checkState(const uint8_t* data, size_t size) {
  ParsedState parsed;
  return parseState(data, size, parsed);
}

bool Chain::setState(const uint8_t* data, size_t size) {
  ParsedState parsed;
  if (!parseState(data, size, parsed)) return false;
  impl_->applyHead(parsed.head, true);
  // A state from before the cabinet slots took files, with the old series response file on: that file becomes slot 2's
  // source, on, centred, at the old level + 3.01 dB (the slot's constant-power centre is -3.01 dB; the series slot played
  // the file at unity), clamped to the
  // slot level's range (an old level above +8.99 dB loses the excess); with the cabinet off it was
  // the only thing heard, so slot 1 is switched off. With the cabinet on (a virtual microphone in series before the
  // file) there is no exact parallel equivalent: slot 1 keeps its microphone and the two are summed.
  auto at = [&](int id) -> double& { return parsed.block[slotOf(id)]; };
  if (!parsed.slotsGiven && at(kUserIrOn) >= 0.5 && !parsed.reference.empty()) {
    if (at(kCabOn) < 0.5) at(kMic1On) = 0.0;
    at(kCabOn) = 1.0;
    at(kMic2On) = 1.0;
    at(kMic2Source) = 1.0;
    at(kMic2Level) = clampToRange(info(kMic2Level), at(kUserIrLevel) + kCentreCompensationDb);
    at(kMic2Pan) = 0.0;
    at(kMic2Polarity) = 0.0;
    parsed.cabIr[1] = parsed.reference;
    parsed.cabIrNormalize[1] = parsed.normalize;
    at(kUserIrOn) = 0.0;
  }
  for (int k = 0; k < kChainCount; ++k) impl_->values[static_cast<size_t>(k)].store(parsed.block[k], std::memory_order_relaxed);
  impl_->engine.setAmpOn(at(kAmpOn) >= 0.5);
  impl_->unknown = std::move(parsed.unknown);
  impl_->cabFolder = parsed.cabFolder;
  impl_->cabMapping = parsed.cabMapping;
  for (int m = 0; m < 2; ++m) {
    impl_->cabIrReference[m] = parsed.cabIr[m];  // the reference only: the host loads it (loadCabinetIr), so a missing
    impl_->cabinet.setFileNormalize(m, parsed.cabIrNormalize[m]);  // file can be reported
    impl_->cabinet.setFileRaw(m, parsed.cabIrRaw[m]);
    impl_->cabMicNames[m] = parsed.cabNames[m];
    impl_->cabinet.setSource(m, parsed.cabSpeakers[m], parsed.cabVariants[m]);
    if (const auto pack = impl_->cabinet.pack())  // the pack is already loaded: the saved name decides the index
      if (const int k = micIndex(*pack, parsed.cabNames[m]); k >= 0) setParameter(m == 0 ? kMic1Type : kMic2Type, k);
  }
  return true;
}

bool Chain::applyPreset(const char* json, size_t size) {
  const auto root = chain::parseJson(json, size);
  if (!root || root->kind != chain::JsonValue::Kind::Object) return false;
  auto numberOf = [](const chain::JsonValue* v, double& out) {
    if (!v || (v->kind != chain::JsonValue::Kind::Number && v->kind != chain::JsonValue::Kind::Bool)) return false;
    out = v->number;
    return std::isfinite(out);
  };
  const chain::JsonValue* params = root->member("params");
  if (params && params->kind != chain::JsonValue::Kind::Object) return false;
  for (const SectionInfo& s : kSections) {
    if (!s.presetKey) continue;  // a session section
    const chain::JsonValue* b = root->member(s.presetKey);
    if (b && b->kind != chain::JsonValue::Kind::Object) return false;
  }
  if (params) {
    HeadState head;
    head.preset = true;
    for (int i = 0; i < kNumParams; ++i) {
      head.values[i] = kParams[i].def;
      head.given[i] = numberOf(params->member(kParams[i].key), head.values[i]);
    }
    impl_->applyHead(head, false);
  }
  for (const SectionInfo& s : kSections) {
    if (!s.presetKey) continue;
    const chain::JsonValue* b = root->member(s.presetKey);
    if (!b) continue;
    for (int id = 0; id < kNumChainParams; ++id) {
      if (id < kNumParams || !s.has(id)) continue;
      double v = info(id).def;
      numberOf(b->member(info(id).key), v);
      setParameter(id, v);
    }
  }
  return true;
}

}  // namespace magmora
