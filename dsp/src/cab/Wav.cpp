#include "cab/Wav.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>

namespace magmora::cab {
namespace {

uint32_t u32(const uint8_t* p) {
  return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool fail(std::string* error, const char* why) {
  if (error) *error = why;
  return false;
}

}  // namespace

bool readWav(const uint8_t* data, size_t size, Audio& out, std::string* error) {
  if (!data || size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
    return fail(error, "not a RIFF WAVE file");
  uint16_t format = 0, channels = 0, bits = 0, align = 0;
  uint32_t rate = 0;
  const uint8_t* samples = nullptr;
  size_t bytes = 0;
  bool haveFormat = false;
  size_t at = 12;
  while (at + 8 <= size) {
    const uint8_t* chunk = data + at;
    const size_t length = u32(chunk + 4);
    const size_t body = at + 8, available = size - body;
    if (std::memcmp(chunk, "fmt ", 4) == 0) {
      if (length < 16 || length > available) return fail(error, "bad format chunk");
      format = u16(data + body);
      channels = u16(data + body + 2);
      rate = u32(data + body + 4);
      align = u16(data + body + 12);
      bits = u16(data + body + 14);
      if (format == 0xFFFE) {  // extensible: the sub-format's first two bytes are the format code
        if (length < 40) return fail(error, "bad extensible format chunk");
        format = u16(data + body + 24);
      }
      haveFormat = true;
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      samples = data + body;
      bytes = length <= available ? length : available;  // a truncated last chunk: read what is there
    }
    if (length > available) break;
    at = body + length + (length & 1);  // chunks are padded to even sizes
  }
  if (!haveFormat) return fail(error, "no format chunk");
  if (!samples) return fail(error, "no data chunk");
  if (channels < 1) return fail(error, "no channels");
  if (rate < 8000 || rate > 768000) return fail(error, "sample rate out of range");
  const bool pcm = format == 1, flt = format == 3;
  if (!(pcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(flt && (bits == 32 || bits == 64)))
    return fail(error, "unsupported sample format (PCM 8/16/24/32 or float 32/64)");
  const size_t width = bits / 8;
  if (align != channels * width) return fail(error, "inconsistent block alignment");
  const size_t frames = bytes / align;
  if (frames < 1) return fail(error, "no samples");
  if (frames > kMaxWavFrames) return fail(error, "file too long");

  const int kept = channels >= 2 ? 2 : 1;
  out.rate = rate;
  out.channels.assign(static_cast<size_t>(kept), std::vector<float>(frames));
  for (size_t n = 0; n < frames; ++n)
    for (int c = 0; c < kept; ++c) {
      const uint8_t* p = samples + n * align + static_cast<size_t>(c) * width;
      double v = 0.0;
      if (flt && bits == 32) {
        float f;
        const uint32_t w = u32(p);
        std::memcpy(&f, &w, 4);
        v = f;
      } else if (flt) {
        double d;
        const uint64_t w = u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32);
        std::memcpy(&d, &w, 8);
        v = d;
      } else if (bits == 8) {
        v = (static_cast<int>(p[0]) - 128) / 128.0;
      } else if (bits == 16) {
        v = static_cast<int16_t>(u16(p)) / 32768.0;
      } else if (bits == 24) {
        const int32_t s = static_cast<int32_t>((uint32_t{p[0]} << 8) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 24)) >> 8;
        v = s / 8388608.0;
      } else {
        v = static_cast<int32_t>(u32(p)) / 2147483648.0;
      }
      // Checked before narrowing: a 64-bit sample beyond float range would become infinite (and converting it is
      // undefined behaviour). Such a sample, and one that is not finite, is read as silence.
      out.channels[static_cast<size_t>(c)][n] = std::isfinite(v) && std::fabs(v) <= FLT_MAX ? static_cast<float>(v) : 0.0f;
    }
  return true;
}

bool readWavFile(const char* path, Audio& out, std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return fail(error, "cannot open the file");
  const std::streamoff size = f.tellg();
  if (size <= 0 || size > static_cast<std::streamoff>(kMaxWavFrames) * 16 + (1 << 20)) return fail(error, "file size out of range");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return fail(error, "cannot read the file");
  return readWav(bytes.data(), bytes.size(), out, error);
}

}  // namespace magmora::cab
