#include "wasapi_render.h"
#include <windows.h>
#include <avrt.h>
#include <cstdint>
#include <algorithm>
#include "mmcss_thread.h"
#include "channel.h"
namespace vc {

bool WasapiRender::start(IMMDevice* dev, FillCb onFill, ErrorCb onError) {
  onFill_  = std::move(onFill);
  onError_ = std::move(onError);
  if (dev == nullptr) return false;

  HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_);
  if (FAILED(hr)) return false;

  WAVEFORMATEX* mix = nullptr;
  hr = client_->GetMixFormat(&mix);
  if (FAILED(hr) || mix == nullptr) return false;

  FormatCheck fc = DeviceEnumerator::check_format(mix);
  channels_ = mix->nChannels;
  kind_ = fc.kind;
  if (!fc.ok) { if (onError_) onError_(fc.message.c_str()); CoTaskMemFree(mix); return false; }

  hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) return false;

  hr = client_->GetBufferSize(&bufferFrames_);
  if (FAILED(hr)) return false;

  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event_ == nullptr) return false;
  hr = client_->SetEventHandle(static_cast<HANDLE>(event_));
  if (FAILED(hr)) return false;

  hr = client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_);
  if (FAILED(hr)) return false;

  // Pre-roll one full buffer of silence so we never underrun the engine at startup.
  BYTE* data = nullptr;
  hr = render_->GetBuffer(bufferFrames_, &data);
  if (SUCCEEDED(hr)) render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);

  mono_.reserve(bufferFrames_);

  running_.store(true, std::memory_order_release);
  hr = client_->Start();
  if (FAILED(hr)) { running_.store(false, std::memory_order_release); return false; }

  thread_ = std::thread([this]{ run(); });
  return true;
}

void WasapiRender::run() {
  MmcssThread mmcss;
  while (running_.load(std::memory_order_acquire)) {
    DWORD wr = WaitForSingleObject(static_cast<HANDLE>(event_), 200);
    if (!running_.load(std::memory_order_acquire)) break;
    if (wr != WAIT_OBJECT_0) continue;

    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { if (onError_) onError_("render device invalidated"); running_.store(false, std::memory_order_release); return; }
    if (FAILED(hr)) { if (onError_) onError_("GetCurrentPadding failed"); running_.store(false, std::memory_order_release); return; }

    UINT32 avail = bufferFrames_ - padding;
    if (avail == 0) continue;

    BYTE* data = nullptr;
    hr = render_->GetBuffer(avail, &data);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { if (onError_) onError_("render device invalidated"); running_.store(false, std::memory_order_release); return; }
    if (FAILED(hr)) { if (onError_) onError_("render GetBuffer failed"); running_.store(false, std::memory_order_release); return; }

    mono_.resize(avail);
    size_t got = onFill_ ? onFill_(mono_.data(), avail) : 0;
    if (got < avail) std::fill(mono_.begin() + got, mono_.end(), 0.0f);   // underrun -> silence

    if (kind_ == SampleKind::Float32) {
      mono_to_interleaved(mono_.data(), avail, channels_, reinterpret_cast<float*>(data));
    } else { // Pcm16
      int16_t* out = reinterpret_cast<int16_t*>(data);
      for (UINT32 i = 0; i < avail; ++i) {
        float s = mono_[i];
        s = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
        int16_t v = static_cast<int16_t>(s * 32767.0f);
        for (int c = 0; c < channels_; ++c) out[(size_t)i * channels_ + c] = v;
      }
    }
    render_->ReleaseBuffer(avail, 0);
  }
}

void WasapiRender::stop() {
  running_.store(false, std::memory_order_release);
  if (event_) SetEvent(static_cast<HANDLE>(event_));
  if (thread_.joinable()) thread_.join();
  if (client_) client_->Stop();
}

WasapiRender::~WasapiRender() {
  stop();
  if (render_) { render_->Release(); render_ = nullptr; }
  if (client_) { client_->Release(); client_ = nullptr; }
  if (event_)  { CloseHandle(static_cast<HANDLE>(event_)); event_ = nullptr; }
}

}
