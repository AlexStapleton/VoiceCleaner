#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <vector>
#include <cstddef>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "device_enumerator.h"
namespace vc {
// Event-driven shared-mode WASAPI capture. Delivers interleaved float32 samples (converted from
// PCM16 if needed) to onSamples on a dedicated MMCSS "Pro Audio" thread. RT-thread: the callback
// must not block. Surfaces device-invalidated etc. via onError. Assumes COM already initialized.
class WasapiCapture {
public:
  using SampleCb = std::function<void(const float* interleaved, size_t frames, int channels)>;
  using ErrorCb  = std::function<void(const char* what)>;
  bool start(IMMDevice* dev, SampleCb onSamples, ErrorCb onError);  // false on setup failure
  void stop();
  ~WasapiCapture();
  uint32_t bufferFrames() const { return bufferFrames_; }   // shared capture buffer; latency input
private:
  void run();
  IAudioClient*        client_  = nullptr;
  IAudioCaptureClient* capture_ = nullptr;
  void*                event_   = nullptr;   // HANDLE
  std::thread          thread_;
  std::atomic<bool>    running_{false};
  SampleCb             onSamples_;
  ErrorCb              onError_;
  int                  channels_ = 0;
  UINT32               bufferFrames_ = 0;    // shared capture buffer size (GetBufferSize); latency input
  SampleKind           kind_ = SampleKind::Unsupported;
  std::vector<float>   scratch_;             // pcm16->float / silence buffer
};
}
