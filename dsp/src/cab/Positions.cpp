#include "cab/Positions.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "chain/Json.h"
#include "common/Number.h"

namespace magmora::cab {
namespace {

constexpr double kInch = 25.4;

// virtualmic.MIC_CORES, NOT_MIC_PREFIX and ZONES, in their order. A microphone token is up to three letters followed by
// a model-number core (see virtualmic.mic_token).
struct MicCore {
  const char* core;
  size_t fewest, most;  // prefix letters allowed
};
const MicCore kMicCores[] = {{"57", 0, 3},  {"58", 0, 3},  {"421", 0, 3}, {"121", 0, 3}, {"160", 0, 3},
                             {"184", 0, 3}, {"414", 0, 3}, {"906", 0, 3}, {"609", 0, 3}, {"7b", 0, 3},
                             {"4038", 0, 3}, {"87", 1, 1}, {"88", 1, 1}, {"5", 1, 1}};
const char* const kNotMicPrefix[] = {"oa", "pos", "pre"};
struct Zone {
  const char* word;
  double index;
};
const Zone kZones[] = {{"cap", 0.0},      {"center", 0.0},      {"centre", 0.0},   {"dustcap", 0.0},   {"capedge", 1.0},
                       {"cap-edge", 1.0}, {"cap_edge", 1.0},    {"cap edge", 1.0}, {"edge of cap", 1.0}, {"cone", 2.0},
                       {"mid", 2.0},      {"midcone", 2.0},     {"cone-edge", 3.0}, {"coneedge", 3.0}, {"edge", 3.0},
                       {"surround", 3.0}};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isWord(char c) { return isLetter(c) || isDigit(c) || c == '_' || (static_cast<unsigned char>(c) >= 0x80); }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
std::string lowered(const std::string& s) {
  std::string out(s);
  for (char& c : out) c = lower(c);
  return out;
}
bool startsWithNoCase(const std::string& s, size_t p, const char* word) {
  const size_t n = std::strlen(word);
  if (p + n > s.size()) return false;
  for (size_t k = 0; k < n; ++k)
    if (lower(s[p + k]) != lower(word[k])) return false;
  return true;
}

// Python's float() and int(): the whole text (surrounding white space allowed) must be the number.
bool toDouble(std::string text, double& out) {
  std::replace(text.begin(), text.end(), ',', '.');
  const char* begin = text.c_str();
  char* stop = nullptr;
  out = parseDouble(begin, &stop);
  if (stop == begin) return false;
  while (*stop && isSpace(*stop)) ++stop;
  return *stop == 0;
}
bool toInt(const std::string& text, int& out) {
  const char* begin = text.c_str();
  char* stop = nullptr;
  const long v = std::strtol(begin, &stop, 10);
  if (stop == begin) return false;
  while (*stop && isSpace(*stop)) ++stop;
  out = static_cast<int>(v);
  return *stop == 0;
}

double toMm(const std::string& value, std::string unit, bool& ok) {  // virtualmic._mm
  double v = 0.0;
  ok = toDouble(value, v);
  unit = lowered(unit);
  if (unit == "in" || unit == "inch" || unit == "inches" || unit == "\"" || unit == "''") return v * kInch;
  if (unit == "cm") return v * 10.0;
  return v;
}

// _NUM at p: digits, optionally [.,] and digits. Returns the end, or p if there is no number.
size_t number(const std::string& s, size_t p) {
  size_t q = p;
  while (q < s.size() && isDigit(s[q])) ++q;
  if (q == p) return p;
  if (q + 1 < s.size() && (s[q] == '.' || s[q] == ',') && isDigit(s[q + 1])) {
    q += 1;
    while (q < s.size() && isDigit(s[q])) ++q;
  }
  return q;
}

// _LEN at p: a number, white space, a unit not followed by a letter. Returns the end or 0.
size_t lengthAt(const std::string& s, size_t p, std::string& value, std::string& unit) {
  const size_t e = number(s, p);
  if (e == p) return 0;
  size_t q = e;
  while (q < s.size() && isSpace(s[q])) ++q;
  static const char* const units[] = {"in", "inch", "inches", "\"", "''", "mm", "cm"};
  for (const char* u : units) {
    if (!startsWithNoCase(s, q, u)) continue;
    const size_t end = q + std::strlen(u);
    if (end < s.size() && isLetter(s[end])) continue;
    value = s.substr(p, e - p);
    unit = s.substr(q, end - q);
    return end;
  }
  return 0;
}

// _ANG: the first match, either alternative; returns false if none.
bool angle(const std::string& s, std::string& value) {
  for (size_t p = 0; p < s.size(); ++p) {
    // (?:\bOA\s*|off[\s_-]*axis[\s_-]*)NUM
    const bool boundary = (p == 0 ? isWord(s[0]) : isWord(s[p - 1]) != isWord(s[p]));
    if (boundary && startsWithNoCase(s, p, "oa")) {
      size_t q = p + 2;
      while (q < s.size() && isSpace(s[q])) ++q;
      const size_t e = number(s, q);
      if (e > q) {
        value = s.substr(q, e - q);
        return true;
      }
    }
    if (startsWithNoCase(s, p, "off")) {
      size_t q = p + 3;
      while (q < s.size() && (isSpace(s[q]) || s[q] == '_' || s[q] == '-')) ++q;
      if (startsWithNoCase(s, q, "axis")) {
        q += 4;
        while (q < s.size() && (isSpace(s[q]) || s[q] == '_' || s[q] == '-')) ++q;
        const size_t e = number(s, q);
        if (e > q) {
          value = s.substr(q, e - q);
          return true;
        }
      }
    }
    // NUM\s*(?:deg|°)
    const size_t e = number(s, p);
    if (e > p) {
      size_t q = e;
      while (q < s.size() && isSpace(s[q])) ++q;
      if (startsWithNoCase(s, q, "deg") || s.compare(q, 2, "\xC2\xB0") == 0) {
        value = s.substr(p, e - p);
        return true;
      }
    }
  }
  return false;
}

bool speaker(const std::string& s, std::string& out) {
  static const char* const tags[] = {"LL", "LR", "UL", "UR", "TL", "TR", "BL", "BR"};
  for (size_t p = 0; p + 2 <= s.size(); ++p) {
    if (p > 0 && isLetter(s[p - 1])) continue;
    if (p + 2 < s.size() && isLetter(s[p + 2])) continue;
    for (const char* t : tags)
      if (s[p] == t[0] && s[p + 1] == t[1]) {
        out = t;
        return true;
      }
  }
  return false;
}

bool micWord(const std::string& low, std::string& out) {  // virtualmic.mic_token
  bool found = false;
  size_t bestLen = 0, bestRank = 0, bestPos = 0;
  for (size_t p = 0; p < low.size(); ++p) {
    if (p > 0 && (isDigit(low[p - 1]) || (low[p - 1] >= 'a' && low[p - 1] <= 'z'))) continue;
    size_t k = 0;
    while (p + k < low.size() && low[p + k] >= 'a' && low[p + k] <= 'z') ++k;
    if (k > 3) continue;
    const std::string prefix = low.substr(p, k);
    if (std::find(std::begin(kNotMicPrefix), std::end(kNotMicPrefix), prefix) != std::end(kNotMicPrefix)) continue;
    for (size_t rank = 0; rank < std::size(kMicCores); ++rank) {
      const MicCore& c = kMicCores[rank];
      const size_t e = p + k + std::strlen(c.core);
      if (k < c.fewest || k > c.most || low.compare(p + k, std::strlen(c.core), c.core) != 0) continue;
      if (e < low.size() && isDigit(low[e])) continue;
      // Longest token first, then the earlier core, then the earlier position.
      if (!found || e - p > bestLen || (e - p == bestLen && rank < bestRank)) {
        found = true;
        bestLen = e - p;
        bestRank = rank;
        bestPos = p;
      }
      break;
    }
  }
  if (found) out = low.substr(bestPos, bestLen);
  return found;
}

bool zone(const std::string& low, double& out) {
  std::vector<Zone> zones(std::begin(kZones), std::end(kZones));
  std::stable_sort(zones.begin(), zones.end(),
                   [](const Zone& a, const Zone& b) { return std::strlen(a.word) > std::strlen(b.word); });
  for (const Zone& z : zones)
    if (low.find(z.word) != std::string::npos) {
      out = z.index;
      return true;
    }
  return false;
}

// _INDEX: (?:pos(?:ition)?|bright(?:ness)?|#)[\s_-]*(\d{1,2}), case-insensitive
bool indexToken(const std::string& s, int& out) {
  for (size_t p = 0; p < s.size(); ++p) {
    size_t q = std::string::npos;
    if (startsWithNoCase(s, p, "position")) q = p + 8;
    else if (startsWithNoCase(s, p, "pos")) q = p + 3;
    else if (startsWithNoCase(s, p, "brightness")) q = p + 10;
    else if (startsWithNoCase(s, p, "bright")) q = p + 6;
    else if (s[p] == '#') q = p + 1;
    if (q == std::string::npos) continue;
    while (q < s.size() && (isSpace(s[q]) || s[q] == '_' || s[q] == '-')) ++q;
    if (q < s.size() && isDigit(s[q])) {
      const size_t e = (q + 1 < s.size() && isDigit(s[q + 1])) ? q + 2 : q + 1;
      out = std::atoi(s.substr(q, e - q).c_str());
      return true;
    }
  }
  return false;
}

// _LETTER on " " + stem + " ": [\s_-]([A-J])(?:[\s_.-]|$)
bool letter(const std::string& stem, int& out) {
  const std::string s = " " + stem + " ";
  for (size_t p = 0; p + 1 < s.size(); ++p) {
    if (!(isSpace(s[p]) || s[p] == '_' || s[p] == '-')) continue;
    const char c = s[p + 1];
    if (c < 'A' || c > 'J') continue;
    if (p + 2 == s.size() || isSpace(s[p + 2]) || s[p + 2] == '_' || s[p + 2] == '.' || s[p + 2] == '-') {
      out = c - 'A';
      return true;
    }
  }
  return false;
}

bool fromCap(const std::string& low) {  // from[\s_-]*(cap|cent)
  for (size_t p = low.find("from"); p != std::string::npos; p = low.find("from", p + 1)) {
    size_t q = p + 4;
    while (q < low.size() && (isSpace(low[q]) || low[q] == '_' || low[q] == '-')) ++q;
    if (low.compare(q, 3, "cap") == 0 || low.compare(q, 4, "cent") == 0) return true;
  }
  return false;
}

std::string stemOf(const std::string& name) {  // Path(name).stem, of the last path component
  std::string base = name;
  const size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
}

// Python (?P<name>...) groups to ECMAScript groups, recording the names. False for look-behind or (?P=name).
bool translate(const std::string& in, std::string& out, std::vector<std::string>& names, std::string* error) {
  out.clear();
  names.clear();
  bool inClass = false;
  for (size_t p = 0; p < in.size(); ++p) {
    const char c = in[p];
    if (c == '\\') {
      out += c;
      if (p + 1 < in.size()) out += in[++p];
      continue;
    }
    if (inClass) {
      if (c == ']') inClass = false;
      out += c;
      continue;
    }
    if (c == '[') {
      inClass = true;
      out += c;
      if (p + 1 < in.size() && in[p + 1] == '^') out += in[++p];
      if (p + 1 < in.size() && in[p + 1] == ']') out += in[++p];
      continue;
    }
    if (c == '(') {
      if (in.compare(p, 4, "(?P<") == 0) {
        const size_t close = in.find('>', p + 4);
        if (close == std::string::npos) {
          if (error) *error = "unterminated group name in " + in;
          return false;
        }
        names.push_back(in.substr(p + 4, close - p - 4));
        out += '(';
        p = close;
        continue;
      }
      if (in.compare(p, 4, "(?<=") == 0 || in.compare(p, 4, "(?<!") == 0 || in.compare(p, 4, "(?P=") == 0) {
        if (error) *error = "look-behind and named back-references are not supported: " + in;
        return false;
      }
      if (p + 1 < in.size() && in[p + 1] != '?') names.emplace_back();
    }
    out += c;
  }
  return true;
}

void setNumber(Position& pos, const std::string& key, double v) {
  if (key == "x_mm") pos.hasX = true, pos.xMm = v;
  else if (key == "dist_mm") pos.hasDist = true, pos.distMm = v;
  else if (key == "angle_deg") pos.hasAngle = true, pos.angleDeg = v;
  else if (key == "zone") pos.hasZone = true, pos.zone = v;
  else if (key == "step") pos.hasStep = true, pos.step = static_cast<int>(v);
}

}  // namespace

bool parseMapping(const std::string& text, Mapping& out, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = "mapping file: " + why;
    return false;
  };
  const auto root = chain::parseJson(text.data(), text.size());
  using Kind = chain::JsonValue::Kind;
  if (!root || root->kind != Kind::Object) return fail("not a JSON object");
  Mapping m;
  if (const chain::JsonValue* ph = root->member("phase")) {
    if (ph->kind != Kind::String || (ph->text != "minimum" && ph->text != "aligned" && ph->text != "raw"))
      return fail("\"phase\" must be \"minimum\", \"aligned\" or \"raw\"");
    m.phase = ph->text;
  }
  if (const chain::JsonValue* schemes = root->member("schemes")) {
    if (schemes->kind != Kind::Array) return fail("\"schemes\" must be a list");
    for (const auto& s : schemes->elements) {
      const chain::JsonValue* rx = s->kind == Kind::Object ? s->member("regex") : nullptr;
      if (!rx || rx->kind != Kind::String) return fail("every scheme needs a \"regex\"");
      Mapping::Scheme sch;
      sch.pattern = rx->text;
      std::string ecma;
      if (!translate(rx->text, ecma, sch.names, error)) return false;
      try {
        sch.regex = std::regex(ecma, std::regex::ECMAScript);
      } catch (const std::regex_error& e) {
        return fail(std::string("bad regular expression ") + rx->text + " (" + e.what() + ")");
      }
      if (const chain::JsonValue* units = s->member("units"); units && units->kind == Kind::Object)
        for (const auto& [k, v] : units->members)
          if (v->kind == Kind::String) sch.units[k] = v->text;
      if (const chain::JsonValue* order = s->member("order"); order && order->kind == Kind::Array) {
        sch.hasOrder = true;
        for (const auto& e : order->elements) sch.order.push_back(e->kind == Kind::String ? e->text : std::string());
      }
      m.schemes.push_back(std::move(sch));
    }
  }
  if (const chain::JsonValue* files = root->member("files")) {
    if (files->kind != Kind::Object) return fail("\"files\" must be an object");
    for (const auto& [name, entry] : files->members) {
      if (entry->kind != Kind::Object) return fail("\"files\" entries must be objects");
      Position p;
      p.source = "mapping";
      for (const auto& [k, v] : entry->members) {
        if (v->kind == Kind::String) {
          if (k == "mic") p.mic = v->text;
          else if (k == "speaker") p.speaker = v->text;
          else if (k == "variant") p.variant = v->text;
        } else if (v->kind == Kind::Number) {
          setNumber(p, k, v->number);
        }
      }
      m.files[name] = p;
    }
  }
  out = std::move(m);
  return true;
}

