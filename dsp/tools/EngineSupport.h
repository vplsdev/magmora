// What magmora-render and magmora-bench share.
#pragma once
#include <magmora/Engine.h>

#include "ToolSupport.h"
#include "circuit/Netlist.h"
#include "engine/DeveloperOptions.h"

namespace magmora::tools {

using Settings = std::vector<std::pair<std::string, std::string>>;

inline void setParameter(Engine& engine, const std::string& key, const std::string& value) {
  static const struct {
    const char* key;
    const char* word;
    double value;
  } words[] = {{"input_jack", "high", 0}, {"input_jack", "normal", 1}, {"channel", "rhythm", 0},
               {"channel", "lead", 1},    {"load", "reactive", 0},     {"load", "resistive", 1},
               {"quality", "standard", 0}, {"quality", "high", 1},     {"quality", "ultra", 2},
               {"phase_mode", "linear", 0}, {"phase_mode", "minimum", 1}, {"channel_mode", "mono_l", 0},
               {"channel_mode", "mono_sum", 1}, {"channel_mode", "dual", 2}};
  const std::string k = key == "input" ? "input_jack" : key;
  for (const ParamInfo& p : kParams) {
    if (k != p.key) continue;
    for (const auto& w : words)
      if (k == w.key && value == w.word) {
        engine.setParam(p.id, w.value);
        return;
      }
    if (p.taper == Taper::Stepped && (value == "true" || value == "True" || value == "false" || value == "False")) {
      engine.setParam(p.id, (value == "true" || value == "True") ? 1.0 : 0.0);
      return;
    }
    char* end = nullptr;
    const double v = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end) fail("bad value " + key + "=" + value);
    if (v < p.min || v > p.max) fail("out of range: " + key + "=" + value);
    engine.setParam(p.id, v);
    return;
  }
  fail("unknown parameter " + key);
}

inline int valueSetIndex(const std::string& name) {
  const int index = Engine::findValueSet(name.c_str());
  if (index < 0) fail("no value set called " + name);
  return index;
}

inline void printVersion(const char* tool) {
  std::printf("%s %s model %s\n", tool, versionString(), modelHash());
}

inline std::string valueSetDescription(int index) {
  const circuit::ValueSetDef& v = circuit::netdata::kValueSets[index];
  return std::string(v.name) + " (model " + v.modelHash + ", netlist " + v.netlistHash + ")";
}

}  // namespace magmora::tools
