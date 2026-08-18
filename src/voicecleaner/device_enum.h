#pragma once
#include <vector>
#include "device_picker.h"   // vc::ui::DeviceRow
namespace vc::ui {
// Enumerate WASAPI capture + render devices into DeviceRows (id ASCII, name UTF-8, is48k flag).
// COM must already be initialized (main.cpp CoInitializeEx). Fills empty vectors on failure.
void enumerateDevices(std::vector<DeviceRow>& capture, std::vector<DeviceRow>& render);
}  // namespace vc::ui
