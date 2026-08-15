#include "bridge.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <string>
#include "channel.h"
#include "device_enumerator.h"
#include "mmcss_thread.h"
#include "rt_memory.h"
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

  Frame silent{}; silent.fill(0.0f);
  for (size_t i = 0; i < ringB_.capacity() / 2; ++i) ringB_.try_push(silent);  // prime half-full

  // Engine + optional DSP chain: constructed here (non-RT) before any thread starts.
  if (cfg_.warmStandby) {
    // Both leaves run warm; SwitchableEngine crossfades between them on request_engine().
    leafA_ = std::make_unique<RnnoiseFilter>();
    leafB_ = std::make_unique<Dfn3StreamAdapter>("models/dfn3");
    dfn3Adapter_ = static_cast<Dfn3StreamAdapter*>(leafB_.get());
    // In warm-standby, `engine` selects which engine is active at startup: "dfn3" -> idx 1 (leafB),
    // anything else -> idx 0 (RNNoise, leafA). Both stay warm; switch live with keys 1/2.
    const int initialActive = (cfg_.engine == "dfn3") ? 1 : 0;
    auto sw = std::make_unique<SwitchableEngine>(leafA_.get(), leafB_.get(), /*fadeFrames=*/15, initialActive);
    switch_ = sw.get();
    engine_ = std::move(sw);
  }
  else if (cfg_.engine == "rnnoise") engine_ = std::make_unique<RnnoiseFilter>();
  else if (cfg_.engine == "dfn3") {
    auto adapter = std::make_unique<Dfn3StreamAdapter>("models/dfn3");
    dfn3Adapter_ = adapter.get();
    engine_ = std::move(adapter);
  }
  else                               engine_ = std::make_unique<PassthroughFilter>();
  if (cfg_.dspChain) chain_ = std::make_unique<DspChain>(engine_.get(), DspChain::Config{});

  frameReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  running_.store(true, std::memory_order_release);
  deviceLost_.store(false, std::memory_order_release);   // reset any prior fatal state
  logger_.start();

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
  if (cfg_.warmStandby) {
    std::printf("bridge running: ring=%zu frames (primed %zu), warm_standby=on "
                "(active=%s [idx %d]; keys 1=RNNoise 2=DFN3), dsp_chain=%s, csv=%s\n",
                ringB_.capacity(), ringB_.capacity()/2,
                switch_->activeIndex() == 0 ? "rnnoise" : "dfn3", switch_->activeIndex(),
                cfg_.dspChain ? "on" : "off", cfg_.csvPath.c_str());
  } else {
    std::printf("bridge running: ring=%zu frames (primed %zu), engine=%s, dsp_chain=%s, csv=%s\n",
                ringB_.capacity(), ringB_.capacity()/2,
                cfg_.engine.c_str(), cfg_.dspChain ? "on" : "off", cfg_.csvPath.c_str());
  }
  return true;
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
    while (ringA_.try_pop(f)) {
      const float* src = f.data();
      const auto t0 = std::chrono::steady_clock::now();   // time ONLY engine+chain compute (Gate C)
      if (chain_)       { chain_->process_frame(f.data(), proc_.data()); src = proc_.data(); }
      else if (engine_) { engine_->ProcessFrame(f.data(), proc_.data()); src = proc_.data(); }
      const auto t1 = std::chrono::steady_clock::now();
      frameStats_.record(std::chrono::duration<double, std::milli>(t1 - t0).count());
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
    if (dfn3Adapter_ && dfn3Adapter_->failedHops() > 0) {
      std::printf("dfn3 engine failures (silenced hops): %llu\n",
                  (unsigned long long)dfn3Adapter_->failedHops());
    }
  }
  render_.stop();
  logger_.stop();
  if (frameReady_) { CloseHandle(static_cast<HANDLE>(frameReady_)); frameReady_ = nullptr; }
  if (lockedA_) { rt_unlock(ringA_.data(), ringA_.byte_size()); lockedA_ = false; }
  if (lockedB_) { rt_unlock(ringB_.data(), ringB_.byte_size()); lockedB_ = false; }
}

Bridge::~Bridge() { stop(); }

}
