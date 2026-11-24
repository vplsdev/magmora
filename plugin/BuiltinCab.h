// The built-in cabinet: a pack of responses computed by the round-8 cabinet model
// (plugin/generated/BuiltinCab.inc, written by proto/cab/make_builtin_pack.py; manifest BuiltinCab.json), compiled in
// and loaded through the virtual microphone like a user's pack. A new instance plays it with the cabinet on.
//
// Provisional. To take it out: set MAGMORA_BUILTIN_CAB to 0 (or delete this file, the two generated files and the
// lines in Magmora.cpp that name builtin_cab), and give presets/01-bedrock.json and the full-chain presets back
// "cab_on": 0. Sessions saved with it then report its pack as missing and keep the reference.
#pragma once

#ifndef MAGMORA_BUILTIN_CAB
#define MAGMORA_BUILTIN_CAB 1
#endif

#include <cstring>
#include <string>
#include <vector>

#include <magmora/Chain.h>

#if MAGMORA_BUILTIN_CAB
#include "generated/BuiltinCab.inc"
#endif

namespace builtin_cab {

constexpr bool kEnabled = MAGMORA_BUILTIN_CAB != 0;

#if !MAGMORA_BUILTIN_CAB
constexpr const char* kName = "";
constexpr const char* kReference = "";
#endif

// The parameter defaults of a new instance: the cabinet on (slot 1, a pack microphone, is on by the chain's defaults).
inline double Default(int id, double chainDefault) { return kEnabled && id == magmora::kCabOn ? 1.0 : chainDefault; }

inline bool IsReference(const std::string& folder) { return kEnabled && folder == kReference; }

// Main thread. The pack becomes the chain's pack; its reference is saved in the session as the pack folder.
inline bool Load(magmora::Chain& chain, std::string* error) {
#if MAGMORA_BUILTIN_CAB
  static_assert(sizeof(float) == sizeof(unsigned int), "float32");
  std::vector<float> samples(static_cast<size_t>(kFiles) * kFrames);  // the float32 bit patterns, as floats
  std::memcpy(samples.data(), kSamples, sizeof kSamples);
  std::vector<magmora::Chain::CabinetPackResponse> responses(kFiles);
  for (size_t k = 0; k < responses.size(); ++k) responses[k] = {kNames[k], kRate, samples.data() + k * kFrames, kFrames};
  return chain.loadCabinetPackMemory(kReference, responses.data(), kFiles, kMapping, error);
#else
  (void)chain;
  if (error) *error = "this build has no built-in cabinet";
  return false;
#endif
}

}  // namespace builtin_cab