Position parseName(const std::string& fileName, const Mapping* mapping) {
  std::string name = fileName;
  if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
  const std::string stem = stemOf(name);
  if (mapping) {
    auto it = mapping->files.find(name);
    if (it == mapping->files.end()) it = mapping->files.find(stem);
    if (it != mapping->files.end()) return it->second;
    for (const Mapping::Scheme& sch : mapping->schemes) {
      std::smatch g;
      if (!std::regex_search(stem, g, sch.regex)) continue;
      Position out;
      out.source = "scheme";
      for (size_t k = 0; k < sch.names.size() && k + 1 < g.size(); ++k) {
        const std::string& key = sch.names[k];
        if (key.empty() || !g[k + 1].matched) continue;
        const std::string v = g[k + 1].str();
        if (key == "x" || key == "dist") {
          const auto u = sch.units.find(key);
          bool ok = false;
          const double mm = toMm(v, u == sch.units.end() ? "in" : u->second, ok);
          if (!ok) continue;
          if (key == "x") out.hasX = true, out.xMm = mm;
          else out.hasDist = true, out.distMm = mm;
        } else if (key == "angle") {
          double a = 0.0;
          if (toDouble(v, a)) out.hasAngle = true, out.angleDeg = a;
        } else if (key == "step") {
          int step = 0;
          bool ok = false;
          if (sch.hasOrder) {
            const auto at = std::find(sch.order.begin(), sch.order.end(), v);
            ok = at != sch.order.end();
            step = static_cast<int>(at - sch.order.begin());
          } else if (v.size() == 1 && isLetter(v[0])) {
            ok = true;
            step = std::toupper(static_cast<unsigned char>(v[0])) - 'A';
          } else {
            ok = toInt(v, step);
          }
          if (ok) out.hasStep = true, out.step = step;
        } else if (key == "mic") {
          out.mic = v;
        } else if (key == "speaker") {
          out.speaker = v;
        } else if (key == "variant") {
          out.variant = v;
        }
      }
      return out;
    }
  }
  Position out;
  out.source = "tokens";
  const std::string low = lowered(stem);
  std::vector<double> lens;
  for (size_t p = 0; p < stem.size();) {
    std::string value, unit;
    const size_t end = lengthAt(stem, p, value, unit);
    if (end > 0) {
      bool ok = false;
      lens.push_back(toMm(value, unit, ok));
      p = end;
    } else {
      ++p;
    }
  }
  if (lens.size() >= 2) {
    out.hasX = out.hasDist = true;
    out.xMm = lens[0];
    out.distMm = lens[1];
  } else if (lens.size() == 1) {
    if (fromCap(low)) out.hasX = true, out.xMm = lens[0];
    else out.hasDist = true, out.distMm = lens[0];
  }
  if (std::string a; angle(stem, a)) {
    double v = 0.0;
    if (toDouble(a, v)) out.hasAngle = true, out.angleDeg = v;
  }
  if (std::string s; speaker(stem, s)) out.speaker = s;
  if (std::string w; micWord(low, w)) out.mic = w;
  if (double z = 0.0; zone(low, z)) out.hasZone = true, out.zone = z;
  int index = 0, l = 0;
  if (indexToken(stem, index)) out.hasStep = true, out.step = index;
  else if (letter(stem, l) && !out.hasX) out.hasStep = true, out.step = l;
  return out;
}

}  // namespace magmora::cab
