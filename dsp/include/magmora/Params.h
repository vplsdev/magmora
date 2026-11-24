// Magmora DSP - parameter table. A stable interface: it is only appended to.
// Plugin, UI and tests depend on these ids, ranges and defaults. Append only; never renumber.
#pragma once

namespace magmora {

enum ParamId : int {
  kInputTrim = 0,   // dB, applied before the amp input
  kInputJack,       // enum InputJack
  kChannel,         // enum Channel
  kBright,          // bool, rhythm channel only (as on the hardware)
  kCrunch,          // bool, rhythm channel only
  kPreRhythm,       // 0..10
  kPreLead,         // 0..10
  kLow,             // 0..10
  kMid,             // 0..10
  kHigh,            // 0..10
  kPostRhythm,      // 0..10
  kPostLead,        // 0..10
  kResonance,       // 0..10
  kPresence,        // 0..10
  kOutputLevel,     // dB, after the speaker-terminal voltage is scaled to full scale
  kStandby,         // bool: true = operating, false = standby (output muted, engine keeps running)
  kPower,           // bool: false = amp off (silence, DSP idle). Host bypass is separate and passes audio.
  kGateThreshold,   // dB, input noise gate; minimum = off
  kLoad,            // enum Load
  kQuality,         // enum Quality (oversampling). Not automatable; changes latency.
  kPhaseMode,       // enum PhaseMode. Not automatable; changes latency.
  kChannelMode,     // enum ChannelMode (how a stereo bus is fed to the mono amp)
  kNumParams
};

enum class InputJack : int { High = 0, Normal = 1 };
enum class Channel : int { Rhythm = 0, Lead = 1 };
enum class Load : int { Reactive4x12 = 0, Resistive = 1 };
enum class Quality : int { Standard = 0, High = 1, Ultra = 2 };    // internal rate >= 176.4 / 352.8 / 705.6 kHz (Ultra: offline rendering)
enum class PhaseMode : int { Linear = 0, Minimum = 1 };            // Minimum reports zero latency
enum class ChannelMode : int { MonoLeft = 0, MonoSum = 1, DualMono = 2 };

enum class Taper : int { Linear, Stepped, Decibel, Logarithmic };  // Logarithmic (appended for Chain, XP0): equal ratios per travel, e.g. Hz

struct ParamInfo {
  ParamId id;
  const char* key;      // stable identifier for state files and tests
  const char* name;     // display name (plugin may override the text, not the meaning)
  const char* unit;
  double min, max, def, step;
  Taper taper;
  bool automatable;
  bool smoothed;        // true: the engine ramps it; false: applied with a short mute (relay-style)
};

// Indexed by ParamId.
inline constexpr ParamInfo kParams[kNumParams] = {
  {kInputTrim,     "input_trim",     "Input",       "dB", -24.0, 24.0,  0.0, 0.1, Taper::Decibel, true,  true},
  {kInputJack,     "input_jack",     "Input Jack",  "",     0.0,  1.0,  0.0, 1.0, Taper::Stepped, true,  false},
  {kChannel,       "channel",        "Channel",     "",     0.0,  1.0,  1.0, 1.0, Taper::Stepped, true,  false},
  {kBright,        "bright",         "Bright",      "",     0.0,  1.0,  0.0, 1.0, Taper::Stepped, true,  false},
  {kCrunch,        "crunch",         "Crunch",      "",     0.0,  1.0,  0.0, 1.0, Taper::Stepped, true,  false},
  {kPreRhythm,     "pre_rhythm",     "Pre Rhythm",  "",     0.0, 10.0,  5.0, 0.01, Taper::Linear, true,  true},
  {kPreLead,       "pre_lead",       "Pre Lead",    "",     0.0, 10.0,  5.0, 0.01, Taper::Linear, true,  true},
  {kLow,           "low",            "Low",         "",     0.0, 10.0,  5.0, 0.01, Taper::Linear, true,  true},
  {kMid,           "mid",            "Mid",         "",     0.0, 10.0,  5.0, 0.01, Taper::Linear, true,  true},
  {kHigh,          "high",           "High",        "",     0.0, 10.0,  5.0, 0.01, Taper::Linear, true,  true},
  {kPostRhythm,    "post_rhythm",    "Post Rhythm", "",     0.0, 10.0,  4.0, 0.01, Taper::Linear, true,  true},
  {kPostLead,      "post_lead",      "Post Lead",   "",     0.0, 10.0,  4.0, 0.01, Taper::Linear, true,  true},
  {kResonance,     "resonance",      "Resonance",   "",     0.0, 10.0,  5.0, 0.01, Taper::Linear, true,  true},
  {kPresence,      "presence",       "Presence",    "",     0.0, 10.0,  5.0, 0.01, Taper::Linear, true,  true},
  {kOutputLevel,   "output_level",   "Output",      "dB", -36.0, 12.0,  0.0, 0.1, Taper::Decibel, true,  true},
  {kStandby,       "standby",        "Standby",     "",     0.0,  1.0,  1.0, 1.0, Taper::Stepped, true,  false},
  {kPower,         "power",          "Power",       "",     0.0,  1.0,  1.0, 1.0, Taper::Stepped, true,  false},
  {kGateThreshold, "gate_threshold", "Gate",        "dB", -96.0, -20.0, -96.0, 0.1, Taper::Decibel, true, true},
  {kLoad,          "load",           "Load",        "",     0.0,  1.0,  0.0, 1.0, Taper::Stepped, false, false},
  {kQuality,       "quality",        "Quality",     "",     0.0,  2.0,  0.0, 1.0, Taper::Stepped, false, false},
  {kPhaseMode,     "phase_mode",     "Phase",       "",     0.0,  1.0,  0.0, 1.0, Taper::Stepped, false, false},
  {kChannelMode,   "channel_mode",   "Routing",     "",     0.0,  2.0,  0.0, 1.0, Taper::Stepped, false, false},
};

// The knob values 0..10 are front-panel positions. The mapping from position to pot resistance
// (audio / linear / "5%" tapers) is part of the circuit model and lives inside the engine.

}  // namespace magmora
