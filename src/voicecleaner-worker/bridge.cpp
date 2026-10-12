#include "bridge.h"
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include "channel.h"
#include "device_enumerator.h"
#include "mmcss_thread.h"
#include "rt_memory.h"
#include "latency.h"
namespace vc {

static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); } // ASCII device IDs

Bridge::Bridge(const Config& cfg)
  : cfg_(cfg),
    ringA_(cfg.ringFrames),
    ringB_(cfg.ringFrames),
    controller_({ (double)(cfg.ringFrames / 2), cfg.kp, cfg.ki, cfg.driftMaxPpm, cfg.filterAlpha }),
    logger_(cfg.csvPath, &ringA_, &ringB_, &dropouts_, &ratioPpm_, &filtOcc_) {
  mono_.reserve(48000);   // 1 s ch0 scratch, avoids steady-state realloc
  rsOut_.resize(2 * FRAME_SAMPLES);   // resize (not just reserve) so [] is valid; ratio<=+150ppm => <=481 out
}

bool Bridge::start() {
  DeviceEnumerator e;
  IMMDevice* capDev = e.resolve(widen(cfg_.captureDeviceId));
  IMMDevice* renDev = e.resolve(widen(cfg_.renderDeviceId));
  if (!capDev || !renDev) {
    std::fprintf(stderr, "error: could not resolve capture/render device (check IDs via --list-devices)\n");
    if (capDev) capDev->Release();
    if (renDev) renDev->Release();
    return false;
  }

  lockedA_ = rt_lock_and_touch(ringA_.data(), ringA_.byte_size());
  lockedB_ = rt_lock_and_touch(ringB_.data(), ringB_.byte_size());
  // The reframing FIFOs are alloc-free (fixed capacity) but still live on the RT threads; lock+touch
  // them so those threads never take a soft page fault on the buffers either (best-effort, like rings).
  lockedAccum_   = rt_lock_and_touch(accum_.data(),   accum_.byte_size());
  lockedReframe_ = rt_lock_and_touch(reframe_.data(), reframe_.byte_size());
  lockedDist_    = rt_lock_and_touch(dist_.data(),    dist_.byte_size());

  // Start from empty buffers. On a device reselect, stop() joined every thread but left the rings and
  // FIFOs holding the previous session's audio (ring B sits at ~capacity/2, the drift setpoint).
  // Priming on top of that would start ring B nearly full: ~+capacity/2 hops of latency that the
  // drift controller can only drain at driftMaxPpm (minutes), plus overruns and stale mic audio.
  // No thread is running here, so the non-thread-safe clear()s are safe.
  ringA_.clear();
  ringB_.clear();
  accum_.clear();
  reframe_.clear();
  dist_.clear();
  resampler_.reset();   // drop the old stream's interpolation history (ratio is re-set every hop)

  Frame silent{}; silent.fill(0.0f);
  for (size_t i = 0; i < ringB_.capacity() / 2; ++i) ringB_.try_push(silent);  // prime half-full

  {
    // Serialize the engine (re)build against reclaimRetiredEngines() (the main loop polls it while a
    // device reselect rebuilds here on the pipe thread; both mutate dyn_/engine_/dfn3Adapter_).
    std::lock_guard<std::mutex> lk(engMutex_);
    // Fold any resident DFN3's failedHops before the old container is destroyed below (its destructor
    // frees resident leaves with no onRetire hook), then null the observer so it never dangles across
    // the rebuild.
    if (dfn3Adapter_) { dfn3FailedHopsRetired_ += dfn3Adapter_->failedHops(); dfn3Adapter_ = nullptr; }

    // Engine: RNNoise (idx 0) / DFN3 (idx 1) live behind a DynamicEngine container that supports
    // warm standby (both resident) and lean on-demand switching. Passthrough ("none") stays static.
    // Per-engine algorithmic delays are known constants for the shipped model: RNNoise 0, DFN3 1440
    // (standard models/dfn3: Dfn3Streaming::latencySamples() = kHop*(1+latencyFrames) = 480*3). Pinned
    // so a switch never shifts output timing; the loader verifies a loaded DFN3 matches the pinned value.
    constexpr uint32_t kDfn3DelaySamples = 1440;
    auto loader = [this](int idx) -> std::unique_ptr<INoiseFilter> {
      if (idx == 0) return std::make_unique<RnnoiseFilter>();
      auto a = std::make_unique<Dfn3StreamAdapter>("models/dfn3");   // throws on model load failure
      if (a->AlgorithmicDelaySamples() != kDfn3DelaySamples)
        std::fprintf(stderr, "warning: DFN3 delay %u != pinned %u — crossfade timing will be off; update delaySamples\n",
                     a->AlgorithmicDelaySamples(), kDfn3DelaySamples);
      dfn3Adapter_ = a.get();
      return a;
    };
    if (cfg_.engine == "rnnoise" || cfg_.engine == "dfn3") {
      DynamicEngine::Config dc;
      dc.mode = cfg_.warmStandby ? DynamicEngine::Mode::Warm : DynamicEngine::Mode::Lean;
      dc.initialActive = (cfg_.engine == "dfn3") ? 1 : 0;
      dc.fadeFrames = 15;
      dc.delaySamples[0] = 0;
      dc.delaySamples[1] = cfg_.lowLatency ? 0u : kDfn3DelaySamples;   // low-latency: don't pin RNNoise to DFN3's delay
      auto de = std::make_unique<DynamicEngine>(loader, dc);   // builds initial leaf(s); may throw
      dyn_ = de.get();
      engine_ = std::move(de);
    } else {
      dyn_ = nullptr;
      engine_ = std::make_unique<PassthroughFilter>();   // "none": static passthrough, no switching
    }
    if (cfg_.dspChain) chain_ = std::make_unique<DspChain>(engine_.get(), DspChain::Config{});
  }

  frameReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  start_ = std::chrono::steady_clock::now();   // telemetry uptimeMs baseline
  running_.store(true, std::memory_order_release);
  deviceLost_.store(false, std::memory_order_release);   // reset any prior fatal state
  if (!cfg_.csvPath.empty()) logger_.start();   // developer drift CSV (--csv); off in normal use
  telemetryOn_ = telemetry_.create(L"Local\\voicecleaner-telemetry");

  auto errCb = [this](const char* w){
    std::fprintf(stderr, "audio error: %s\n", w);
    deviceLost_.store(true, std::memory_order_release);
  };

  if (!render_.start(renDev, [this](float* o, size_t n){ return onFill(o, n); }, errCb)) {
    std::fprintf(stderr, "error: render failed to start\n");
    capDev->Release(); renDev->Release(); stop(); return false;
  }
  relay_ = std::thread([this]{ relayLoop(); });
  if (!capture_.start(capDev, [this](const float* in, size_t f, int c){ onCapture(in, f, c); }, errCb)) {
    std::fprintf(stderr, "error: capture failed to start\n");
    capDev->Release(); renDev->Release(); stop(); return false;
  }

  capDev->Release(); renDev->Release();   // WASAPI classes Activated their own client; device no longer needed

  // Latency budget (F3): computed from the ACTUAL queried WASAPI buffer sizes + this session's config,
  // so it's a real per-device figure, not the doc estimate. App-controlled stages only (analog/driver
  // latency is outside the process). Republished each telemetry hop via estLatencyMs_.
  {
    LatencyInputs li;
    li.captureBufferFrames = capture_.bufferFrames();
    li.renderBufferFrames  = render_.bufferFrames();
    li.ringPrimedFrames    = (uint32_t)(ringB_.capacity() / 2);
    li.engineDelaySamples  = engine_ ? engine_->AlgorithmicDelaySamples() : 0u;
    li.sampleRate          = 48000.0;
    const LatencyBreakdown lb = compute_latency(li);
    estLatencyMs_ = (float)lb.totalMs;
    std::printf("latency (app-controlled, ms): capture=%.1f + framer=%.1f + ring=%.1f + engine=%.1f + render=%.1f = %.1f total\n",
                lb.captureMs, lb.framerMs, lb.ringMs, lb.engineMs, lb.renderMs, lb.totalMs);
  }

  std::printf("bridge running: ring=%zu frames (primed %zu), engine=%s, warm_standby=%s, dsp_chain=%s, csv=%s\n",
              ringB_.capacity(), ringB_.capacity()/2, cfg_.engine.c_str(),
              cfg_.warmStandby ? "on" : "off", cfg_.dspChain ? "on" : "off",
              cfg_.csvPath.empty() ? "off" : cfg_.csvPath.c_str());
  return true;
}

