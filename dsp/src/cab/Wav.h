// A WAV reader of our own (XP10): RIFF WAVE, PCM 8 / 16 / 24 / 32-bit, IEEE float 32 / 64, plain or
// WAVE_FORMAT_EXTENSIBLE, any rate, any number of channels (the first two are kept). Main thread; allocates.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace magmora::cab {

struct Audio {
  double rate = 0.0;
  std::vector<std::vector<float>> channels;  // one or two
  size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
};

constexpr size_t kMaxWavFrames = 30 * 192000;  // a response file longer than 30 s at 192 kHz is refused

// False with a reason in `error` for anything that is not a readable WAV.
bool readWav(const uint8_t* data, size_t size, Audio& out, std::string* error = nullptr);
bool readWavFile(const char* path, Audio& out, std::string* error = nullptr);

}  // namespace magmora::cab
