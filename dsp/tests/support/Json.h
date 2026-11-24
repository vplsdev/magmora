// Just enough JSON to read reference/golden and reference/*.json in the tests.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace magmora::test {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  double number = 0.0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;

  static Json load(const std::string& path);  // throws std::runtime_error
  static Json parse(const std::string& text);

  const Json& operator[](const char* key) const;
  const Json& operator[](size_t k) const;
  const Json& operator[](int k) const { return (*this)[static_cast<size_t>(k)]; }
  bool has(const char* key) const;
  size_t size() const { return items.size(); }
  std::string scalar() const;  // a number, bool or string, as the settings spell it
};

}  // namespace magmora::test