void Bridge::request_engine(int idx) {
  std::lock_guard<std::mutex> lk(engMutex_);
  if (!dyn_) return;   // passthrough: no switching
  try {
    dyn_->requestActive(idx);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "engine switch to %d failed (kept current engine): %s\n", idx, e.what());
  }
}

void Bridge::reset_counters() noexcept {
  dropouts_.reset();                                              // atomic counts: safe from this (non-RT) thread
  statsResetRequested_.store(true, std::memory_order_relaxed);    // relay clears frameStats_/ringBHigh_ next hop
}

void Bridge::reclaimRetiredEngines() {
  std::lock_guard<std::mutex> lk(engMutex_);
  if (!dyn_) return;
  dyn_->reclaim([this](INoiseFilter* p, int idx) {
    if (idx == 1) {   // a DFN3 leaf is about to be freed: preserve its failedHops, drop the dangling observer
      dfn3FailedHopsRetired_ += static_cast<Dfn3StreamAdapter*>(p)->failedHops();
      if (dfn3Adapter_ == p) dfn3Adapter_ = nullptr;
    }
  });
}

void Bridge::onCapture(const float* interleaved, size_t frames, int channels) {
  mono_.resize(frames);
  extract_channel(interleaved, frames, channels, 0, mono_.data());
  accum_.write(mono_.data(), frames);
  Frame f;
  while (accum_.pop_frame(f.data())) {
    if (!ringA_.try_push(f)) dropouts_.record_overrun();
  }
  SetEvent(static_cast<HANDLE>(frameReady_));
}

