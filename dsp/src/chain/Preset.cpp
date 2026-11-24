#include <magmora/Preset.h>

#include <magmora/Chain.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "chain/Json.h"
#include "common/Number.h"

namespace magmora {
namespace {

using chain::JsonValue;
using Kind = JsonValue::Kind;

// The preset objects and the parameters each carries (as plugin/scripts/generate.py and Chain::applyPreset).
struct Block {
  const char* key;
  int ranges[2][2];  // first, last; {-1, -1} unused
};
constexpr Block kBlocks[] = {
    {"params", {{0, kNumParams - 1}, {kAmpOn, kAmpOn}}},
    {"pedal", {{kPedalOn, kPedalLevel}, {-1, -1}}},
    {"eq", {{kEqOn, kEqBand4Type}, {-1, -1}}},
    {"cab", {{kCabOn, kMicOffset}, {kMic1Source, kMic2Source}}},
    {"fx", {{kFxOn, kFxOn}, {-1, -1}}},
};
constexpr const char* kFormat = "magmora-preset";
constexpr size_t kMaxPathBytes = 4096;
constexpr size_t kMaxShortText = 256;

template <class F>
void forEachId(const Block& b, F&& f) {
  for (const auto& r : b.ranges)
    for (int id = r[0]; r[0] >= 0 && id <= r[1]; ++id)
      if (isPresetParameter(id)) f(id);
}

const ParamInfo& info(int id) { return *Chain::parameterInfo(id); }

bool plainText(const std::string& s, size_t limit) {
  if (s.size() > limit) return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

void escape(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20 || c == 0x7f) {
      static const char* const hex = "0123456789abcdef";
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
}

bool fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

bool readString(const JsonValue* v, std::string& out, size_t limit) {
  if (!v) return true;  // absent: left as it is
  if (v->kind != Kind::String || !plainText(v->text, limit)) return false;
  out = v->text;
  return true;
}

// A file or pack reference: null (none) or an object.
bool readRef(const JsonValue* v, PresetFileRef& ref, bool pack, PresetCabFiles& cab) {
  ref = PresetFileRef{};
  if (!v || v->kind == Kind::Null) return true;
  if (v->kind != Kind::Object) return false;
  ref.present = true;
  if (!readString(v->member("path"), ref.path, kMaxPathBytes) || !readString(v->member("name"), ref.name, kMaxShortText) ||
      !readString(v->member("mapping"), ref.mapping, kMaxPathBytes))
    return false;
  if (ref.path.empty() && ref.name.empty()) return false;
  if (const JsonValue* b = v->member("bytes")) {
    if (b->kind != Kind::Number || !std::isfinite(b->number) || b->number < 0.0 || b->number > 9.0e15 ||
        b->number != std::floor(b->number))
      return false;
    ref.bytes = static_cast<int64_t>(b->number);
  }
  if (const JsonValue* n = v->member("normalize")) {
    if (n->kind != Kind::Bool) return false;
    ref.normalize = n->number != 0.0;
  }
  if (!pack) return true;
  const char* lists[] = {"mics", "speakers", "variants"};
  std::string* into[] = {cab.mic, cab.speaker, cab.variant};
  for (int k = 0; k < 3; ++k) {
    const JsonValue* a = v->member(lists[k]);
    if (!a) continue;
    if (a->kind != Kind::Array || a->elements.size() > 2) return false;
    for (size_t m = 0; m < a->elements.size(); ++m)
      if (!readString(a->elements[m].get(), into[k][m], kMaxShortText)) return false;
  }
  return true;
}

void writeRef(std::string& out, const char* key, const PresetFileRef& ref, const PresetCabFiles* pack, bool last) {
  out += "    ";
  escape(out, key);
  out += ": ";
  if (!ref.present) {
    out += last ? "null\n" : "null,\n";
    return;
  }
  out += "{\"path\": ";
  escape(out, ref.path);
  out += ", \"name\": ";
  escape(out, ref.name);
  if (pack) {
    out += ", \"mapping\": ";
    escape(out, ref.mapping);
    const std::string* lists[] = {pack->mic, pack->speaker, pack->variant};
    const char* names[] = {"mics", "speakers", "variants"};
    for (int k = 0; k < 3; ++k) {
      out += ", ";
      escape(out, names[k]);
      out += ": [";
      escape(out, lists[k][0]);
      out += ", ";
      escape(out, lists[k][1]);
      out += "]";
    }
  } else {
    if (ref.bytes >= 0) out += ", \"bytes\": " + std::to_string(ref.bytes);
    out += ref.normalize ? ", \"normalize\": true" : ", \"normalize\": false";
  }
  out += last ? "}\n" : "},\n";
}

// Whole UTF-8 characters only: cut before a continuation byte.
void cutUtf8(std::string& s, size_t limit) {
  if (s.size() <= limit) return;
  size_t at = limit;
  while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
  s.resize(at);
}

}  // namespace

UserPreset::UserPreset() {
  for (int id = 0; id < kNumChainParams; ++id) {
    values[id] = info(id).def;
    given[id] = false;
  }
}

bool isPresetParameter(int id) {
  return id >= 0 && id < kNumChainParams && !Chain::isSessionParameter(id) && !Chain::parameterHidden(id);
}

void setPresetSnapshot(UserPreset& preset, const double* values) {
  for (int id = 0; id < kNumChainParams; ++id) {
    preset.given[id] = isPresetParameter(id);
    preset.values[id] = preset.given[id] ? values[id] : info(id).def;
  }
}

bool readUserPreset(const char* text, size_t size, UserPreset& out, std::string* error) {
  if (!text || size == 0) return fail(error, "empty file");
  if (size > kMaxPresetFileBytes) return fail(error, "too large for a preset");
  const auto root = chain::parseJson(text, size);
  if (!root || root->kind != Kind::Object) return fail(error, "not a Magmora preset");
  const JsonValue* format = root->member("format");
  const JsonValue* params = root->member("params");
  if (format) {
    if (format->kind != Kind::String || format->text != kFormat) return fail(error, "not a Magmora preset");
  } else if (!params) {
    return fail(error, "not a Magmora preset");  // neither a user preset nor a factory-style one
  }
  if (const JsonValue* v = root->member("version")) {
    if (v->kind != Kind::Number || !(v->number >= 1.0) || v->number != std::floor(v->number))
      return fail(error, "not a Magmora preset: bad version");
  }

  UserPreset p;
  const JsonValue* name = root->member("name");
  if (!name || !readString(name, p.name, 4 * kMaxPresetNameBytes)) return fail(error, "not a Magmora preset: no name");
  p.name = cleanPresetName(p.name);
  if (p.name.empty()) return fail(error, "not a Magmora preset: no name");
  if (!readString(root->member("category"), p.category, kMaxShortText)) return fail(error, "not a Magmora preset: bad category");

  for (const Block& b : kBlocks) {
    const JsonValue* o = root->member(b.key);
    if (o && o->kind != Kind::Object) return fail(error, std::string("not a Magmora preset: \"") + b.key + "\" is not an object");
    // The head's preset parameters are always set (absent: defaults), as the factory presets; a block only with its object.
    if (!o && std::strcmp(b.key, "params") != 0) continue;
    bool ok = true;
    forEachId(b, [&](int id) {
      p.given[id] = true;
      const JsonValue* v = o ? o->member(info(id).key) : nullptr;
      if (!v) return;
      if ((v->kind != Kind::Number && v->kind != Kind::Bool) || !std::isfinite(v->number)) {
        ok = false;
        return;
      }
      const ParamInfo& pi = info(id);
      double x = std::min(std::max(v->number, pi.min), pi.max);
      if (pi.taper == Taper::Stepped) x = std::round(x);
      p.values[id] = x;
    });
    if (!ok) return fail(error, std::string("not a Magmora preset: a value in \"") + b.key + "\" is not a number");
  }

  if (const JsonValue* files = root->member("cab_files")) {
    if (files->kind != Kind::Object) return fail(error, "not a Magmora preset: bad file references");
    p.hasCabFiles = true;
    if (!readRef(files->member("pack"), p.cab.pack, true, p.cab) || !readRef(files->member("mic1"), p.cab.ir[0], false, p.cab) ||
        !readRef(files->member("mic2"), p.cab.ir[1], false, p.cab))
      return fail(error, "not a Magmora preset: bad file references");
  }
  out = std::move(p);
  return true;
}

std::string writeUserPreset(const UserPreset& preset) {
  std::string out = "{\n  \"format\": \"magmora-preset\",\n  \"version\": " + std::to_string(kUserPresetVersion) + ",\n  \"name\": ";
  escape(out, cleanPresetName(preset.name));
  out += ",\n  \"category\": ";
  escape(out, preset.category);
  for (const Block& b : kBlocks) {
    std::string body;
    forEachId(b, [&](int id) {
      if (!preset.given[id]) return;
      body += body.empty() ? "\n    " : ",\n    ";
      escape(body, info(id).key);
      body += ": " + formatDouble(preset.values[id]);
    });
    if (body.empty() && std::strcmp(b.key, "params") != 0) continue;
    out += ",\n  ";
    escape(out, b.key);
    out += body.empty() ? ": {}" : ": {" + body + "\n  }";
  }
  if (preset.hasCabFiles) {
    out += ",\n  \"cab_files\": {\n";
    writeRef(out, "pack", preset.cab.pack, &preset.cab, false);
    writeRef(out, "mic1", preset.cab.ir[0], nullptr, false);
    writeRef(out, "mic2", preset.cab.ir[1], nullptr, true);
    out += "  }";
  }
  out += "\n}\n";
  return out;
}

std::string cleanPresetName(const std::string& name) {
  std::string s;
  for (unsigned char c : name)
    if (c >= 0x20 && c != 0x7f) s += static_cast<char>(c);
  const size_t first = s.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  s = s.substr(first, s.find_last_not_of(' ') - first + 1);
  cutUtf8(s, kMaxPresetNameBytes);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

std::string presetFileStem(const std::string& name) {
  std::string s;
  for (unsigned char c : cleanPresetName(name)) {
    // What Windows or macOS refuse or treat specially in a file name, and path separators.
    if (std::strchr("/\\:*?\"<>|", c) || c < 0x20) s += '-';
    else s += static_cast<char>(c);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(0, 1);
  if (s.empty()) s = "Preset";
  // Device names Windows reserves, whatever the extension.
  std::string upper;
  for (char c : s.substr(0, s.find('.'))) upper += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
  static const char* const kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                          "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                          "LPT7", "LPT8", "LPT9"};
  for (const char* r : kReserved)
    if (upper == r) s += '_';
  return s;
}

}  // namespace magmora
