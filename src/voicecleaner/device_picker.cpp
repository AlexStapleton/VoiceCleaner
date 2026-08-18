#include "device_picker.h"
namespace vc::ui {

void DevicePicker::setDevices(std::vector<DeviceRow> capture, std::vector<DeviceRow> render) {
  capture_ = std::move(capture); render_ = std::move(render);
}
void DevicePicker::setActive(std::string captureId, std::string renderId) {
  activeCapture_ = std::move(captureId); activeRender_ = std::move(renderId);
}
const std::vector<DeviceRow>& DevicePicker::rows(PickerKind k) const {
  return k == PickerKind::Capture ? capture_ : render_;
}
const std::string& DevicePicker::activeId(PickerKind k) const {
  return k == PickerKind::Capture ? activeCapture_ : activeRender_;
}
std::string DevicePicker::fieldLabel(PickerKind k) const {
  const std::string& id = activeId(k);
  if (id.empty()) return "Select a device...";
  for (const auto& r : rows(k)) if (r.id == id) return r.name;
  return "(not connected)";
}
bool DevicePicker::deviceGone(PickerKind k) const {
  const std::string& id = activeId(k);
  if (id.empty()) return false;
  for (const auto& r : rows(k)) if (r.id == id) return false;
  return true;
}
std::string DevicePicker::pick(PickerKind k, size_t rowIndex) {
  const auto& rs = rows(k);
  if (rowIndex >= rs.size()) return "";
  const DeviceRow& r = rs[rowIndex];
  if (!selectable(r)) return "";
  if (r.id == activeId(k)) return "";
  (k == PickerKind::Capture ? activeCapture_ : activeRender_) = r.id;
  open_ = false;
  return r.id;
}

}  // namespace vc::ui