void Bridge::relayLoop() {
  MmcssThread mmcss;
  Frame f, f2;
  while (running_.load(std::memory_order_acquire)) {
    WaitForSingleObject(static_cast<HANDLE>(frameReady_), 200);
    if (!running_.load(std::memory_order_acquire)) break;
    if (statsResetRequested_.exchange(false, std::memory_order_relaxed)) {
      frameStats_.reset(); hopStats_.reset(); ringBHigh_ = 0;   // relay owns these -> reset here (race-free)
    }
    while (ringA_.try_pop(f)) {
      const auto hop0 = std::chrono::steady_clock::now();   // whole-hop timing start (F2): pop..push, everything below
      const vc::ipc::Params sp = params_.snapshot();
      if (chain_) {
        chain_->setHpfCutoffHz(sp.hpfCutoffHz);
        chain_->setAgcTargetDb(sp.agcTargetDbfs);
        chain_->setLimiterCeilingDb(sp.limiterCeilingDbfs);
        chain_->setDeclickStrength(sp.declickStrength);
        chain_->setDeclickVoiceFloor(sp.declickVoiceFloor);
        chain_->setGateStrength(sp.gateStrength);
        chain_->setMixStrength(sp.mixStrength);
        chain_->setAgcMaxGainDb(sp.agcMaxGainDb);
        chain_->setBypass(sp.bypass != 0);
      }
      if (engine_) engine_->SetAggression(sp.aggression);   // DFN3-only; RNNoise/passthrough no-op
      const float* src = f.data();
      const auto t0 = std::chrono::steady_clock::now();   // time ONLY engine+chain compute (Gate C)
      if (chain_)       { chain_->process_frame(f.data(), proc_.data()); src = proc_.data(); }
      else if (engine_) { engine_->ProcessFrame(f.data(), proc_.data()); src = proc_.data(); }
      const auto t1 = std::chrono::steady_clock::now();
      frameStats_.record(std::chrono::duration<double, std::milli>(t1 - t0).count());
      if (telemetryOn_) {
        float framePeak = 0.0f; double frameSumSq = 0.0;
        for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
          const float a = f[i] < 0 ? -f[i] : f[i];
          frameSumSq += (double)f[i] * (double)f[i];
          if (a > framePeak) framePeak = a;
        }
        if (framePeak > winPeak_) winPeak_ = framePeak;
        winSumSq_ += frameSumSq;
        { const uint32_t occ = (uint32_t)ringB_.size(); if (occ > ringBHigh_) ringBHigh_ = occ; }  // running high-water
        if (++hopsSincePublish_ >= 100) {   // ~1 s at 10 ms/hop
          const double n = (double)hopsSincePublish_ * (double)FRAME_SAMPLES;
          vc::ipc::Telemetry t{};
          t.inputRms = (float)std::sqrt(winSumSq_ / n);
          t.inputPeak = winPeak_;
          t.vadActive = (chain_ && chain_->vadActive()) ? 1u : 0u;
          t.agcGainDb = chain_ ? chain_->currentAgcGainDb() : 0.0f;
          t.frameP50Ms = (float)frameStats_.percentileMs(50.0);
          t.frameP99Ms = (float)frameStats_.percentileMs(99.0);
          t.frameMaxMs = (float)frameStats_.maxMs();
          t.hopP50Ms = (float)hopStats_.percentileMs(50.0);
          t.hopP99Ms = (float)hopStats_.percentileMs(99.0);
          t.hopMaxMs = (float)hopStats_.maxMs();
          t.estLatencyMs = estLatencyMs_;
          t.ringAOcc = (uint32_t)ringA_.size();
          t.ringBOcc = (uint32_t)ringB_.size();
          t.ringBHigh = ringBHigh_;   // monotonic since start/last ResetCounters
          t.uptimeMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start_).count();
          t.dropouts = (uint32_t)dropouts_.overruns();
          t.activeEngine = dyn_ ? (uint32_t)dyn_->activeIndex()
                                : (cfg_.engine == "dfn3" ? 1u : 0u);
          t.driftRatioPpm = ratioPpm_.load(std::memory_order_relaxed);
          t.workerState = (uint32_t)vc::ipc::WorkerState::Running;
          t.bypass   = (chain_ && sp.bypass != 0) ? 1u : 0u;   // true bypass only meaningful with a chain
          t.dspChain = chain_ ? 1u : 0u;                        // capability flag for the UI
          t.seq = ++telemetrySeq_;
          telemetry_.write(t);
          hopsSincePublish_ = 0; winPeak_ = 0.0f; winSumSq_ = 0.0;
        }
      }
      if (cfg_.driftCompensation) {
        const double ratio = controller_.update((double)ringB_.size());
        resampler_.set_ratio(ratio);
        ratioPpm_.store(controller_.ratioPpm(), std::memory_order_relaxed);
        filtOcc_.store(controller_.filteredOccupancy(), std::memory_order_relaxed);
        const size_t m = resampler_.process(src, FRAME_SAMPLES, rsOut_.data(), rsOut_.size());
        reframe_.write(rsOut_.data(), m);
        while (reframe_.pop_frame(f2.data()))
          if (!ringB_.try_push(f2)) dropouts_.record_overrun();
      } else {
        Frame pf; for (size_t i = 0; i < FRAME_SAMPLES; ++i) pf[i] = src[i];
        if (!ringB_.try_push(pf)) dropouts_.record_overrun();
      }
      // Whole-hop time (F2): everything from ringA pop through ringB push — the engine+chain
      // slice (frameStats_) plus params snapshot, telemetry aggregation, drift + resample +
      // reframe. Reveals the per-hop cost that the narrower frameStats_ window excludes.
      hopStats_.record(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - hop0).count());
    }
  }
}

