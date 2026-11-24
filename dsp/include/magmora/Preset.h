// Magmora DSP - user preset files (added 2026-11-24). A user preset is the factory presets' JSON (presets/*.json:
// "params" for the head, then one object per block) with a format tag, a version and the cabinet's file references:
//
//   {"format": "magmora-preset", "version": 1, "name": "Wall Of Sound", "category": "User",
//    "params": {...}, "pedal": {...}, "eq": {...}, "cab": {...}, "fx": {...},
//    "cab_files": {"pack": {"path": ..., "name": ..., "mapping": ..., "mics": [..], "speakers": [..], "variants": [..]},
//                  "mic1": null, "mic2": {"path": ..., "name": ..., "bytes": 52844, "normalize": true}}}
//
// Session settings (input trim, load, quality, phase mode, routing) and the hidden compatibility parameters are never
// written and never read. Reading validates everything and changes nothing on failure; unknown keys are ignored and
// missing ones take their defaults; writing does not depend on the process locale. Main thread; allocates.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "ChainParams.h"

namespace magmora {

struct PresetFileRef {
  bool present = false;    // false: none (null in the file)
  std::string path;        // as saved: an absolute path, or the built-in cabinet's reference
  std::string name;        // the file's or folder's own name, to find it next to the preset or elsewhere
  std::string mapping;     // a pack's mapping file, or empty
  int64_t bytes = -1;      // a file's size when saved; -1 unknown
  bool normalize = true;   // a file's normalisation
};

struct PresetCabFiles {
  PresetFileRef pack;
  PresetFileRef ir[2];                         // the slots' response files
  std::string mic[2], speaker[2], variant[2];  // the slots' pack microphones by name (indexes differ between packs)
};

struct UserPreset {
  UserPreset();  // every value at its default, nothing given, no file references
  std::string name;
  std::string category = "User";
  double values[kNumChainParams];
  bool given[kNumChainParams];  // what the file sets; a saved user preset sets every preset parameter
  bool hasCabFiles = false;     // false: a file without "cab_files" (a factory-style file) leaves the references alone
  PresetCabFiles cab;
};

inline constexpr int kUserPresetVersion = 1;
inline constexpr size_t kMaxPresetFileBytes = 256 * 1024;
inline constexpr size_t kMaxPresetNameBytes = 63;

// Carried by presets: neither a session setting nor a hidden compatibility parameter.
bool isPresetParameter(int id);
// A whole snapshot of `values` (indexed by chain parameter id): every preset parameter given.
void setPresetSnapshot(UserPreset& preset, const double* values);
// False, with a reason and `out` untouched, for anything that is not a well-formed preset.
bool readUserPreset(const char* text, size_t size, UserPreset& out, std::string* error = nullptr);
// The given values, the name and category, and the file references when hasCabFiles. Pretty-printed JSON.
std::string writeUserPreset(const UserPreset& preset);
// A display name made usable: control characters dropped, spaces trimmed, at most kMaxPresetNameBytes (whole UTF-8
// characters). Empty when nothing is left.
std::string cleanPresetName(const std::string& name);
// A file name for a preset (without ".json"): portable on macOS and Windows, never empty, never "." or "..".
std::string presetFileStem(const std::string& name);

}  // namespace magmora
