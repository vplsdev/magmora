// The amplifier netlist as data. The tables are generated from proto/blockamp (design/generate_netlist.py);
// this file only knows how to instantiate them, never what is in them.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace magmora::circuit {

enum class Kind { Resistor, Capacitor, Inductor, VoltageSource, CurrentSource, Triode, Pentode, Diode, Transformer };

constexpr int kMaxNodes = 8;
constexpr int kMaxKnobs = 16;
constexpr int kGround = -1;

struct SwitchDef {
  const char* key;
  const char* off;
  const char* on;
  bool defaultOn;
};

struct KnobDef {
  const char* key;
  double def;
};

// A knob-dependent resistor: max(offset + slope * taper(position / 10), rmin).
struct LawDef {
  int knob;
  double mid;  // 0: linear, else the audio taper through (0.5, mid)
  double offset, slope, rmin;
};

struct VariantDef {
  uint64_t combinations;  // bit k: this variant is present for switch combination k
  const char* nodes[kMaxNodes];
  int numNodes;
  double value;
  int law;
};

struct ElementDef {
  Kind kind;
  const char* name;
  int section;
  int firstVariant, numVariants;
  bool isInput;
  double params[12];
};

// One set of component values and device parameters (value_sets.json).
struct ValueSetDef {
  const char* name;
  const char* modelHash;    // as scripts/export_model_data.py would compute it for this set
  const char* netlistHash;
  const LawDef* laws;
  const VariantDef* variants;
  const ElementDef* elements;
  int numElements;
};

struct ClusterDef {
  const char* name;
  unsigned sections;
  const char* cutIn;
  const char* cutOut;
  const char* rails[4];
  int numRails;
  bool extrapolated;  // whether others may extrapolate what this cluster presents
};

namespace netdata {
#include "circuit/NetlistData.inc"
}

constexpr int kNumKnobs = static_cast<int>(sizeof(netdata::kKnobs) / sizeof(KnobDef));
constexpr int kNumValueSets = static_cast<int>(sizeof(netdata::kValueSets) / sizeof(ValueSetDef));
constexpr int kDefaultValueSet = 0;  // the prototype
constexpr int kNumSwitches = static_cast<int>(sizeof(netdata::kSwitches) / sizeof(SwitchDef));
constexpr unsigned kStructuralSwitches = netdata::kSwitch_load;
static_assert(kNumKnobs <= kMaxKnobs, "raise kMaxKnobs");
static_assert(kNumSwitches <= 6, "variant masks are 64 bits wide");

struct Controls {
  unsigned switches;
  std::array<double, kMaxKnobs> knobs;  // front-panel positions, 0..10

  Controls();  // the prototype's defaults
  // key/value as the harness writes them ("channel=lead", "crunch=1", "pre_lead=7"); false if unknown.
  bool set(const char* key, const char* value);
};

int findKnob(const char* key);
int findValueSet(const char* name);  // -1 if there is none

struct Variant {
  uint64_t combinations;
  std::array<int, kMaxNodes> nodes;
  int numNodes;
  double value;
  const LawDef* law;
};

struct Item {
  Kind kind;
  std::string name;
  bool input;
  std::array<double, 12> params;
  std::vector<Variant> variants;

  // The variant in force for a switch combination, or null if the element is absent.
  const Variant* variant(unsigned switches) const {
    for (const Variant& v : variants)
      if ((v.combinations >> switches) & 1u) return &v;
    return nullptr;
  }
};

// v(plus) - v(minus), or the branch current of a voltage source; with both, branch current + gain (v(plus) - v(minus)).
struct Output {
  std::string label;
  int plus = kGround, minus = kGround;
  int branch = -1;  // item index of a voltage source whose branch current is (part of) the output
  double gain = 0.0;
};

class Circuit {
 public:
  // The listed sections for one value of the structural switches; every relay variant is kept.
  static Circuit fromSections(unsigned sections, unsigned structural, int valueSet);

  int addSource(Kind, const std::string& name, const char* plus, const char* minus, double value, bool input);
  void addOutput(const std::string& label, const char* plus, const char* minus = "0");
  void addBranchOutput(const std::string& label, int sourceItem);
  // branch current of sourceItem + gain * v(node)
  void addCompanionOutput(const std::string& label, int sourceItem, const char* node, double gain);
  void addResistor(const std::string& name, const char* a, const char* b, double ohms);

  int findNode(const std::string& name) const;  // -2 if there is none
  int findOutput(const std::string& label) const;
  int findInput(const std::string& name) const;  // index among the inputs
  const Item* findItem(const std::string& name) const;

  std::vector<std::string> nodes;
  std::vector<Item> items;
  std::vector<Output> outputs;
  std::vector<int> inputs;  // item indices, in order
  uint64_t combinations = 0;  // the switch combinations this structure allows

 private:
  int node(const char* name);
};

// Resistance, capacitance, inductance or source value of a variant.
double valueOf(const Variant&, const Controls&);
double taper(double x, double mid);

}  // namespace magmora::circuit