size_t Bridge::onFill(float* outMono, size_t n) {
  Frame f;
  while (dist_.buffered() < n && ringB_.try_pop(f)) dist_.push_frame(f.data());
  size_t got = dist_.read(outMono, n);
  if (got < n) dropouts_.record_underrun();
  return got;
}

void Bridge::stop() {
  dropouts_.stop_accounting();
  capture_.stop();
  running_.store(false, std::memory_order_release);
  if (frameReady_) SetEvent(static_cast<HANDLE>(frameReady_));
  if (relay_.joinable()) {
    relay_.join();
    // Relay (sole writer of frameStats_) is now joined -> read with no data race.
    // Print inside this branch so the summary emits exactly once (stop() also runs from ~Bridge).
    if (frameStats_.count() > 0) {
      std::printf("frame-time (engine+chain, ms): n=%llu mean=%.2f max=%.2f p50=%.2f p99=%.2f p99.9=%.2f  over_10ms=%.2f%%\n",
                  (unsigned long long)frameStats_.count(),
                  frameStats_.meanMs(), frameStats_.maxMs(),
                  frameStats_.percentileMs(50.0), frameStats_.percentileMs(99.0), frameStats_.percentileMs(99.9),
                  frameStats_.fractionOverMs(10.0) * 100.0);
    }
    if (hopStats_.count() > 0) {
      std::printf("hop-time  (whole relay hop, ms): n=%llu mean=%.2f max=%.2f p50=%.2f p99=%.2f p99.9=%.2f  over_10ms=%.2f%%\n",
                  (unsigned long long)hopStats_.count(),
                  hopStats_.meanMs(), hopStats_.maxMs(),
                  hopStats_.percentileMs(50.0), hopStats_.percentileMs(99.0), hopStats_.percentileMs(99.9),
                  hopStats_.fractionOverMs(10.0) * 100.0);
    }
    {
      std::lock_guard<std::mutex> lk(engMutex_);   // dfn3Adapter_ may be written by a --dev-keys request_engine
      const uint64_t dfn3Failed = dfn3FailedHopsRetired_ + (dfn3Adapter_ ? dfn3Adapter_->failedHops() : 0);
      if (dfn3Failed > 0) {
        std::printf("dfn3 engine failures (silenced hops): %llu\n", (unsigned long long)dfn3Failed);
      }
    }
  }
  reclaimRetiredEngines();   // RT is joined; free any leaf the final fade retired
  render_.stop();
  logger_.stop();
  if (frameReady_) { CloseHandle(static_cast<HANDLE>(frameReady_)); frameReady_ = nullptr; }
  if (lockedA_) { rt_unlock(ringA_.data(), ringA_.byte_size()); lockedA_ = false; }
  if (lockedB_) { rt_unlock(ringB_.data(), ringB_.byte_size()); lockedB_ = false; }
  if (lockedAccum_)   { rt_unlock(accum_.data(),   accum_.byte_size());   lockedAccum_ = false; }
  if (lockedReframe_) { rt_unlock(reframe_.data(), reframe_.byte_size()); lockedReframe_ = false; }
  if (lockedDist_)    { rt_unlock(dist_.data(),    dist_.byte_size());    lockedDist_ = false; }
}

bool Bridge::reselectCapture(const std::string& deviceId, double driftSeedPpm) {
  stop();
  cfg_.captureDeviceId = deviceId;
  if (cfg_.driftCompensation) controller_.seedPpm(driftSeedPpm);  // no relay running here -> race-free
  return start();
}

bool Bridge::reselectRender(const std::string& deviceId) {
  stop();
  cfg_.renderDeviceId = deviceId;
  // A new output device has its own clock: restart drift tracking from zero instead of carrying the
  // old device's correction into the new pairing.
  if (cfg_.driftCompensation) controller_.seedPpm(0.0);   // no relay running here -> race-free
  return start();
}

Bridge::~Bridge() { stop(); }

}
