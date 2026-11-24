// Magmora DSP - parameters of the blocks around the head. Append only; never renumber.
// The ids continue the head's (Params.h) from kNumParams = 22, so one integer space covers the whole chain. They
// live in their own table so that Params.h, kNumParams and everything built on them stay as they were.
#pragma once
#include "Params.h"

namespace magmora {

enum ChainParamId : int {
  // pedal (before the head, inside its oversampler; XP1, XP2)
  kPedalOn = kNumParams,  // bool
  kPedalDrive,            // 0..10
  kPedalTone,             // 0..10
  kPedalLevel,            // 0..10
  // EQ (XP3): runs inside the head's oversampled loop, after the amp
  kEqOn,                  // bool
  kEqPosition,            // enum EqPosition; no audible effect while every block after the head is linear
  kEqHighPassFreq,        // Hz
  kEqHighPassSlope,       // enum FilterSlope
  kEqLowPassFreq,         // Hz
  kEqLowPassSlope,        // enum FilterSlope
  kEqBand1Freq, kEqBand1Gain, kEqBand1Q, kEqBand1Type,  // Hz, dB, Q, enum BandType (outer band: bell or shelf)
  kEqBand2Freq, kEqBand2Gain, kEqBand2Q,
  kEqBand3Freq, kEqBand3Gain, kEqBand3Q,
  kEqBand4Freq, kEqBand4Gain, kEqBand4Q, kEqBand4Type,
  // cabinet and microphones (XP5, XP6)
  kCabOn,                 // bool
  kCabFloor,              // unused since the cabinet plays responses only: kept, hidden (Chain::parameterHidden)
  kMic1On, kMic1Type, kMic1Position, kMic1Distance, kMic1Angle, kMic1Level, kMic1Pan, kMic1Polarity,
  kMic2On, kMic2Type, kMic2Position, kMic2Distance, kMic2Angle, kMic2Level, kMic2Pan, kMic2Polarity,
  kMicAlign,              // bool: remove the difference in arrival time
  kMicOffset,             // mm, put back on purpose
  // the former user response file. Unused since the cabinet slots take files: the file is a slot's source. Hidden; old states
  // with it on load as slot 2 = that file.
  kUserIrOn,              // bool
  kUserIrLevel,           // dB
  // FX slot (reserved, empty in this round)
  kFxOn,                  // bool
  // cabinet slots' sources: 0 a microphone of the loaded pack, 1 the slot's response file
  kMic1Source, kMic2Source,
  // the head itself on or off: off, the signal passes pedal, EQ and cabinet without the amp (Engine::setAmpOn).
  // Saved in its own state section ("amp "); a preset carries it in "params", with the head's other values.
  kAmpOn,                 // bool
  // the pitch transposer at the input (added 2026-11-24): semitones, -12..+12. 0 takes the transposer out of the
  // signal path (no delay, no CPU); any other value puts it in, at the mode's delay. Whether it is in, and its mode,
  // are structural (as kQuality): they take effect at Chain::applyTransposeStructure() (also called by prepare and
  // applyStructuralParams), where the latency changes. Moving between non-zero values is wait-free and crossfaded.
  // Both are session settings: a preset never carries them. Saved in their own state section ("trns").
  kTranspose,             // integer semitones
  kTransposeMode,         // enum TransposeMode; not automatable
  kNumChainParams
};

enum class TransposeMode : int { Tight = 0, Standard = 1 };  // about 5.3 ms and 10.7 ms of delay at any rate
enum class EqPosition : int { BeforeCabinet = 0, AfterCabinet = 1 };
enum class FilterSlope : int { Off = 0, Slope12 = 1, Slope24 = 2 };  // dB per octave
enum class BandType : int { Bell = 0, Shelf = 1 };
// Superseded: kMic1Type / kMic2Type index the loaded pack's microphones (Chain::cabinetMicName).
enum class MicType : int { Dynamic57 = 0, Dynamic421 = 1, Dynamic58 = 2 };
inline constexpr int kMaxCabinetMics = 16;

// Indexed by id - kNumParams. Every block's "on" defaults to off, and the head's (amp_on) to on: a session from
// before the chain, or from before the head had its own switch, sounds as it did.
// Cabinet controls are smoothed through response crossfades and none is structural.
inline constexpr ParamInfo kChainParams[static_cast<int>(kNumChainParams) - static_cast<int>(kNumParams)] = {
  {ParamId(kPedalOn),         "pedal_on",         "Pedal",          "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kPedalDrive),      "pedal_drive",      "Drive",          "",    0.0,  10.0,     5.0,  0.01, Taper::Linear,     true, true},
  {ParamId(kPedalTone),       "pedal_tone",       "Tone",           "",    0.0,  10.0,     5.0,  0.01, Taper::Linear,     true, true},
  {ParamId(kPedalLevel),      "pedal_level",      "Level",          "",    0.0,  10.0,     5.0,  0.01, Taper::Linear,     true, true},
  {ParamId(kEqOn),            "eq_on",            "EQ",             "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kEqPosition),      "eq_position",      "EQ Position",    "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kEqHighPassFreq),  "eq_hp_freq",       "High-Pass",      "Hz", 20.0, 1000.0,   80.0,  0.1, Taper::Logarithmic, true, true},
  {ParamId(kEqHighPassSlope), "eq_hp_slope",      "High-Pass Slope","",    0.0,   2.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kEqLowPassFreq),   "eq_lp_freq",       "Low-Pass",       "Hz", 1000.0, 20000.0, 12000.0, 1.0, Taper::Logarithmic, true, true},
  {ParamId(kEqLowPassSlope),  "eq_lp_slope",      "Low-Pass Slope", "",    0.0,   2.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kEqBand1Freq),     "eq_b1_freq",       "Band 1",         "Hz", 30.0,  500.0,   100.0, 0.1, Taper::Logarithmic, true, true},
  {ParamId(kEqBand1Gain),     "eq_b1_gain",       "Band 1 Gain",    "dB", -15.0,  15.0,     0.0, 0.1, Taper::Linear,      true, true},
  {ParamId(kEqBand1Q),        "eq_b1_q",          "Band 1 Q",       "",    0.3,  10.0,    0.71, 0.01, Taper::Logarithmic, true, true},
  {ParamId(kEqBand1Type),     "eq_b1_type",       "Band 1 Type",    "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kEqBand2Freq),     "eq_b2_freq",       "Band 2",         "Hz", 100.0, 2000.0,  400.0, 0.1, Taper::Logarithmic, true, true},
  {ParamId(kEqBand2Gain),     "eq_b2_gain",       "Band 2 Gain",    "dB", -15.0,  15.0,     0.0, 0.1, Taper::Linear,      true, true},
  {ParamId(kEqBand2Q),        "eq_b2_q",          "Band 2 Q",       "",    0.3,  10.0,    0.71, 0.01, Taper::Logarithmic, true, true},
  {ParamId(kEqBand3Freq),     "eq_b3_freq",       "Band 3",         "Hz", 400.0, 8000.0, 1600.0, 1.0, Taper::Logarithmic, true, true},
  {ParamId(kEqBand3Gain),     "eq_b3_gain",       "Band 3 Gain",    "dB", -15.0,  15.0,     0.0, 0.1, Taper::Linear,      true, true},
  {ParamId(kEqBand3Q),        "eq_b3_q",          "Band 3 Q",       "",    0.3,  10.0,    0.71, 0.01, Taper::Logarithmic, true, true},
  {ParamId(kEqBand4Freq),     "eq_b4_freq",       "Band 4",         "Hz", 1000.0, 16000.0, 5000.0, 1.0, Taper::Logarithmic, true, true},
  {ParamId(kEqBand4Gain),     "eq_b4_gain",       "Band 4 Gain",    "dB", -15.0,  15.0,     0.0, 0.1, Taper::Linear,      true, true},
  {ParamId(kEqBand4Q),        "eq_b4_q",          "Band 4 Q",       "",    0.3,  10.0,    0.71, 0.01, Taper::Logarithmic, true, true},
  {ParamId(kEqBand4Type),     "eq_b4_type",       "Band 4 Type",    "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kCabOn),           "cab_on",           "Cabinet",        "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kCabFloor),        "cab_floor",        "Floor",          "",    0.0,   1.0,     1.0,  1.0, Taper::Stepped,     false, true},  // hidden
  {ParamId(kMic1On),          "mic1_on",          "Mic 1",          "",    0.0,   1.0,     1.0,  1.0, Taper::Stepped,     true, true},
  {ParamId(kMic1Type),        "mic1_type",        "Mic 1 Type",     "",    0.0,  15.0,     0.0,  1.0, Taper::Stepped,     true, true},
  {ParamId(kMic1Position),    "mic1_position",    "Mic 1 Position", "",    0.0,   1.0,     0.3, 0.001, Taper::Linear,     true, true},
  {ParamId(kMic1Distance),    "mic1_distance",    "Mic 1 Distance", "mm",  0.0, 300.0,    25.0,  0.5, Taper::Linear,     true, true},
  {ParamId(kMic1Angle),       "mic1_angle",       "Mic 1 Angle",    "deg", 0.0,  90.0,     0.0,  0.5, Taper::Linear,     true, true},
  {ParamId(kMic1Level),       "mic1_level",       "Mic 1 Level",    "dB", -36.0,  12.0,     0.0, 0.1, Taper::Decibel,     true, true},
  {ParamId(kMic1Pan),         "mic1_pan",         "Mic 1 Pan",      "",   -1.0,   1.0,     0.0, 0.01, Taper::Linear,     true, true},
  {ParamId(kMic1Polarity),    "mic1_polarity",    "Mic 1 Polarity", "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, true},
  {ParamId(kMic2On),          "mic2_on",          "Mic 2",          "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, true},
  {ParamId(kMic2Type),        "mic2_type",        "Mic 2 Type",     "",    0.0,  15.0,     1.0,  1.0, Taper::Stepped,     true, true},
  {ParamId(kMic2Position),    "mic2_position",    "Mic 2 Position", "",    0.0,   1.0,     0.5, 0.001, Taper::Linear,     true, true},
  {ParamId(kMic2Distance),    "mic2_distance",    "Mic 2 Distance", "mm",  0.0, 300.0,    25.0,  0.5, Taper::Linear,     true, true},
  {ParamId(kMic2Angle),       "mic2_angle",       "Mic 2 Angle",    "deg", 0.0,  90.0,     0.0,  0.5, Taper::Linear,     true, true},
  {ParamId(kMic2Level),       "mic2_level",       "Mic 2 Level",    "dB", -36.0,  12.0,     0.0, 0.1, Taper::Decibel,     true, true},
  {ParamId(kMic2Pan),         "mic2_pan",         "Mic 2 Pan",      "",   -1.0,   1.0,     0.0, 0.01, Taper::Linear,     true, true},
  {ParamId(kMic2Polarity),    "mic2_polarity",    "Mic 2 Polarity", "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, true},
  {ParamId(kMicAlign),        "mic_align",        "Align",          "",    0.0,   1.0,     1.0,  1.0, Taper::Stepped,     true, true},
  {ParamId(kMicOffset),       "mic_offset",       "Offset",         "mm", -50.0,  50.0,     0.0,  0.1, Taper::Linear,     true, true},
  {ParamId(kUserIrOn),        "user_ir_on",       "User IR",        "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     false, false},  // hidden
  {ParamId(kUserIrLevel),     "user_ir_level",    "User IR Level",  "dB", -36.0,  12.0,     0.0, 0.1, Taper::Decibel,     false, true},   // hidden
  {ParamId(kFxOn),            "fx_on",            "FX",             "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kMic1Source),      "mic1_source",      "Mic 1 Source",   "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     false, true},
  {ParamId(kMic2Source),      "mic2_source",      "Mic 2 Source",   "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     false, true},
  {ParamId(kAmpOn),           "amp_on",           "Amp",            "",    0.0,   1.0,     1.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kTranspose),       "transpose",        "Transpose",      "st", -12.0,  12.0,     0.0,  1.0, Taper::Stepped,     true, false},
  {ParamId(kTransposeMode),   "transpose_mode",   "Transpose Mode", "",    0.0,   1.0,     0.0,  1.0, Taper::Stepped,     false, false},
};

}  // namespace magmora
