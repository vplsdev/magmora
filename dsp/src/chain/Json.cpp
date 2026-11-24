#include "chain/Json.h"

#include <cctype>
#include <cstdlib>

#include "common/Number.h"

namespace magmora::chain {
namespace {

constexpr int kMaxDepth = 32;

struct Parser {
  const char* p;
  const char* end;

  void space() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool literal(const char* word) {
    const char* q = p;
    for (; *word; ++word, ++q)
      if (q >= end || *q != *word) return false;
    p = q;
    return true;
  }
  bool string(std::string& out) {
    if (p >= end || *p != '"') return false;
    ++p;
    out.clear();
    while (p < end && *p != '"') {
      if (static_cast<unsigned char>(*p) < 0x20) return false;
      if (*p == '\\') {
        if (++p >= end) return false;
        switch (*p) {
          case '"': case '\\': case '/': out += *p; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u':
            if (end - p < 5) return false;
            for (int k = 1; k <= 4; ++k)
              if (!std::isxdigit(static_cast<unsigned char>(p[k]))) return false;
            {
              const unsigned code = static_cast<unsigned>(std::strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16));
              p += 4;
              if (code < 0x80) {
                out += static_cast<char>(code);
              } else if (code < 0x800) {  // as UTF-8, so that file names in a mapping file match
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
              } else {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
              }
            }
            break;
          default: return false;
        }
        ++p;
      } else {
        out += *p++;
      }
    }
    if (p >= end) return false;
    ++p;
    return true;
  }
  bool number(double& out) {
    const char* start = p;
    if (p < end && *p == '-') ++p;
    if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) return false;
    while (p < end && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' ||
                       *p == '-'))
      ++p;
    const std::string text(start, p);
    char* stop = nullptr;
    out = parseDouble(text.c_str(), &stop);
    return stop == text.c_str() + text.size();
  }
  bool value(JsonValue& v, int depth) {
    if (depth > kMaxDepth) return false;
    space();
    if (p >= end) return false;
    if (*p == '{') {
      v.kind = JsonValue::Kind::Object;
      ++p;
      space();
      if (p < end && *p == '}') {
        ++p;
        return true;
      }
      for (;;) {
        space();
        std::string key;
        if (!string(key)) return false;
        space();
        if (p >= end || *p++ != ':') return false;
        auto child = std::make_unique<JsonValue>();
        if (!value(*child, depth + 1)) return false;
        v.members[key] = std::move(child);
        space();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == '}') {
          ++p;
          return true;
        }
        return false;
      }
    }
    if (*p == '[') {
      v.kind = JsonValue::Kind::Array;
      ++p;
      space();
      if (p < end && *p == ']') {
        ++p;
        return true;
      }
      for (;;) {
        auto element = std::make_unique<JsonValue>();
        if (!value(*element, depth + 1)) return false;
        v.elements.push_back(std::move(element));
        space();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == ']') {
          ++p;
          return true;
        }
        return false;
      }
    }
    if (*p == '"') {
      v.kind = JsonValue::Kind::String;
      return string(v.text);
    }
    if (literal("true")) {
      v.kind = JsonValue::Kind::Bool;
      v.number = 1.0;
      return true;
    }
    if (literal("false")) {
      v.kind = JsonValue::Kind::Bool;
      v.number = 0.0;
      return true;
    }
    if (literal("null")) return true;
    v.kind = JsonValue::Kind::Number;
    return number(v.number);
  }
};

}  // namespace

std::unique_ptr<JsonValue> parseJson(const char* text, size_t size) {
  if (!text) return nullptr;
  Parser parser{text, text + size};
  auto root = std::make_unique<JsonValue>();
  if (!parser.value(*root, 0)) return nullptr;
  parser.space();
  return parser.p == parser.end ? std::move(root) : nullptr;
}

}  // namespace magmora::chain
