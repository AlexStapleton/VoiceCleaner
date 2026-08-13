#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include "config.h"
#include "spsc_ring.h"
#include "frame.h"
#include "framer.h"
#include "dropout.h"
#include "logger.h"
#include "wasapi_capture.h"
#include "wasapi_render.h"
namespace vc {
class Bridge {
public:
  explicit Bridge(const Config& cfg);
  bool start();   // resolves devices, locks+primes rings, starts render/relay/capture/logger
  void stop();
  ~Bridge();
private:
  void   relayLoop();
  void   onCapture(const float* interleaved, size_t frames, int channels);
  size_t onFill(float* outMono, size_t n);
  Config             cfg_;
  SpscRing<Frame>    ringA_;
  SpscRing<Frame>    ringB_;
  DropoutCounter     dropouts_;
  FrameAccumulator   accum_;      // capture side only
  FrameDistributor   dist_;       // render (onFill) side only
  std::vector<float> mono_;       // capture ch0 scratch
  void*              frameReady_ = nullptr;   // auto-reset HANDLE, capture->relay
  std::atomic<bool>  running_{false};
  std::thread        relay_;
  WasapiCapture      capture_;
  WasapiRender       render_;
  Logger             logger_;
  bool               lockedA_ = false;
  bool               lockedB_ = false;
};
}
