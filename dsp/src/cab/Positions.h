// Position metadata of a response file from its name: a port of
// proto/cab/virtualmic.parse_name, which it matches name for name. Three sources, in order: a per-file entry of the
// pack's mapping file, the mapping file's regular expressions with named groups, and built-in tokens. Main thread.
#pragma once
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace magmora::cab {

constexpr const char* kMappingFileName = "magmora-positions.json";

struct Position {
  std::string mic, speaker, variant;  // empty: not given
  bool hasX = false, hasDist = false, hasAngle = false, hasZone = false, hasStep = false;
  double xMm = 0.0, distMm = 0.0, angleDeg = 0.0, zone = 0.0;
  int step = 0;
  std::string source;  // "mapping", "scheme" or "tokens"
};

struct Mapping {
  struct Scheme {
    std::string pattern;            // as written (Python syntax: (?P<name>...) groups)
    std::regex regex;               // the ECMAScript translation
    std::vector<std::string> names; // capture group k + 1 -> its name ("" for an unnamed group)
    std::map<std::string, std::string> units;
    std::vector<std::string> order;
    bool hasOrder = false;
  };
  std::string phase;  // "minimum", "aligned", "raw", or empty (not given)
  std::vector<Scheme> schemes;
  std::map<std::string, Position> files;  // by file name or stem
};

// The mapping file (JSON). False with a reason for malformed JSON, an unusable regular expression (ECMAScript
// syntax with Python's named groups; look-behind is not available) or an unknown phase.
bool parseMapping(const std::string& text, Mapping& out, std::string* error = nullptr);
Position parseName(const std::string& fileName, const Mapping* mapping = nullptr);

}  // namespace magmora::cab
