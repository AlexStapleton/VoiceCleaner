#include "device_enum.h"
#include "device_enumerator.h"   // vc::DeviceEnumerator (win-audio)
#include "utf8.h"
namespace vc::ui {

static std::vector<DeviceRow> rowsFor(vc::DeviceEnumerator& e, EDataFlow flow) {
  std::vector<DeviceRow> out;
  for (const auto& d : e.list(flow)) {
    DeviceRow r;
    r.id = vc::wideToUtf8(d.id);
    r.name = vc::wideToUtf8(d.name);
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
