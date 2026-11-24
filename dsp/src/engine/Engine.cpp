// Implementation of the stable interface in magmora/Engine.h.
//
// Everything below process() runs host sample by host sample from a single state machine, so the output cannot
// depend on how the host cuts the signal into blocks.
#include <magmora/Engine.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <vector>

#include "engine/Calibration.h"
#include "engine/DenormalGuard.h"
#include "engine/DeveloperOptions.h"
#include "engine/Gate.h"
#include "engine/ParamSmoother.h"
#include "engine/Quality.h"
#include "engine/RelayMute.h"
#include "graph/AmpGraph.h"
#include "resample/Oversampler.h"

namespace magmora {
namespace model {
#include "circuit/ModelData.inc"
}

namespace engine {

WorkCounters& workCounters() {
  static WorkCounters counters;
  return counters;
}

namespace {
// The developer counters (DeveloperOptions.h), only while a tool has switched them on.
void count(std::atomic<long> WorkCounters::*counter, long n = 1) {
  WorkCounters& c = workCounters();
  if (n != 0 && c.on.load(std::memory_order_relaxed)) (c.*counter).fetch_add(n, std::memory_order_relaxed);
}
}  // namespace

}  // namespace engine

namespace {

constexpr double kMinimumRate = 44100.0, kMaximumRate = 192000.0;

// The prototype's DC solve stands a 1 pS conductance in for each capacitor. The transient then starts with that
// current missing, which is a 0.1 V bump at the speaker. A smaller stand-in is the true fixed point.
constexpr double kDcCapLeak = 1e-18;

constexpr double kPowerAveragingSeconds = 0.050;

// Recovery budget (graph/Cluster.h), in device evaluations spent on failed solves and their recoveries, scaled with
// the internal rate: kRecoveryPerInternalSample per internal sample and a ceiling of kRecoveryCeilingPerFactor times the
// oversampling factor (at 48 kHz Standard, 6x: 20 per host sample and 400). Chosen on the review's stress (lead, every
// knob at 10, full-scale noise, 64-sample blocks, 48 / 176.4 / 192 kHz) and on gate G4's tones. Measured on the way:
// 12 per host sample at 6x starved the 10 kHz, 10 mV tone (it needs about 16: 20 003 fallbacks, the 10 kHz band
// failing the aliasing gate); a ceiling of 1200 or 2000 let one High block at 48 kHz reach 198 to 200 %; the 6x allowance at 1x
// (176.4 / 192 kHz) left the worst Standard block at 92 %. The ordinary Newton work on noise sets most of the worst block.
constexpr double kRecoveryPerInternalSample = 20.0 / 6.0;
constexpr double kRecoveryCeilingPerFactor = 400.0 / 6.0;

// The head's own on/off: a raised-cosine crossfade of this length, as the pedal's bypass (pedal/PedalStage.cpp).
constexpr double kAmpFadeSeconds = 0.012;
constexpr double kPi = 3.14159265358979323846;

static_assert(std::atomic<double>::is_always_lock_free, "setParam and process must be wait-free");

}  // namespace

struct Engine::Impl {
  struct Channel {
    graph::AmpGraph amp;
    resample::Oversampler oversampler;
    engine::Gate gate;
    const double* speaker = nullptr;
    const double* secondary = nullptr;
    const double* plateSupply = nullptr;
    double idleSupply = 0.0;
    long nonFiniteSeen = 0, fallbacksSeen = 0, rebuildsSeen = 0, retriesSeen = 0;
  };

  Impl() {
    for (int id = 0; id < kNumParams; ++id) {
      params[id].store(kParams[id].def, std::memory_order_relaxed);
      snapshot[id] = kParams[id].def;
    }
    for (int id = 0; id < kNumParams; ++id) {
      knobOf[id] = circuit::findKnob(kParams[id].key);
      if (knobOf[id] >= 0) knobIds[static_cast<size_t>(knobOf[id])] = id;
    }
    inputVolts.store(Calibration().inputVoltsPerFS, std::memory_order_relaxed);
    outputVolts.store(Calibration().outputVoltsAtFS, std::memory_order_relaxed);
  }

  bool prepare(double rate, int maxBlock, int numChannels);
  void reset();
  template <class Sample>
  void process(const Sample* const* in, Sample* const* out, int frames);

