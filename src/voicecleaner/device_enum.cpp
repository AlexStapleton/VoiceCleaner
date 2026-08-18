#include "device_enum.h"
#include "device_enumerator.h"   // vc::DeviceEnumerator (win-audio)
#include <windows.h>
namespace vc::ui {

static std::string toUtf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
static std::string toAscii(const std::wstring& w) {   // device IDs are ASCII
  std::string s; s.reserve(w.size());
  for (wchar_t ch : w) s.push_back(static_cast<char>(ch));
  return s;
}

static std::vector<DeviceRow> rowsFor(vc::DeviceEnumerator& e, EDataFlow flow) {
  std::vector<DeviceRow> out;
  for (const auto& d : e.list(flow)) {
    DeviceRow r;
    r.id = toAscii(d.id);
    r.name = toUtf8(d.name);
    r.sampleRate = d.sampleRate;
    r.is48k = (d.sampleRate == 48000);
    out.push_back(std::move(r));
  }
  return out;
}

void enumerateDevices(std::vector<DeviceRow>& capture, std::vector<DeviceRow>& render) {
  vc::DeviceEnumerator e;
  capture = rowsFor(e, eCapture);
  render  = rowsFor(e, eRender);
}

}  // namespace vc::ui
