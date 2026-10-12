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
// Event-driven shared-mode WASAPI render. Each period asks onFill for up to N mono float samples,
// upmixes to the endpoint's channel count, and writes them. If onFill returns short, the remainder
// is zero-filled (a render-side underrun; the caller's onFill is responsible for counting it).
// Pre-rolls one buffer of silence at start. Assumes COM already initialized.
class WasapiRender {
public:
  // fill up to n mono samples into outMono; return count written (< n => underrun, zero-filled here)
  using FillCb  = std::function<size_t(float* outMono, size_t n)>;
  using ErrorCb = std::function<void(const char* what)>;
  bool start(IMMDevice* dev, FillCb onFill, ErrorCb onError);  // false on setup failure
  void stop();
  ~WasapiRender();
  uint32_t bufferFrames() const { return bufferFrames_; }   // shared render buffer (pre-rolled at start); latency input
private:
  void run();
  IAudioClient*       client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  void*               event_  = nullptr;   // HANDLE
  std::thread         thread_;
  std::atomic<bool>   running_{false};
  FillCb              onFill_;
  ErrorCb             onError_;
  int                 channels_ = 0;
  SampleKind          kind_ = SampleKind::Unsupported;
  UINT32              bufferFrames_ = 0;
  std::vector<float>  mono_;                // per-period mono scratch
};
}
