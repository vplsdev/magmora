#include "support/Json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace magmora::test {
namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : p_(text.c_str()) {}

  Json value() {
    Json v;
    skip();
    const char c = *p_;
    if (c == '{') {
      v.type = Json::Type::Object;
      ++p_;
      if (!accept('}')) {
        do {
          skip();
          std::string key = string();
          expect(':');
          Json member = value();
          v.members.emplace_back(std::move(key), std::move(member));
        } while (accept(','));
        expect('}');
      }
    } else if (c == '[') {
      v.type = Json::Type::Array;
      ++p_;
      if (!accept(']')) {
        do v.items.push_back(value());
        while (accept(','));
        expect(']');
      }
    } else if (c == '"') {
      v.type = Json::Type::String;
      v.text = string();
    } else if (word("true") || word("false")) {
      v.type = Json::Type::Bool;
      v.number = c == 't' ? 1.0 : 0.0;
    } else if (word("null")) {
      v.type = Json::Type::Null;
    } else {
      char* end = nullptr;
      v.type = Json::Type::Number;
      v.number = std::strtod(p_, &end);
      if (end == p_) throw std::runtime_error("bad JSON");
      p_ = end;
    }
    return v;
  }

 private:
  void skip() {
    while (std::isspace(static_cast<unsigned char>(*p_))) ++p_;
  }
  bool accept(char c) {
    skip();
    if (*p_ != c) return false;
    ++p_;
    return true;
  }
  void expect(char c) {
    if (!accept(c)) throw std::runtime_error(std::string("JSON: expected ") + c);
  }
  bool word(const char* w) {
    const char* q = p_;
    while (*w)
      if (*q++ != *w++) return false;
    p_ = q;
    return true;
  }
  std::string string() {
    expect('"');
    std::string out;
    while (*p_ && *p_ != '"') {
      if (*p_ == '\\' && p_[1]) ++p_;
      out += *p_++;
    }
    expect('"');
    return out;
  }

  const char* p_;
};

}  // namespace

Json Json::parse(const std::string& text) { return Parser(text).value(); }

Json Json::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot read " + path);
  std::stringstream s;
  s << f.rdbuf();
  return parse(s.str());
}

const Json& Json::operator[](const char* key) const {
  for (const auto& m : members)
    if (m.first == key) return m.second;
  throw std::runtime_error(std::string("JSON: no key ") + key);
}

const Json& Json::operator[](size_t k) const { return items.at(k); }

bool Json::has(const char* key) const {
  for (const auto& m : members)
    if (m.first == key) return true;
  return false;
}

std::string Json::scalar() const {
  if (type == Type::String) return text;
  if (type == Type::Bool) return number != 0.0 ? "1" : "0";
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%.17g", number);
  return buffer;
}

}  // namespace magmora::test
