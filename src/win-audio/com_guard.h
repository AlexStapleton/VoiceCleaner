#pragma once
#include <objbase.h>
#include <stdexcept>
namespace vc {
struct ComGuard {
  ComGuard() { HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
               if (FAILED(hr)) throw std::runtime_error("CoInitializeEx failed"); }
  ~ComGuard() { CoUninitialize(); }
};
}
