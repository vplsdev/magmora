// Where the golden data lives. MAGMORA_GOLDEN_DIR overrides it (for checking against a freshly exported set).
#pragma once
#include <cstdlib>
#include <string>
#include <vector>

#include "circuit/Netlist.h"
#include "support/Json.h"

namespace magmora::test {

inline std::string goldenDir() {
  const char* dir = std::getenv("MAGMORA_GOLDEN_DIR");
  return dir ? dir : MAGMORA_SOURCE_DIR "/reference/golden";
}

inline std::vector<std::string> goldenCases() {
  std::vector<std::string> out;
  for (const char* setting : {"lead_default", "crunch_bright", "lead_extreme"})
    for (const char* rate : {"96000", "192000"}) out.push_back(goldenDir() + "/" + setting + "_" + rate + ".json");
  return out;
}

inline circuit::Controls controlsFrom(const Json& setting) {
  circuit::Controls c;
  for (const auto& [key, value] : setting.members)
    if (!c.set(key.c_str(), value.scalar().c_str())) std::abort();
  return c;
}

}  // namespace magmora::test
