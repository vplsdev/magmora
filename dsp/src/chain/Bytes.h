// Bounded byte reading and writing for the chain state, in the machine's byte order (as the head state).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace magmora::chain {

constexpr int32_t kMaxString = 128;     // the head chunk's own limits (plug-in state)
constexpr int32_t kMaxEntries = 4096;
constexpr int32_t kMaxBody = 1 << 20;
constexpr int32_t kMaxSections = 64;

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
  template <class T>
  void put(const T& value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out_.insert(out_.end(), p, p + sizeof(T));
  }
  void bytes(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    out_.insert(out_.end(), p, p + n);
  }
  void string(const char* text) {
    const int32_t length = static_cast<int32_t>(std::strlen(text));
    put(length);
    bytes(text, static_cast<size_t>(length));
  }
  size_t size() const { return out_.size(); }

 private:
  std::vector<uint8_t>& out_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), pos_(0), end_(size) {}
  template <class T>
  bool get(T& value) {
    return bytes(&value, sizeof(T));
  }
  bool bytes(void* to, size_t n) {
    if (n > end_ - pos_) return false;
    if (n) std::memcpy(to, data_ + pos_, n);
    pos_ += n;
    return true;
  }
  bool string(std::string& text) {
    int32_t length = 0;
    if (!get(length) || length < 0 || length > kMaxString) return false;
    text.assign(static_cast<size_t>(length), '\0');
    return !length || bytes(&text[0], static_cast<size_t>(length));
  }
  bool skip(size_t n) {
    if (n > end_ - pos_) return false;
    pos_ += n;
    return true;
  }
  // Narrows the readable range to the next n bytes (a section or a chunk body).
  bool limit(size_t n) {
    if (n > end_ - pos_) return false;
    end_ = pos_ + n;
    return true;
  }
  const uint8_t* here() const { return data_ + pos_; }
  size_t left() const { return end_ - pos_; }

 private:
  const uint8_t* data_;
  size_t pos_, end_;
};

}  // namespace magmora::chain
