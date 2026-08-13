#include "device_enumerator.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <propvarutil.h>

namespace vc {

namespace {

SampleKind classify(const WAVEFORMATEX* wfx) {
  if (wfx == nullptr) return SampleKind::Unsupported;

  if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32) {
    return SampleKind::Float32;
  }
  if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16) {
    return SampleKind::Pcm16;
  }
  if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
    const WAVEFORMATEXTENSIBLE* wfxExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
    if (wfxExt->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && wfx->wBitsPerSample == 32) {
      return SampleKind::Float32;
    }
    if (wfxExt->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && wfx->wBitsPerSample == 16) {
      return SampleKind::Pcm16;
    }
  }
  return SampleKind::Unsupported;
}

}  // namespace

std::vector<DeviceInfo> DeviceEnumerator::list(EDataFlow flow) {
  std::vector<DeviceInfo> result;

  IMMDeviceEnumerator* pEnum = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&pEnum));
  if (FAILED(hr) || pEnum == nullptr) {
    return result;
  }

  IMMDeviceCollection* pColl = nullptr;
  hr = pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pColl);
  if (FAILED(hr) || pColl == nullptr) {
    pEnum->Release();
    return result;
  }

  UINT count = 0;
  pColl->GetCount(&count);

  for (UINT i = 0; i < count; ++i) {
    IMMDevice* pDevice = nullptr;
    if (FAILED(pColl->Item(i, &pDevice)) || pDevice == nullptr) {
      continue;
    }

    LPWSTR idStr = nullptr;
    if (FAILED(pDevice->GetId(&idStr)) || idStr == nullptr) {
      pDevice->Release();
      continue;
    }
    std::wstring id(idStr);
    CoTaskMemFree(idStr);
    idStr = nullptr;

    std::wstring name;
    IPropertyStore* pStore = nullptr;
    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore)) && pStore != nullptr) {
      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal != nullptr) {
        name = pv.pwszVal;
      }
      PropVariantClear(&pv);
      pStore->Release();
    }

    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    SampleKind kind = SampleKind::Unsupported;

    IAudioClient* pClient = nullptr;
    if (SUCCEEDED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&pClient))) && pClient != nullptr) {
      WAVEFORMATEX* pwfx = nullptr;
      if (SUCCEEDED(pClient->GetMixFormat(&pwfx)) && pwfx != nullptr) {
        sampleRate = pwfx->nSamplesPerSec;
        channels = pwfx->nChannels;
        kind = classify(pwfx);
        CoTaskMemFree(pwfx);
      }
      pClient->Release();
    }

    pDevice->Release();

    result.push_back(DeviceInfo{std::move(id), std::move(name), sampleRate, channels, kind});
  }

  pColl->Release();
  pEnum->Release();

  return result;
}

IMMDevice* DeviceEnumerator::resolve(const std::wstring& id) {
  IMMDeviceEnumerator* pEnum = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&pEnum));
  if (FAILED(hr) || pEnum == nullptr) {
    return nullptr;
  }

  IMMDevice* pDevice = nullptr;
  hr = pEnum->GetDevice(id.c_str(), &pDevice);
  pEnum->Release();

  if (FAILED(hr)) {
    return nullptr;
  }
  return pDevice;
}

FormatCheck DeviceEnumerator::check_format(const WAVEFORMATEX* mix) {
  if (mix == nullptr) {
    return FormatCheck{false, SampleKind::Unsupported, 0, "No mix format available."};
  }

  if (mix->nSamplesPerSec != 48000) {
    return FormatCheck{
        false, classify(mix), mix->nChannels,
        "Endpoint is not 48000 Hz - set it in Sound Settings > Device Properties > "
        "Advanced (both VB-Cable endpoints and the mic must be 48000 Hz)."};
  }

  SampleKind kind = classify(mix);
  if (kind == SampleKind::Unsupported) {
    return FormatCheck{false, kind, mix->nChannels,
                        "Unsupported sample format (need 32-bit float or 16-bit PCM shared-mode)."};
  }

  return FormatCheck{true, kind, mix->nChannels, ""};
}

}  // namespace vc
