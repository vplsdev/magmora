// Minimal RIFF/WAVE for the developer tools: PCM 16/24/32 and float 32/64 in, float 32 out.
#pragma once
#include <cstdint>

#include "ToolSupport.h"

namespace magmora::tools {

struct Audio {
  double rate = 0.0;
  std::vector<std::vector<float>> channels;
  size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
};

inline uint32_t readLE(const unsigned char* p, int bytes) {
  uint32_t v = 0;
  for (int k = 0; k < bytes; ++k) v |= static_cast<uint32_t>(p[k]) << (8 * k);
  return v;
}

inline Audio readWav(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) fail("cannot read " + path);
  std::vector<unsigned char> b;
  unsigned char chunk[1 << 14];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) b.insert(b.end(), chunk, chunk + n);
  std::fclose(f);
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
    fail(path + ": not a WAVE file");

  uint32_t format = 0, bits = 0, count = 0;
  Audio audio;
  for (size_t at = 12; at + 8 <= b.size();) {
    const unsigned char* h = b.data() + at;
    const size_t body = at + 8;
    const size_t declared = readLE(h + 4, 4);
    const size_t available = declared < b.size() - body ? declared : b.size() - body;
    if (std::memcmp(h, "fmt ", 4) == 0 && available >= 16) {
      format = readLE(h + 8, 2);
      count = readLE(h + 10, 2);
      audio.rate = readLE(h + 12, 4);
      bits = readLE(h + 22, 2);
      if (format == 0xfffe && available >= 40) format = readLE(h + 8 + 24, 2);
      if (count < 1 || count > 64) fail(path + ": channel count");
    } else if (std::memcmp(h, "data", 4) == 0) {
      const bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32);
      const bool real = format == 3 && (bits == 32 || bits == 64);
      if (!pcm && !real) fail(path + ": unsupported sample format");
      const size_t width = bits / 8, frames = available / (width * count);
      audio.channels.assign(count, std::vector<float>(frames));
      for (size_t t = 0; t < frames; ++t)
        for (uint32_t c = 0; c < count; ++c) {
          const unsigned char* p = b.data() + body + width * (t * count + c);
          float v;
          if (real && bits == 32) {
            std::memcpy(&v, p, 4);
          } else if (real) {
            double d;
            std::memcpy(&d, p, 8);
            v = static_cast<float>(d);
          } else {
            const uint32_t shifted = readLE(p, static_cast<int>(width)) << (32 - bits);
            v = static_cast<float>(static_cast<int32_t>(shifted) / 2147483648.0);
          }
          audio.channels[c][t] = v;
        }
      return audio;
    }
    at = body + declared + (declared & 1);
  }
  fail(path + ": no data chunk");
}

inline void writeWav(const std::string& path, const Audio& audio) {
  const uint32_t count = static_cast<uint32_t>(audio.channels.size());
  const uint32_t frames = static_cast<uint32_t>(audio.frames());
  const uint32_t rate = static_cast<uint32_t>(audio.rate + 0.5), data = frames * count * 4;
  std::vector<unsigned char> b;
  const auto put = [&b](uint32_t v, int bytes) {
    for (int k = 0; k < bytes; ++k) b.push_back(static_cast<unsigned char>(v >> (8 * k)));
  };
  const auto tag = [&b](const char* s) { b.insert(b.end(), s, s + 4); };
  tag("RIFF");
  put(50 + data, 4);
  tag("WAVE");
  tag("fmt ");
  put(18, 4);
  put(3, 2);
  put(count, 2);
  put(rate, 4);
  put(rate * count * 4, 4);
  put(count * 4, 2);
  put(32, 2);
  put(0, 2);
  tag("fact");
  put(4, 4);
  put(frames, 4);
  tag("data");
  put(data, 4);
  for (uint32_t t = 0; t < frames; ++t)
    for (uint32_t c = 0; c < count; ++c) {
      uint32_t bits;
      std::memcpy(&bits, &audio.channels[c][t], 4);
      put(bits, 4);
    }
  writeRaw(path, b);
}

}  // namespace magmora::tools
