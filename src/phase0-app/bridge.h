#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include "config.h"
#include "spsc_ring.h"
#include "frame.h"
#include "framer.h"
#include "resampler.h"
#include "drift_control.h"
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
  bool device_lost() const noexcept { return deviceLost_.load(std::memory_order_acquire); }
private:
  void   relayLoop();
  void   onCapture(const float* interleaved, size_t frames, int channels);
  size_t onFill(float* outMono, size_t n);
  Config             cfg_;
  SpscRing<Frame>    ringA_;
  SpscRing<Frame>    ringB_;
  DropoutCounter     dropouts_;
  DriftController    controller_;
  Resampler          resampler_;
  FrameAccumulator   reframe_;    // relay side: re-block resampler output into 480-sample frames
  std::vector<float> rsOut_;      // relay side: resampler output scratch
  std::atomic<double> ratioPpm_{0.0};
  std::atomic<double> filtOcc_{0.0};
  FrameAccumulator   accum_;      // capture side only
  FrameDistributor   dist_;       // render (onFill) side only
  std::vector<float> mono_;       // capture ch0 scratch
  void*              frameReady_ = nullptr;   // auto-reset HANDLE, capture->relay
  std::atomic<bool>  running_{false};
  std::atomic<bool>  deviceLost_{false};
  std::thread        relay_;
  WasapiCapture      capture_;
  WasapiRender       render_;
  Logger             logger_;
  bool               lockedA_ = false;
  bool               lockedB_ = false;
};
}
