#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include "config.h"
#include "spsc_ring.h"
#include "frame.h"
#include "framer.h"
#include "resampler.h"
#include "drift_control.h"
#include "dropout.h"
#include "frame_time_stats.h"
#include "logger.h"
#include "wasapi_capture.h"
#include "wasapi_render.h"
#include "inoise_filter.h"
#include "passthrough_filter.h"
#include "rnnoise_filter.h"
#include "dsp_chain.h"
#include "param_store.h"
#include "telemetry_shm.h"
namespace vc {
class Bridge {
public:
  explicit Bridge(const Config& cfg);
  bool start();   // resolves devices, locks+primes rings, starts render/relay/capture/logger
  void stop();
  ~Bridge();
  // Live device re-selection (non-RT): stop() -> swap device id -> start(). controller_ is a
  // member (not rebuilt by start()/stop()); reselectCapture seeds it after stop() joins the old
  // relay but before start() spawns a new one, so the warm-start is race-free.
  bool reselectCapture(const std::string& deviceId, double driftSeedPpm);
  bool reselectRender(const std::string& deviceId);
  bool device_lost() const noexcept { return deviceLost_.load(std::memory_order_acquire); }
  // Non-RT: zero the dropout + frame-time counters (and ring high-water) for a clean measurement
  // window. dropouts_ is atomic (reset here); relay-owned stats are cleared on the relay's next hop.
  void reset_counters() noexcept;
  vc::ipc::ParamStore& params() noexcept { return params_; }
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
  std::unique_ptr<INoiseFilter> engine_;   // constructed in start() (non-RT): RnnoiseFilter or PassthroughFilter
  std::unique_ptr<DspChain>     chain_;     // optional DC->HPF->AGC->limiter wrap
  Frame                         proc_;      // processed-frame scratch (relay RT)
  std::atomic<double> ratioPpm_{0.0};
  std::atomic<double> filtOcc_{0.0};
  FrameTimeStats     frameStats_;   // per-hop engine+chain compute times (written by relay thread only)
  FrameTimeStats     hopStats_;     // per-hop WHOLE relay hop times (pop->push; relay thread only)
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
  bool               lockedAccum_ = false;    // capture-thread reframing FIFO, VirtualLock'd like the rings
  bool               lockedReframe_ = false;  // relay-thread reframing FIFO
  bool               lockedDist_ = false;     // render-thread reframing FIFO
  vc::ipc::ParamStore      params_;
  vc::ipc::TelemetryWriter telemetry_;
  bool                     telemetryOn_ = false;
  uint32_t                 telemetrySeq_ = 0;
  int                      hopsSincePublish_ = 0;
  float                    winPeak_ = 0.0f;
  double                   winSumSq_ = 0.0;
  std::atomic<bool>        statsResetRequested_{false};  // main tick -> relay clears frameStats_/ringBHigh_ next hop
  uint32_t                 ringBHigh_ = 0;               // relay-owned running max of ringB occupancy (telemetry)
  std::chrono::steady_clock::time_point start_{};        // set in start(); telemetry uptimeMs
  float                    estLatencyMs_ = 0.0f;         // app-controlled latency, computed in start() (telemetry)
};
}
