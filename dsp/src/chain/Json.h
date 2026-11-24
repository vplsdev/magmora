// A small JSON reader for presets and the cabinet's position mapping file: objects, arrays, strings, numbers,
// booleans and null. Main thread only; allocates.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace magmora::chain {

struct JsonValue {
  enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
  double number = 0.0;
  std::string text;
  std::map<std::string, std::unique_ptr<JsonValue>> members;  // Object
  std::vector<std::unique_ptr<JsonValue>> elements;            // Array
  const JsonValue* member(const char* key) const {
    const auto it = members.find(key);
    return it == members.end() ? nullptr : it->second.get();
  }
};

// Null on malformed input or trailing garbage.
std::unique_ptr<JsonValue> parseJson(const char* text, size_t size);

}  // namespace magmora::chain
