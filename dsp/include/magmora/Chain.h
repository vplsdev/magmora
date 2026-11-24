// Magmora DSP - the whole signal chain: pedal, head, EQ, cabinet, user response file, FX slot.
// A stable interface: it is only appended to.
//
// Chain owns an Engine (the head, unchanged) and the blocks around it. With every block off the chain's output is
// the engine's, bit for bit, at the engine's latency. Same threading model as Engine: prepare, structural changes
// and state on the main thread; parameters from any thread, wait-free; process on the audio thread, no allocation
// and no locks.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ChainParams.h"
#include "Engine.h"

namespace magmora {

enum class ChainBlock : int { Pedal = 0, Eq, Cabinet, UserIr, FxSlot, Amp /* the head itself */, Count };

// The cabinet's virtual microphone, as last built (XP6; added 2026-11-23). Main thread, for the UI.
struct CabinetMicStatus {
  int source = 0;                  // 0 a microphone of the pack, 1 the slot's response file
  bool on = false, found = false;  // found: the microphone index names a microphone of the loaded pack / a file is loaded
  std::string mic, speaker, variant;
  // 0: millimetres across the cone (documented positions); 1: an ordered path without millimetres (show it as
  // "brightness", never as a distance); 2: zone words (cap, cap edge, cone, edge); 3: files without a position.
  int kind = 3;
  double angleDeg = 0.0;           // the recorded angle in use (angle snaps)
  double xMm = 0.0, distMm = 0.0;  // kind 0: where the microphone is; distMm also for the others when documented
  double pathIndex = 0.0;          // kinds 1 to 3: position along the list, 0 .. count - 1
  bool morphX = false, morphDist = false;  // interpolated between recordings, else the nearest recording
  std::vector<std::string> files;  // the recordings in use
};
struct CabinetStatus {
  bool packLoaded = false;
  std::string folder, error;       // error: why the last load failed (shown by the plug-in), else empty
  std::string phase;               // "minimum", "aligned" or "raw" (from the pack's mapping file; default "aligned")
  int files = 0, skipped = 0;      // usable files; files that could not be read
  CabinetMicStatus mic[2];
  double measuredDelaySamples = 0.0;  // raw packs with both microphones on: microphone 2 against microphone 1
  int detectedPolarity = 1;           // of microphone 2 against microphone 1: shown, never applied
  double lastBuildMs = 0.0;
  long builds = 0;
};

class Chain {
public:
  Chain();
  ~Chain();
  Chain(const Chain&) = delete;
  Chain& operator=(const Chain&) = delete;

  // --- main thread, not real-time safe -------------------------------------------------------
  // Allocates everything. numInputChannels: 1 or 2 (passed on to Engine::prepare); the output is always stereo.
  bool prepare(double sampleRate, int maxBlockSize, int numInputChannels = 2);
  void setCalibration(const Calibration&);
  // engine().latencySamples() plus transposeLatencySamples(): every other block around the head adds exactly zero
  // (gate G-X2), and the head's bypass (amp_on off) runs through the same oversampled loop, so the latency does not
  // depend on any block's on. With the transposer out (kTranspose 0, the default) it equals the engine's.
  int latencySamples() const;
  // As Engine::applyStructuralParams (kQuality, kPhaseMode, kLoad, kChannelMode, the value set); also applies the
  // transposer's structure (applyTransposeStructure), at once and without a crossfade (audio is stopped).
  void applyStructuralParams();


  // The head. Value sets, calibration and meters are reached through it; parameters 0 .. kNumParams - 1 may be set
  // either here or through Chain::setParameter, with the same effect.
  Engine& engine();
  const Engine& engine() const;

  // --- parameters: one id space, 0 .. kNumChainParams - 1 ------------------------------------
  // Ids below kNumParams are the head's (Params.h); the rest are ChainParams.h. Plain units, clamped; any thread.
  void setParameter(int id, double value);
  double getParameter(int id) const;
  static int parameterCount();                   // kNumChainParams
  static const ParamInfo* parameterInfo(int id);  // null out of range
  static int findParameter(const char* key);      // -1 if unknown
  bool blockOn(ChainBlock) const;                 // that block's "on" parameter (ChainBlock::Amp: amp_on)

  // --- audio thread, real-time safe ------------------------------------------------------------
  void reset();
  // in: numInputChannels pointers; out: two pointers; may alias. frames <= maxBlockSize, may be 0.
  void process(const float* const* in, float* const* out, int frames);
  void process(const double* const* in, double* const* out, int frames);

  const Meters& meters() const;