  void readParams();
  unsigned switchesFromParams() const;
  void restartCircuit();
  void restartAmp();   // the circuit only (the head switched back on while the loop around it runs)
  void restartPath();  // the gate and the resamplers only
  void snapKnobs();
  void advanceControls();
  void upChannel(Channel&, double x, double* internal);
  void ampChannel(Channel&, double* internal);
  engine::InternalStage* stage = nullptr;     // Chain's blocks at the internal rate after the amp (the EQ)
  engine::InternalStage* preStage = nullptr;  // and before it (the pedal); null: none
  std::atomic<bool> ampWanted{true};          // the head on; any thread
  long stageFallbacksSeen = 0;

  int enumValue(ParamId id) const {
    return static_cast<int>(std::lround(params[id].load(std::memory_order_relaxed)));
  }

  // any thread
  std::atomic<double> params[kNumParams];
  std::atomic<double> inputVolts, outputVolts;
  std::atomic<int> valueSetChoice{circuit::kDefaultValueSet};  // structural: read by prepare(); set from any thread
  Meters meters;

  // set by prepare()
  bool ready = false;
  double sampleRate = 0.0;
  int maxBlockSize = 0, channels = 0, cores = 0, factor = 1, latency = 0;
  ChannelMode mode = ChannelMode::MonoLeft;
  unsigned structural = 0;
  int knobOf[kNumParams] = {};
  std::array<int, circuit::kNumKnobs> knobIds{};
  double powerScale = 0.0, powerCoefficient = 1.0;
  std::unique_ptr<Channel> channel[2];

