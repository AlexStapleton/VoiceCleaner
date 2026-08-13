#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace vc {

namespace {

constexpr uint16_t kWaveFormatPCM = 1;
constexpr uint16_t kWaveFormatIEEEFloat = 3;
constexpr uint16_t kWaveFormatExtensible = 0xFFFE;

template <typename T>
bool readPod(std::ifstream& f, T& value) {
  f.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(f);
}

template <typename T>
void writePod(std::ofstream& f, const T& value) {
  f.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

}  // namespace

bool wav_read(const std::string& path, WavData& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;

  char riffId[4];
  if (!f.read(riffId, 4) || std::memcmp(riffId, "RIFF", 4) != 0) return false;

  uint32_t riffSize = 0;
  if (!readPod(f, riffSize)) return false;

  char waveId[4];
  if (!f.read(waveId, 4) || std::memcmp(waveId, "WAVE", 4) != 0) return false;

  bool haveFmt = false;
  uint16_t audioFormat = 0;
  uint16_t numChannels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;

  std::vector<uint8_t> dataBytes;
  bool haveData = false;

  while (f) {
    char chunkId[4];
    if (!f.read(chunkId, 4)) break;  // no more chunks, EOF is fine
    uint32_t chunkSize = 0;
    if (!readPod(f, chunkSize)) return false;

    if (std::memcmp(chunkId, "fmt ", 4) == 0) {
      if (chunkSize < 16) return false;
      uint16_t blockAlign = 0;
      uint32_t byteRate = 0;
      if (!readPod(f, audioFormat)) return false;
      if (!readPod(f, numChannels)) return false;
      if (!readPod(f, sampleRate)) return false;
      if (!readPod(f, byteRate)) return false;
      if (!readPod(f, blockAlign)) return false;
      if (!readPod(f, bitsPerSample)) return false;
      std::streamsize remaining = static_cast<std::streamsize>(chunkSize) - 16;
      if (remaining > 0) {
        // Might be extensible fmt; we don't need the extra fields beyond
        // resolving audioFormat, so just skip them.
        f.seekg(remaining, std::ios::cur);
      }
      if (!f) return false;
      haveFmt = true;
      // Chunks are word-aligned: skip a pad byte if chunkSize is odd.
      if (chunkSize % 2 != 0) f.seekg(1, std::ios::cur);
    } else if (std::memcmp(chunkId, "data", 4) == 0) {
      dataBytes.resize(chunkSize);
      if (chunkSize > 0) {
        if (!f.read(reinterpret_cast<char*>(dataBytes.data()), chunkSize)) return false;
      }
      haveData = true;
      if (chunkSize % 2 != 0) f.seekg(1, std::ios::cur);
    } else {
      f.seekg(chunkSize, std::ios::cur);
      if (chunkSize % 2 != 0) f.seekg(1, std::ios::cur);
    }
  }

  if (!haveFmt || !haveData) return false;
  if (numChannels == 0 || sampleRate == 0) return false;

  uint16_t effectiveFormat = audioFormat;
  if (effectiveFormat == kWaveFormatExtensible) {
    if (bitsPerSample == 16) effectiveFormat = kWaveFormatPCM;
    else if (bitsPerSample == 32) effectiveFormat = kWaveFormatIEEEFloat;
    else return false;
  }

  out.sampleRate = sampleRate;
  out.channels = numChannels;
  out.samples.clear();

  if (effectiveFormat == kWaveFormatPCM && bitsPerSample == 16) {
    size_t count = dataBytes.size() / sizeof(int16_t);
    out.samples.resize(count);
    const int16_t* src = reinterpret_cast<const int16_t*>(dataBytes.data());
    for (size_t i = 0; i < count; ++i) {
      out.samples[i] = static_cast<float>(src[i]) / 32768.0f;
    }
  } else if (effectiveFormat == kWaveFormatIEEEFloat && bitsPerSample == 32) {
    size_t count = dataBytes.size() / sizeof(float);
    out.samples.resize(count);
    std::memcpy(out.samples.data(), dataBytes.data(), count * sizeof(float));
  } else {
    return false;
  }

  return true;
}

bool wav_write(const std::string& path, const WavData& in, bool asFloat) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;

  const uint16_t channels = in.channels;
  const uint32_t sampleRate = in.sampleRate;
  const uint16_t bitsPerSample = asFloat ? 32 : 16;
  const uint16_t audioFormat = asFloat ? kWaveFormatIEEEFloat : kWaveFormatPCM;
  const uint16_t blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));
  const uint32_t byteRate = sampleRate * blockAlign;
  const uint32_t dataSize = static_cast<uint32_t>(in.samples.size() * (bitsPerSample / 8));
  const uint32_t fmtChunkSize = 16;
  const uint32_t riffSize = 4 + (8 + fmtChunkSize) + (8 + dataSize);

  f.write("RIFF", 4);
  writePod(f, riffSize);
  f.write("WAVE", 4);

  f.write("fmt ", 4);
  writePod(f, fmtChunkSize);
  writePod(f, audioFormat);
  writePod(f, channels);
  writePod(f, sampleRate);
  writePod(f, byteRate);
  writePod(f, blockAlign);
  writePod(f, bitsPerSample);

  f.write("data", 4);
  writePod(f, dataSize);

  if (asFloat) {
    f.write(reinterpret_cast<const char*>(in.samples.data()),
            static_cast<std::streamsize>(in.samples.size() * sizeof(float)));
  } else {
    for (float s : in.samples) {
      float clamped = std::clamp(s, -1.0f, 1.0f);
      int32_t v = static_cast<int32_t>(std::lround(clamped * 32767.0f));
      v = std::clamp(v, -32768, 32767);
      int16_t sample16 = static_cast<int16_t>(v);
      writePod(f, sample16);
    }
  }

  return static_cast<bool>(f);
}

}  // namespace vc
