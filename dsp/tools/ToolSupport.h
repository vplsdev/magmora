// Shared by the developer tools: arguments, files, timing.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace magmora::tools {

[[noreturn]] inline void fail(const std::string& message) {
  std::fprintf(stderr, "error: %s\n", message.c_str());
  std::exit(2);
}

class Args {
 public:
  Args(int argc, char** argv) : items_(argv + 1, argv + argc) {}

  bool empty() const { return next_ >= items_.size(); }
  bool atOption() const { return empty() || items_[next_].compare(0, 2, "--") == 0; }
  std::string take(const char* what) {
    if (empty()) fail(std::string("missing ") + what);
    return items_[next_++];
  }
  double number(const char* what) {
    const std::string s = take(what);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end) fail(std::string("not a number for ") + what + ": " + s);
    return v;
  }
  // key=value pairs, one or more.
  void settings(std::vector<std::pair<std::string, std::string>>& out) {
    do {
      const std::string s = take("key=value");
      const size_t eq = s.find('=');
      if (eq == std::string::npos) fail("expected key=value, got " + s);
      out.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    } while (!atOption());
  }

 private:
  std::vector<std::string> items_;
  size_t next_ = 0;
};

inline std::vector<std::string> split(const std::string& list, char separator = ',') {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(separator, start);
    if (end == std::string::npos) end = list.size();
    if (end > start) out.push_back(list.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

inline std::vector<double> readF64(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) fail("cannot read " + path);
  std::vector<double> out;
  double chunk[4096];
  size_t n;
  while ((n = std::fread(chunk, sizeof(double), 4096, f)) > 0) out.insert(out.end(), chunk, chunk + n);
  std::fclose(f);
  return out;
}

template <class T>
void writeRaw(const std::string& path, const std::vector<T>& data) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) fail("cannot write " + path);
  const bool ok = data.empty() || std::fwrite(data.data(), sizeof(T), data.size(), f) == data.size();
  if (std::fclose(f) != 0 || !ok) fail("cannot write " + path);
}

}  // namespace magmora::tools
