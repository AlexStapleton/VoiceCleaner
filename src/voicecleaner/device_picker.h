#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace vc::ui {

struct DeviceRow { std::string id; std::string name; uint32_t sampleRate = 0; bool is48k = false; };
enum class PickerKind { Capture, Render };

// Pure picker state. Fed enumerated device rows + the active ids; computes the field
// label, which rows are selectable, device-gone, and the open/close/pick transitions. No Windows.
class DevicePicker {
 public:
  void setDevices(std::vector<DeviceRow> capture, std::vector<DeviceRow> render);
  void setActive(std::string captureId, std::string renderId);

  void open(PickerKind k) { open_ = true; openKind_ = k; }
  void close() { open_ = false; }
  bool isOpen() const { return open_; }
  PickerKind openKind() const { return openKind_; }

  const std::vector<DeviceRow>& rows(PickerKind k) const;
  const std::string& activeId(PickerKind k) const;
  std::string fieldLabel(PickerKind k) const;                 // name / "(not connected)" / "Select a device..."
  static bool selectable(const DeviceRow& r) { return r.is48k; }
  bool deviceGone(PickerKind k) const;                        // active id set but absent from rows
  // Click a list row: returns the chosen id (and updates active + closes) iff selectable & changed; else "".
  std::string pick(PickerKind k, size_t rowIndex);

 private:
  std::vector<DeviceRow> capture_, render_;
  std::string activeCapture_, activeRender_;
  bool open_ = false;
  PickerKind openKind_ = PickerKind::Capture;
};

}  // namespace vc::ui