  // --- state and presets (main thread) -------------------------------------------------------
  // Layout: u32 magic "SCCH", i32 kStateVersion, i32 section count, then per section: 4-byte tag, i32 version,
  // i32-length model hash, i32 payload size, payload. All integers and doubles in the machine's byte order, as the
  // head state has always been. Sections: "head" (the head state chunk exactly as the 0.1.0 plug-in writes it,
  // version 2, preset name and UI scale included), "pedl", "eq  ", "cab ", "uir ", "fx  " (key and value pairs of
  // that block; "uir " adds its file reference), and from 2026-11-24 "trns" (transpose and transpose_mode; a state
  // without it loads at 0 st, Tight). Unknown sections are kept and written back.
  // Version 2 adds the "amp " section (amp_on, the head's own on/off); a state without it loads with the head on.
  static constexpr int kStateVersion = 2;
  void getState(std::vector<uint8_t>& out) const;
  // Accepts a chain state, or a bare head state chunk of version 1 or 2 (a session from before the chain). A block
  // without a section is loaded off, at its defaults. Returns false, changing nothing, for a malformed input. A value
  // set or structural change takes effect at applyStructuralParams(), as for Engine.
  bool setState(const uint8_t* data, size_t size);
  // What setState would accept, without changing anything (any thread).
  static bool checkState(const uint8_t* data, size_t size);
  // A preset: JSON, one object per block ("params" for the head, as the 0.1.0 presets; then "pedal", "eq", "cab",
  // "user_ir", "fx"). A present object sets its block's preset parameters (absent keys take their defaults); an
  // absent object leaves the block as it is. The session parameters (input trim, load, quality, phase mode,
  // routing, transpose and its mode) are never set by a preset. False, changing nothing, for malformed JSON.
  bool applyPreset(const char* json, size_t size);
  static bool isSessionParameter(int id);

  // What the plug-in keeps in the head section next to the parameters (it was always part of the head chunk).
  void setSessionInfo(const char* presetName, float uiScale);
  const char* presetName() const;
  float uiScale() const;
  // The user response file's reference, as saved in the state (setState restores the reference only; the host
  // then calls loadUserIr with it, so that a missing file can be reported).
  void setUserIrReference(const char* reference);
  const char* userIrReference() const;

  // --- user response file (added 2026-11-23). Since the cabinet slots take files these act on slot 2's file; the slot's
  // source, on and level are the parameters kMic2Source, kMic2On and kMic2Level. --------------------------------
  // Main thread, while audio runs: read (WAV: PCM 8/16/24/32-bit or float 32/64, mono or stereo, any rate, at most
  // 10 s), resample to the host rate, build, and hand over to the audio thread, which crossfades to it in 20 ms.
  // Zero added latency. False with a reason, and the slot unchanged, for a file that cannot be used.
  bool loadUserIr(const char* path, std::string* error = nullptr);  // also sets userIrReference on success
  bool loadUserIrSamples(const float* const* channels, int numChannels, int frames, double sampleRate,
                         std::string* error = nullptr);
  void clearUserIr();  // back to no response (the slot passes the signal through)
  bool userIrLoaded() const;
  double userIrSeconds() const;
  int userIrChannels() const;
  // Normalised: scaled to unit energy (the loudest channel), so that responses of any level and length play at a
  // comparable loudness. A smoothed gain; saved in the state. Default on.
  void setUserIrNormalize(bool on);
  bool userIrNormalize() const;
  // Main-thread housekeeping: frees responses the audio thread has finished with. Call now and then (an idle timer);
  // loads, clears and prepare also do it.
  void idle();

