#include "wasapi_capture.h"
#include <windows.h>
#include <avrt.h>
#include <cstdint>
#include "mmcss_thread.h"
namespace vc {

bool WasapiCapture::start(IMMDevice* dev, SampleCb onSamples, ErrorCb onError) {
  if (running_.load(std::memory_order_acquire)) return false;   // already started
  onSamples_ = std::move(onSamples);
  onError_   = std::move(onError);
  if (dev == nullptr) return false;

  HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_);
  if (FAILED(hr)) return false;

  WAVEFORMATEX* mix = nullptr;
  hr = client_->GetMixFormat(&mix);
  if (FAILED(hr) || mix == nullptr) return false;

  FormatCheck fc = DeviceEnumerator::check_format(mix);
  channels_ = mix->nChannels;
  kind_ = fc.kind;
  if (!fc.ok) {
    if (onError_) onError_(fc.message.c_str());
    CoTaskMemFree(mix);
    return false;
  }

  hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                           0, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) return false;

  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event_ == nullptr) return false;
  hr = client_->SetEventHandle(static_cast<HANDLE>(event_));
  if (FAILED(hr)) return false;

  hr = client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_);
  if (FAILED(hr)) return false;

  scratch_.reserve((size_t)channels_ * 4800);   // ~100 ms headroom, avoids steady-state realloc

  running_.store(true, std::memory_order_release);
  hr = client_->Start();
  if (FAILED(hr)) { running_.store(false, std::memory_order_release); return false; }

  thread_ = std::thread([this]{ run(); });
  return true;
}

void WasapiCapture::run() {
  MmcssThread mmcss;   // register this thread as Pro Audio / CRITICAL (reverts on scope exit)
  while (running_.load(std::memory_order_acquire)) {
    DWORD wr = WaitForSingleObject(static_cast<HANDLE>(event_), 200);
    if (!running_.load(std::memory_order_acquire)) break;
    if (wr != WAIT_OBJECT_0) continue;   // timeout -> re-check running_

    UINT32 packet = 0;
    HRESULT hr = capture_->GetNextPacketSize(&packet);
    if (FAILED(hr)) { if (onError_) onError_("GetNextPacketSize failed"); running_.store(false, std::memory_order_release); break; }
    while (packet != 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { if (onError_) onError_("capture device invalidated"); running_.store(false, std::memory_order_release); return; }
      if (FAILED(hr)) { if (onError_) onError_("capture GetBuffer failed"); running_.store(false, std::memory_order_release); return; }

      const size_t total = (size_t)frames * channels_;
      const float* out = nullptr;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        scratch_.assign(total, 0.0f);
        out = scratch_.data();
      } else if (kind_ == SampleKind::Float32) {
        out = reinterpret_cast<const float*>(data);
      } else { // Pcm16
        scratch_.resize(total);
        const int16_t* src = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < total; ++i) scratch_[i] = src[i] * (1.0f / 32768.0f);
        out = scratch_.data();
      }
      if (frames > 0 && onSamples_) onSamples_(out, frames, channels_);

      capture_->ReleaseBuffer(frames);
      hr = capture_->GetNextPacketSize(&packet);
      if (FAILED(hr)) { if (onError_) onError_("GetNextPacketSize failed"); running_.store(false, std::memory_order_release); return; }
    }
  }
}

void WasapiCapture::stop() {
  running_.store(false, std::memory_order_release);
  if (event_) SetEvent(static_cast<HANDLE>(event_));   // wake the wait
  if (thread_.joinable()) thread_.join();
  if (client_) client_->Stop();
  if (capture_) { capture_->Release(); capture_ = nullptr; }
  if (client_)  { client_->Release();  client_  = nullptr; }
  if (event_)   { CloseHandle(static_cast<HANDLE>(event_)); event_ = nullptr; }
}

WasapiCapture::~WasapiCapture() { stop(); }

}
