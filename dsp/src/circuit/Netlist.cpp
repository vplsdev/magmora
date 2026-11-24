#include "circuit/Netlist.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/Number.h"

namespace magmora::circuit {
namespace {

bool equals(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// Switch combinations whose structural bits equal `structural`.
uint64_t combinationsWith(unsigned structural) {
  uint64_t mask = 0;
  for (unsigned c = 0; c < (1u << kNumSwitches); ++c)
    if ((c & kStructuralSwitches) == structural) mask |= uint64_t{1} << c;
  return mask;
}

}  // namespace

Controls::Controls() : switches(0), knobs{} {
  for (int s = 0; s < kNumSwitches; ++s)
    if (netdata::kSwitches[s].defaultOn) switches |= 1u << s;
  for (int k = 0; k < kNumKnobs; ++k) knobs[static_cast<size_t>(k)] = netdata::kKnobs[k].def;
}

int findKnob(const char* key) {
  for (int k = 0; k < kNumKnobs; ++k)
    if (equals(key, netdata::kKnobs[k].key)) return k;
  return -1;
}

bool Controls::set(const char* key, const char* value) {
  if (equals(key, "input_jack")) key = "input";
  for (int s = 0; s < kNumSwitches; ++s) {
    const SwitchDef& d = netdata::kSwitches[s];
    if (!equals(key, d.key)) continue;
    const bool on = equals(value, d.on) || equals(value, "1") || equals(value, "true") || equals(value, "True");
    const bool off = equals(value, d.off) || equals(value, "0") || equals(value, "false") || equals(value, "False");
    if (!on && !off) return false;
    switches = on ? (switches | 1u << s) : (switches & ~(1u << s));
    return true;
  }
  const int k = findKnob(key);
  if (k < 0) return false;
  char* end = nullptr;
  const double v = parseDouble(value, &end);
  if (end == value || *end != '\0' || !std::isfinite(v)) return false;
  knobs[static_cast<size_t>(k)] = v;
  return true;
}

int findValueSet(const char* name) {
  for (int k = 0; k < kNumValueSets; ++k)
    if (equals(name, netdata::kValueSets[k].name)) return k;
  return -1;
}

int Circuit::node(const char* name) {
  if (equals(name, "0")) return kGround;
  const auto it = std::find(nodes.begin(), nodes.end(), name);
  if (it != nodes.end()) return static_cast<int>(it - nodes.begin());
  nodes.emplace_back(name);
  return static_cast<int>(nodes.size()) - 1;
}

Circuit Circuit::fromSections(unsigned sections, unsigned structural, int valueSet) {
  const ValueSetDef& values = netdata::kValueSets[valueSet];
  Circuit c;
  const uint64_t kept = combinationsWith(structural);
  c.combinations = kept;
  for (int index = 0; index < values.numElements; ++index) {
    const ElementDef& e = values.elements[index];
    if (!((sections >> e.section) & 1u)) continue;
    Item item{e.kind, e.name, e.isInput, {}, {}};
    std::copy(std::begin(e.params), std::end(e.params), item.params.begin());
    for (int v = e.firstVariant; v < e.firstVariant + e.numVariants; ++v) {
      const VariantDef& d = values.variants[v];
      if (!(d.combinations & kept)) continue;
      Variant variant{d.combinations & kept, {}, d.numNodes, d.value, d.law < 0 ? nullptr : values.laws + d.law};
      for (int n = 0; n < d.numNodes; ++n) variant.nodes[static_cast<size_t>(n)] = c.node(d.nodes[n]);
      item.variants.push_back(variant);
    }
    if (item.variants.empty()) continue;
    if (item.input) c.inputs.push_back(static_cast<int>(c.items.size()));
    c.items.push_back(std::move(item));
  }
  return c;
}

int Circuit::addSource(Kind kind, const std::string& name, const char* plus, const char* minus, double value, bool input) {
  Item item{kind, name, input, {}, {}};
  Variant v{combinations, {}, 2, value, nullptr};
  v.nodes[0] = node(plus);
  v.nodes[1] = node(minus);
  item.variants.push_back(v);
  const int index = static_cast<int>(items.size());
  if (input) inputs.push_back(index);
  items.push_back(std::move(item));
  return index;
}

void Circuit::addOutput(const std::string& label, const char* plus, const char* minus) {
  outputs.push_back({label, node(plus), node(minus), -1, 0.0});
}

void Circuit::addBranchOutput(const std::string& label, int sourceItem) {
  outputs.push_back({label, kGround, kGround, sourceItem, 0.0});
}

void Circuit::addCompanionOutput(const std::string& label, int sourceItem, const char* plus, double gain) {
  outputs.push_back({label, node(plus), kGround, sourceItem, gain});
}

void Circuit::addResistor(const std::string& name, const char* a, const char* b, double ohms) {
  Item item{Kind::Resistor, name, false, {}, {}};
  Variant v{combinations, {}, 2, ohms, nullptr};
  v.nodes[0] = node(a);
  v.nodes[1] = node(b);
  item.variants.push_back(v);
  items.push_back(std::move(item));
}

int Circuit::findNode(const std::string& name) const {
  const auto it = std::find(nodes.begin(), nodes.end(), name);
  return it == nodes.end() ? -2 : static_cast<int>(it - nodes.begin());
}

int Circuit::findOutput(const std::string& label) const {
  for (size_t k = 0; k < outputs.size(); ++k)
    if (outputs[k].label == label) return static_cast<int>(k);
  return -1;
}

int Circuit::findInput(const std::string& name) const {
  for (size_t k = 0; k < inputs.size(); ++k)
    if (items[static_cast<size_t>(inputs[k])].name == name) return static_cast<int>(k);
  return -1;
}

const Item* Circuit::findItem(const std::string& name) const {
  for (const Item& item : items)
    if (item.name == name) return &item;
  return nullptr;
}

double taper(double x, double mid) {
  x = std::min(std::max(x, 0.0), 1.0);
  if (mid == 0.0) return x;
  const double root = 1.0 / mid - 1.0;
  const double b = root * root;
  return (std::pow(b, x) - 1.0) / (b - 1.0);
}

double valueOf(const Variant& v, const Controls& controls) {
  if (!v.law) return v.value;
  const LawDef& law = *v.law;
  const double f = taper(controls.knobs[static_cast<size_t>(law.knob)] / 10.0, law.mid);
  return std::max(law.offset + law.slope * f, law.rmin);
}

}  // namespace magmora::circuit