  // --- cabinet: a virtual microphone on the user's own response files (added 2026-11-23) --------------------------
  // Main thread. Scans the folder (recursively) for WAV files, reads their positions from the names and from the
  // mapping file (mappingFile, or <folder>/magmora-positions.json if present), decodes them, and starts building
  // responses on a worker thread. The parameters kCabOn .. kMicOffset then act on it: kMic1Type / kMic2Type index
  // cabinetMicName(), position runs along the microphone's track, distance and angle select or interpolate. False with
  // a reason (and the previous pack kept) when nothing usable is found; also sets the reference saved in the state.
  bool loadCabinetPack(const char* folder, std::string* error = nullptr, const char* mappingFile = nullptr);
  void clearCabinetPack();  // the cabinet then passes the signal through
  bool cabinetPackLoaded() const;
  // As saved in the state ("cabp" section). setState restores these, and the host then calls loadCabinetPack with
  // them, so that a missing folder can be reported (cabinetStatus().error).
  const char* cabinetPackReference() const;
  const char* cabinetMappingReference() const;
  int cabinetMicCount() const;                      // microphones found in the pack (at most 16 are reachable)
  std::string cabinetMicName(int index) const;      // "unlabelled" for files without a microphone name
  std::vector<std::string> cabinetSpeakers(int micIndex) const;
  std::vector<std::string> cabinetVariants(int micIndex, const char* speaker) const;
  std::vector<double> cabinetAngles(int micIndex, const char* speaker, const char* variant) const;
  // Which speaker and variant (preamp, take) microphone 0 or 1 uses; "" picks the pack's largest set. Saved.
  void setCabinetMicSource(int mic, const char* speaker, const char* variant);
  std::string cabinetMicSpeaker(int mic) const;
  std::string cabinetMicVariant(int mic) const;
  CabinetStatus cabinetStatus() const;
  // --- cabinet slots' response files (added 2026-11-23) --------------------------------------------------------
  // Each slot (0: Mic 1, 1: Mic 2) plays a microphone of the pack (kMicNSource = 0) or its own response file
  // (kMicNSource = 1), summed in parallel with the slot's on, level, pan and polarity; position, distance, angle and
  // the microphone type do nothing for a file. Both kinds pan at constant power (-3.01 dB each side at the centre); a
  // stereo file keeps its sides (left by the left gain, right by the right). Main thread, while audio runs: read (as loadUserIr), built on the
  // cabinet's worker, crossfaded in 20 ms, zero latency. False with a reason and the slot unchanged otherwise.
  bool loadCabinetIr(int slot, const char* path, std::string* error = nullptr);  // also sets the reference
  bool loadCabinetIrSamples(int slot, const float* const* channels, int numChannels, int frames, double sampleRate,
                            std::string* error = nullptr);
  void clearCabinetIr(int slot);
  // Offline renderers and tests: blocks until the response for the current cabinet controls has been handed to the
  // audio thread (it then fades in with the next process call). False after the timeout.
  bool waitForCabinet(double timeoutSeconds = 5.0);
  // The length of the response the cabinet convolves now (0 without one): what it adds to the chain's tail.
  double cabinetResponseSeconds() const;
  bool cabinetIrLoaded(int slot) const;
  double cabinetIrSeconds(int slot) const;
  int cabinetIrChannels(int slot) const;
  void setCabinetIrNormalize(int slot, bool on);  // unit energy of the louder channel (XP10's switch); default on
  bool cabinetIrNormalize(int slot) const;
  // A file is taken as aligned with the other slot; declared raw (and the other slot raw too), the arrival difference
  // is measured and removed when kMicAlign is on. Default off.
  void setCabinetIrRaw(int slot, bool raw);
  bool cabinetIrRaw(int slot) const;
  // As saved in the state ("cabp" version 2). setState restores these; the host then calls loadCabinetIr with them.
  void setCabinetIrReference(int slot, const char* reference);
  const char* cabinetIrReference(int slot) const;
  // --- cabinet: a pack held in memory (added 2026-11-23) ---------------------------------------------------------
  // As loadCabinetPack, from responses the caller holds (the plug-in's built-in pack): each is a name (read for its
  // position like a file name), a rate and mono samples, which are copied. mappingJson: the mapping file's text, or
  // null. reference is saved in the state as the pack's folder (cabinetPackReference) and shown as it; the host
  // recognises it when the state is restored and calls this again. False with a reason, the previous pack kept.
  struct CabinetPackResponse {
    const char* name;
    double sampleRate;
    const float* samples;
    int frames;
  };
  bool loadCabinetPackMemory(const char* reference, const CabinetPackResponse* responses, int count,
                             const char* mappingJson, std::string* error = nullptr);

  // Parameters kept for compatibility but without meaning in this release (kCabFloor, kUserIrOn, kUserIrLevel): not
  // shown, not automated.
  static bool parameterHidden(int id);

  // --- the pitch transposer at the input (added 2026-11-24) ----------------------------------------------------
  // It sits ahead of the head (before its input trim, which is a gain and commutes with it, and its gate), on the
  // signal the routing sends to the amp (each channel with dual-mono routing). kTranspose != 0 puts it in the signal
  // path in the mode kTransposeMode (Tight about 5.3 ms, Standard about 10.7 ms of delay at any rate); 0 takes it out
  // (no delay, no CPU, the head fed the input itself, bit for bit). Whether it is in, and its mode, change only here:
  // main thread, wait-free (atomic stores), allowed while audio runs. latencySamples() has the new value at once; the
  // audio thread then starts the new path from silence, lets its analysis window fill (about 43 ms; the old path plays
  // meanwhile) and crossfades to it (raised cosine, kTransposeFadeMs), so the change is click-free; taking the
  // transposer out crossfades at once. A further change waits for the one under way to finish.
  // Moving kTranspose between non-zero values needs no call: the transposer crossfades shifts itself, and the
  // latency does not change. Called by prepare() and applyStructuralParams() too.
  static constexpr double kTransposeFadeMs = 10.0;
  void applyTransposeStructure();
  bool transposeInPath() const;         // as last applied
  int transposeLatencySamples() const;  // the transposer's share of latencySamples(): 0 while it is out
  // The audio thread, or after process() returned: true while the path is changing (for tests).
  bool transposeSwitching() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magmora
