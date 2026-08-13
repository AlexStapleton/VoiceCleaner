#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace vc {
struct WavData {
  uint32_t sampleRate = 48000;
  uint16_t channels   = 1;
  std::vector<float> samples;   // interleaved, float [-1,1]
};
bool wav_read(const std::string& path, WavData& out);              // PCM16 or float32; false on error
bool wav_write(const std::string& path, const WavData& in, bool asFloat = false);  // PCM16 or float32
}
