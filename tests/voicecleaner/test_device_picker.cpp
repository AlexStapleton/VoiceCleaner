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

TEST(DevicePicker, OpenCloseTracksKind) {
  DevicePicker p;
  EXPECT_FALSE(p.isOpen());
  p.open(PickerKind::Render);
  EXPECT_TRUE(p.isOpen());
  EXPECT_EQ(p.openKind(), PickerKind::Render);
  p.close();
  EXPECT_FALSE(p.isOpen());
}

TEST(DevicePicker, PickSelectableChangedReturnsIdAndCloses) {
  DevicePicker p;
  p.setDevices({row("c1","K688",48000), row("c2","USB",44100)}, {});
  p.setActive("c1", "");
  p.open(PickerKind::Capture);
  EXPECT_EQ(p.pick(PickerKind::Capture, 1), "");   // c2 is 44.1k -> not selectable, no-op
  EXPECT_TRUE(p.isOpen());
  EXPECT_EQ(p.pick(PickerKind::Capture, 0), "");   // c1 already active -> no-op
  // add a second 48k device and pick it
  p.setDevices({row("c1","K688",48000), row("c3","Yeti",48000)}, {});
  p.open(PickerKind::Capture);
  EXPECT_EQ(p.pick(PickerKind::Capture, 1), "c3"); // selectable + changed
  EXPECT_FALSE(p.isOpen());                         // pick closes
  EXPECT_EQ(p.activeId(PickerKind::Capture), "c3");
  EXPECT_EQ(p.pick(PickerKind::Capture, 9), "");   // out of range -> no-op
}
