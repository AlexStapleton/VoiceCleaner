#include <gtest/gtest.h>
#include "device_picker.h"
using namespace vc::ui;

static DeviceRow row(const char* id, const char* name, uint32_t sr) {
  DeviceRow r; r.id = id; r.name = name; r.sampleRate = sr; r.is48k = (sr == 48000); return r;
}

TEST(DevicePicker, FieldLabelPresentAbsentEmpty) {
  DevicePicker p;
  p.setDevices({row("c1","K688",48000), row("c2","USB Mic",44100)}, {row("r1","CABLE Input",48000)});
  p.setActive("c1", "r1");
  EXPECT_EQ(p.fieldLabel(PickerKind::Capture), "K688");
  EXPECT_EQ(p.fieldLabel(PickerKind::Render), "CABLE Input");
  p.setActive("gone", "r1");
  EXPECT_EQ(p.fieldLabel(PickerKind::Capture), "(not connected)");
  EXPECT_TRUE(p.deviceGone(PickerKind::Capture));
  p.setActive("", "r1");
  EXPECT_EQ(p.fieldLabel(PickerKind::Capture), "Select a device...");
  EXPECT_FALSE(p.deviceGone(PickerKind::Capture));   // empty active is not "gone"
}

TEST(DevicePicker, NonFortyEightKIsNotSelectable) {
  DeviceRow bad = row("c2","USB Mic",44100);
  DeviceRow good = row("c1","K688",48000);
  EXPECT_FALSE(DevicePicker::selectable(bad));
  EXPECT_TRUE(DevicePicker::selectable(good));
}
