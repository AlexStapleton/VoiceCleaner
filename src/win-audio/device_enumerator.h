#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <mmdeviceapi.h>
#include <audioclient.h>
namespace vc {
enum class SampleKind { Float32, Pcm16, Unsupported };
struct DeviceInfo { std::wstring id; std::wstring name; uint32_t sampleRate; uint16_t channels; SampleKind kind; };
struct FormatCheck { bool ok; SampleKind kind; uint16_t channels; std::string message; };

class DeviceEnumerator {
public:
  std::vector<DeviceInfo> list(EDataFlow flow);              // eRender or eCapture
  IMMDevice* resolve(const std::wstring& id);                // caller Release()s; nullptr if not found
  static FormatCheck check_format(const WAVEFORMATEX* mix);  // requires 48000 Hz + float32/pcm16
};
}
