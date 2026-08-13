#include "bridge.h"
#include <windows.h>
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
    logger_(cfg.csvPath, &ringA_, &ringB_, &dropouts_) {
  mono_.reserve(48000);   // 1 s ch0 scratch, avoids steady-state realloc
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

  frameReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  running_.store(true, std::memory_order_release);
  logger_.start();

  auto errCb = [](const char* w){ std::fprintf(stderr, "audio error: %s\n", w); };

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
  std::printf("bridge running: ring=%zu frames (primed %zu), csv=%s\n",
              ringB_.capacity(), ringB_.capacity()/2, cfg_.csvPath.c_str());
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
  Frame f;
  while (running_.load(std::memory_order_acquire)) {
    WaitForSingleObject(static_cast<HANDLE>(frameReady_), 200);
    if (!running_.load(std::memory_order_acquire)) break;
    while (ringA_.try_pop(f)) {
      if (!ringB_.try_push(f)) dropouts_.record_overrun();
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
  if (relay_.joinable()) relay_.join();
  render_.stop();
  logger_.stop();
  if (frameReady_) { CloseHandle(static_cast<HANDLE>(frameReady_)); frameReady_ = nullptr; }
  if (lockedA_) { rt_unlock(ringA_.data(), ringA_.byte_size()); lockedA_ = false; }
  if (lockedB_) { rt_unlock(ringB_.data(), ringB_.byte_size()); lockedB_ = false; }
}

Bridge::~Bridge() { stop(); }

}
