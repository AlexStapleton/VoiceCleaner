#pragma once
#include <atomic>
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
#include "dfn3_stream_adapter.h"
#include "switchable_engine.h"
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
  bool device_lost() const noexcept { return deviceLost_.load(std::memory_order_acquire); }
  // Non-RT switch trigger (warm-standby only). idx 0 = rnnoise, 1 = dfn3. No-op otherwise.
  void request_engine(int idx) noexcept { if (switch_) switch_->requestActive(idx); }
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
  std::unique_ptr<INoiseFilter> leafA_, leafB_;  // warm-standby leaves (owned; borrowed by switch_)
  std::unique_ptr<INoiseFilter> engine_;   // constructed in start() (non-RT)
  SwitchableEngine*             switch_ = nullptr;  // non-owning; points into engine_ when warm-standby
  Dfn3StreamAdapter*            dfn3Adapter_ = nullptr;  // non-owning; set whenever a DFN3 adapter is live (observability)
  std::unique_ptr<DspChain>     chain_;     // optional DC->HPF->AGC->limiter wrap
  Frame                         proc_;      // processed-frame scratch (relay RT)
  std::atomic<double> ratioPpm_{0.0};
  std::atomic<double> filtOcc_{0.0};
  FrameTimeStats     frameStats_;   // per-hop engine+chain compute times (written by relay thread only)
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
  vc::ipc::ParamStore      params_;
  vc::ipc::TelemetryWriter telemetry_;
  bool                     telemetryOn_ = false;
  uint32_t                 telemetrySeq_ = 0;
  int                      hopsSincePublish_ = 0;
};
}