  // audio thread
  double snapshot[kNumParams];
  circuit::Controls controls;
  unsigned wantedSwitches = 0;
  bool knobsMoving = false, idle = false, powerOn = true, operate = true;
  std::array<engine::ParamSmoother, circuit::kNumKnobs> knobs;
  engine::ParamSmoother trimDb, levelDb;
  double trimGain = 1.0, levelGain = 1.0;
  engine::RelayMute mute;
  double volts = 1.0, perVolt = 0.0, power = 0.0, inputPeak = 0.0;
  uint32_t fallbackTotal = 0;
  // the head's own on/off, audio thread
  bool ampRunning = true;    // the amp's circuit is computed; false once the head is off and faded out
  bool pathRunning = false;  // the gate, resamplers and stages hold current state (false: stale, restart them)
  double ampFade = 1.0, ampStep = 1.0;  // 1: the head; 0: bypassed (the pedal and the EQ only)
};

unsigned Engine::Impl::switchesFromParams() const {
  using namespace circuit::netdata;
  unsigned bits = structural;
  if (snapshot[kChannel] >= 0.5) bits |= kSwitch_channel;
  if (snapshot[kCrunch] >= 0.5) bits |= kSwitch_crunch;
  if (snapshot[kBright] >= 0.5) bits |= kSwitch_bright;
  if (snapshot[kInputJack] >= 0.5) bits |= kSwitch_input;
  return bits;
}

void Engine::Impl::readParams() {
  for (int id = 0; id < kNumParams; ++id) snapshot[id] = params[id].load(std::memory_order_relaxed);
  wantedSwitches = switchesFromParams();
  powerOn = snapshot[kPower] >= 0.5;
  operate = snapshot[kStandby] >= 0.5;
  volts = inputVolts.load(std::memory_order_relaxed);
  perVolt = 1.0 / outputVolts.load(std::memory_order_relaxed);
}

void Engine::Impl::snapKnobs() {
  for (int k = 0; k < circuit::kNumKnobs; ++k) {
    const size_t i = static_cast<size_t>(k);
    knobs[i].snap(snapshot[knobIds[i]]);
    controls.knobs[i] = knobs[i].value();
  }
  knobsMoving = false;
}

bool Engine::Impl::prepare(double rate, int maxBlock, int numChannels) {
  ready = false;
  if (!(rate >= kMinimumRate && rate <= kMaximumRate) || maxBlock < 1 || numChannels < 1 || numChannels > 2) return false;
  sampleRate = rate;
  maxBlockSize = maxBlock;
  channels = numChannels;

  const size_t quality = static_cast<size_t>(std::clamp(enumValue(kQuality), 0, 2));
  const bool minimumPhase = enumValue(kPhaseMode) == static_cast<int>(PhaseMode::Minimum);
  mode = static_cast<ChannelMode>(std::clamp(enumValue(kChannelMode), 0, 2));
  structural = enumValue(kLoad) == static_cast<int>(Load::Resistive) ? circuit::netdata::kSwitch_load : 0u;
  cores = (channels == 2 && mode == ChannelMode::DualMono) ? 2 : 1;
  factor = engine::oversamplingFactor(quality, rate);

  readParams();
  controls.switches = wantedSwitches;
  snapKnobs();

  graph::GraphOptions options;
  options.sampleRate = rate * factor;
  options.valueSet = valueSetChoice.load(std::memory_order_relaxed);
  options.dcCapLeak = kDcCapLeak;
  options.probes = {"sec0"};  // with the speaker node and Rsec: the current into the load, for the power meter
  options.companionAtDefaults = true;
  options.recoveryPerSample = kRecoveryPerInternalSample;
  options.recoveryCap = kRecoveryCeilingPerFactor * factor;

  channel[1].reset();
  for (int c = 0; c < cores; ++c) {
    channel[c] = std::make_unique<Channel>();
    Channel& ch = *channel[c];
    if (!ch.amp.prepare(options, controls)) return false;
    ch.speaker = ch.amp.output("P", "spk");
    ch.secondary = ch.amp.probe("sec0");
    ch.plateSupply = ch.amp.output("S", "v_b");
    if (!ch.speaker) return false;
    ch.idleSupply = ch.plateSupply ? *ch.plateSupply : 0.0;
    ch.oversampler.prepare(rate, factor, minimumPhase);
    ch.gate.prepare(rate);
  }
  latency = channel[0]->oversampler.latency();

  powerScale = 0.0;
  const circuit::PartitionedAmp& amp = channel[0]->amp.amp();
  if (const int p = amp.find("P"); p >= 0 && channel[0]->secondary)
    if (const circuit::Item* r = amp.clusters[static_cast<size_t>(p)].findItem("Rsec"))
      powerScale = 1.0 / r->variants[0].value;
  powerCoefficient = 1.0 - std::exp(-1.0 / (kPowerAveragingSeconds * rate));

  for (auto& k : knobs) k.prepare(rate);
  trimDb.prepare(rate);
  levelDb.prepare(rate);
  mute.prepare(rate);
  ampStep = 1.0 / std::max(1.0, kAmpFadeSeconds * rate);

  ready = true;
  reset();
  return true;
}

void Engine::Impl::restartCircuit() {
  engine::count(&engine::WorkCounters::restarts);
  restartAmp();
  restartPath();
  pathRunning = true;
}

void Engine::Impl::restartAmp() {
  for (int c = 0; c < cores; ++c) channel[c]->amp.reset(controls);
  power = 0.0;
}

void Engine::Impl::restartPath() {
  for (int c = 0; c < cores; ++c) {
    Channel& ch = *channel[c];
    ch.oversampler.reset();
    ch.gate.setThreshold(snapshot[kGateThreshold], kParams[kGateThreshold].min);
    ch.gate.reset();
  }
}

void Engine::Impl::reset() {
  if (!ready) return;
  readParams();
  controls.switches = wantedSwitches;
  snapKnobs();
  trimDb.snap(snapshot[kInputTrim]);
  levelDb.snap(snapshot[kOutputLevel]);
  trimGain = engine::dbToGain(trimDb.value());
  levelGain = engine::dbToGain(levelDb.value());
  restartCircuit();
  idle = !powerOn;
  mute.snap(!powerOn || !operate);
  ampRunning = ampWanted.load(std::memory_order_relaxed);
  ampFade = ampRunning ? 1.0 : 0.0;
}

void Engine::Impl::advanceControls() {
  if (trimDb.moving()) trimGain = engine::dbToGain(trimDb.next());
  if (levelDb.moving()) levelGain = engine::dbToGain(levelDb.next());
  if (!knobsMoving || idle || !ampRunning) return;
  bool still = false;
  for (size_t k = 0; k < knobs.size(); ++k) {
    if (!knobs[k].moving()) continue;
    controls.knobs[k] = knobs[k].next();
    still = still || knobs[k].moving();
  }
  for (int c = 0; c < cores; ++c) {
    channel[c]->amp.setKnobs(controls);       // exact at every host sample, so there are no steps to hear
    if (!still) channel[c]->amp.settle();     // a full rebuild once the movement is over
  }
  if (!still) engine::count(&engine::WorkCounters::settles);
  knobsMoving = still;
}

// Up to the internal rate: volts at the input jack.
void Engine::Impl::upChannel(Channel& ch, double x, double* internal) {
  x *= trimGain;
  inputPeak = std::max(inputPeak, std::fabs(x));
  x = ch.gate.process(x) * volts;
  ch.oversampler.up(x, internal);
}

// Through the amp; the speaker voltage at the internal rate is left in `internal`.
void Engine::Impl::ampChannel(Channel& ch, double* internal) {
  double watts = 0.0;
  for (int k = 0; k < factor; ++k) {
    ch.amp.processSample(internal[k]);
    const double speaker = *ch.speaker;
    if (ch.secondary) watts += speaker * (*ch.secondary - speaker);
    internal[k] = speaker;
  }
  if (&ch == channel[0].get()) power += powerCoefficient * (watts * powerScale / factor - power);

  const long bad = ch.amp.nonFinite();
  if (bad != ch.nonFiniteSeen) {  // the cluster went back to its operating point: do that under the relay mute
    ch.nonFiniteSeen = bad;
    mute.close();
  }
}

template <class Sample>
void Engine::Impl::process(const Sample* const* in, Sample* const* out, int frames) {
  if (!ready || frames <= 0) return;
  const engine::DenormalGuard denormals;

  readParams();
  for (size_t k = 0; k < knobs.size(); ++k) {
    knobs[k].setTarget(snapshot[knobIds[k]]);
    knobsMoving = knobsMoving || knobs[k].moving();
  }
  trimDb.setTarget(snapshot[kInputTrim]);
  levelDb.setTarget(snapshot[kOutputLevel]);
  for (int c = 0; c < cores; ++c)
    channel[c]->gate.setThreshold(snapshot[kGateThreshold], kParams[kGateThreshold].min);

  const bool headOn = ampWanted.load(std::memory_order_relaxed);
  inputPeak = 0.0;
  double outputPeak = 0.0;
  for (int n = 0; n < frames; ++n) {
    double x[2] = {0.0, 0.0};
    for (int c = 0; c < channels; ++c) {
      const double s = static_cast<double>(in[c][n]);
      x[c] = std::isfinite(s) ? std::clamp(s, -engine::kInputClamp, engine::kInputClamp) : 0.0;
    }
    if (channels == 2 && mode == ChannelMode::MonoSum) x[0] = 0.5 * (x[0] + x[1]);

    if (headOn && !ampRunning) {  // the head switched back on: its amp starts from the operating point
      ampRunning = true;
      if (!idle && !powerOn) {
        idle = true;  // its power switch is off: it stays silent
      } else if (!idle) {
        controls.switches = wantedSwitches;
        snapKnobs();
        engine::count(&engine::WorkCounters::restarts);
        restartAmp();
        mute.snap(!operate);
      }
    }
    if (idle && powerOn) {  // power on: start from the operating point
      idle = false;
      if (ampRunning) {  // a head that is off starts when it is switched back on
        controls.switches = wantedSwitches;
        snapKnobs();
        if (pathRunning) {  // the loop around it is running for the bypass: leave it alone
          engine::count(&engine::WorkCounters::restarts);
          restartAmp();
        } else {
          restartCircuit();
        }
        mute.close();
      }
    }
    advanceControls();

    double y[2] = {0.0, 0.0};
    const bool ampComputed = !idle && ampRunning;
    const bool path = ampComputed || ampFade < 1.0;  // the head, or its bypass
    if (path) {
      if (!pathRunning) restartPath();  // its state is from before it stopped
      const bool switching = ampComputed && wantedSwitches != controls.switches;
      double gain = 0.0;
      if (ampComputed) {
        gain = mute.next(switching || !operate || !powerOn);
        if (mute.closed()) {
          if (switching) {  // the relays move while nothing can be heard
            controls.switches = wantedSwitches;
            for (int c = 0; c < cores; ++c) channel[c]->amp.setSwitches(controls);
            engine::count(&engine::WorkCounters::relaySwitches);
            mute.close();
          }
          if (!powerOn) idle = true;
        }
      }
      const double scale = perVolt * levelGain;
      double internal[2][resample::kMaxFactor];
      for (int c = 0; c < cores; ++c) upChannel(*channel[c], x[c], internal[c]);
      if (preStage && preStage->active()) {  // the pedal, ahead of cluster A (XP2)
        double* lanes[2] = {internal[0], cores == 2 ? internal[1] : nullptr};
        preStage->process(lanes, factor);
      }
      if (ampComputed && ampFade >= 1.0) {  // the head, exactly as before it had its own switch
        for (int c = 0; c < cores; ++c) ampChannel(*channel[c], internal[c]);
        if (stage && stage->active()) {
          double* lanes[2] = {internal[0], cores == 2 ? internal[1] : nullptr};
          stage->process(lanes, factor);
        }
        for (int c = 0; c < cores; ++c) {
          const double speaker = channel[c]->oversampler.down(internal[c]);
          if (mute.closed()) gain = 0.0;
          const double v = speaker * scale * gain;
          y[c] = std::isfinite(v) ? std::clamp(v, -engine::kOutputClamp, engine::kOutputClamp) : 0.0;
          outputPeak = std::max(outputPeak, std::fabs(y[c]));
        }
      } else {
        // The head fading out or in, or off. Its share (behind the relay mute) and the volts at the input jack,
        // brought to the speaker's scale so that they leave at unity, are mixed ahead of the EQ and the downsampler.
        const double w = 0.5 - 0.5 * std::cos(kPi * ampFade);
        const double through = (1.0 - w) / (volts * perVolt);
        const double share = ampComputed && !mute.closed() ? w * gain : 0.0;
        for (int c = 0; c < cores; ++c) {
          double* lane = internal[c];
          if (ampComputed) {
            double jack[resample::kMaxFactor];
            std::copy_n(lane, factor, jack);
            ampChannel(*channel[c], lane);
            for (int k = 0; k < factor; ++k) lane[k] = share * lane[k] + through * jack[k];
          } else {
            for (int k = 0; k < factor; ++k) lane[k] *= through;
          }
        }
        if (stage && stage->active()) {
          double* lanes[2] = {internal[0], cores == 2 ? internal[1] : nullptr};
          stage->process(lanes, factor);
        }
        for (int c = 0; c < cores; ++c) {
          const double v = channel[c]->oversampler.down(internal[c]) * scale;
          y[c] = std::isfinite(v) ? std::clamp(v, -engine::kOutputClamp, engine::kOutputClamp) : 0.0;
          outputPeak = std::max(outputPeak, std::fabs(y[c]));
        }
      }
      if (cores == 1) y[1] = y[0];
    }
    pathRunning = path;
    if (headOn ? ampFade < 1.0 : ampFade > 0.0) ampFade = std::clamp(ampFade + (headOn ? ampStep : -ampStep), 0.0, 1.0);
    if (!headOn && ampFade == 0.0 && ampRunning) {  // faded out: the circuit is no longer computed
      ampRunning = false;
      power = 0.0;
    }
    for (int c = 0; c < channels; ++c) out[c][n] = static_cast<Sample>(y[c]);
  }

  // meters, once per block
  for (engine::InternalStage* st : {preStage, stage})
    if (st) {
      const long now = st->fallbacks();
      if (now > stageFallbacksSeen) fallbackTotal += static_cast<uint32_t>(now - stageFallbacksSeen);
      stageFallbacksSeen = now;
      break;  // only the pre-stage (the pedal) has a solver
    }
  const Channel& first = *channel[0];
  for (int c = 0; c < cores; ++c) {
    Channel& ch = *channel[c];
    const long now = ch.amp.fallbacks(), rebuilt = ch.amp.rebuilds(), retried = ch.amp.retries();
    fallbackTotal += static_cast<uint32_t>(now - ch.fallbacksSeen);
    ch.fallbacksSeen = now;
    engine::count(&engine::WorkCounters::clusterRebuilds, rebuilt - ch.rebuildsSeen);
    engine::count(&engine::WorkCounters::recoveries, retried - ch.retriesSeen);
    ch.rebuildsSeen = rebuilt;
    ch.retriesSeen = retried;
  }
  double sag = 0.0;
  if (!idle && ampRunning && first.plateSupply && first.idleSupply > 0.0)
    sag = std::clamp((first.idleSupply - *first.plateSupply) / (engine::kSagAtFullScale * first.idleSupply), 0.0, 1.0);
  meters.inputPeak.store(static_cast<float>(inputPeak), std::memory_order_relaxed);
  meters.outputPeak.store(static_cast<float>(outputPeak), std::memory_order_relaxed);
  meters.platePowerW.store(idle || !ampRunning ? 0.f : static_cast<float>(std::max(power, 0.0)), std::memory_order_relaxed);
  meters.supplySag.store(static_cast<float>(sag), std::memory_order_relaxed);
  meters.gateGain.store(static_cast<float>(first.gate.gain()), std::memory_order_relaxed);
  meters.solverFallbacks.store(fallbackTotal, std::memory_order_relaxed);
}

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

bool Engine::prepare(double sampleRate, int maxBlockSize, int numChannels) {
  return impl_->prepare(sampleRate, maxBlockSize, numChannels);
}

void Engine::setCalibration(const Calibration& c) {
  if (std::isfinite(c.inputVoltsPerFS) && c.inputVoltsPerFS > 0.0)
    impl_->inputVolts.store(c.inputVoltsPerFS, std::memory_order_relaxed);
  if (std::isfinite(c.outputVoltsAtFS) && c.outputVoltsAtFS > 0.0)
    impl_->outputVolts.store(c.outputVoltsAtFS, std::memory_order_relaxed);
}

int Engine::latencySamples() const { return impl_->ready ? impl_->latency : 0; }

bool Engine::setValueSet(int index) {
  if (index < 0 || index >= circuit::kNumValueSets) return false;
  impl_->valueSetChoice.store(index, std::memory_order_relaxed);
  return true;
}

int Engine::valueSet() const { return impl_->valueSetChoice.load(std::memory_order_relaxed); }
int Engine::valueSetCount() { return circuit::kNumValueSets; }
int Engine::defaultValueSet() { return circuit::kDefaultValueSet; }

const char* Engine::valueSetName(int index) {
  return index >= 0 && index < circuit::kNumValueSets ? circuit::netdata::kValueSets[index].name : nullptr;
}

const char* Engine::valueSetModelHash(int index) {
  return index >= 0 && index < circuit::kNumValueSets ? circuit::netdata::kValueSets[index].modelHash : nullptr;
}

int Engine::findValueSet(const char* name) { return name ? circuit::findValueSet(name) : -1; }

bool Engine::qualityIsRealtime(Quality q) {
  const int k = static_cast<int>(q);
  return k >= 0 && k < static_cast<int>(engine::kQualities.size()) && engine::kQualities[static_cast<size_t>(k)].realtime;
}

double Engine::internalSampleRate() const { return impl_->ready ? impl_->sampleRate * impl_->factor : 0.0; }
int Engine::oversamplingFactor() const { return impl_->ready ? impl_->factor : 0; }
void Engine::setInternalStage(engine::InternalStage* stage) { impl_->stage = stage; }
void Engine::setPreStage(engine::InternalStage* stage) { impl_->preStage = stage; }
int Engine::ampCores() const { return impl_->ready ? impl_->cores : 0; }
void Engine::setAmpOn(bool on) { impl_->ampWanted.store(on, std::memory_order_relaxed); }
bool Engine::ampOn() const { return impl_->ampWanted.load(std::memory_order_relaxed); }

void Engine::applyStructuralParams() {
  if (impl_->ready) impl_->prepare(impl_->sampleRate, impl_->maxBlockSize, impl_->channels);
}

void Engine::setParam(ParamId id, double value) {
  if (id < 0 || id >= kNumParams || !std::isfinite(value)) return;
  impl_->params[id].store(std::clamp(value, kParams[id].min, kParams[id].max), std::memory_order_relaxed);
}

double Engine::getParam(ParamId id) const {
  return (id < 0 || id >= kNumParams) ? 0.0 : impl_->params[id].load(std::memory_order_relaxed);
}

void Engine::reset() {
  const engine::DenormalGuard denormals;
  impl_->reset();
}

void Engine::process(const float* const* in, float* const* out, int numFrames) { impl_->process(in, out, numFrames); }
void Engine::process(const double* const* in, double* const* out, int numFrames) { impl_->process(in, out, numFrames); }
const Meters& Engine::meters() const { return impl_->meters; }

const char* versionString() { return MAGMORA_VERSION; }
const char* modelHash() { return model::kModelHash; }

}  // namespace magmora
