#include <gtest/gtest.h>
#include "command.h"
using namespace vc::ipc;

static std::vector<uint8_t> roundtrip_ok(const Command& in, Command& out) {
  auto bytes = encode(in);
  EXPECT_TRUE(decode(bytes.data(), bytes.size(), out));
  return bytes;
}

TEST(Codec, SetParamsRoundTripsExactly) {
  Command in; in.type = CmdType::SetParams;
  in.params.hpfCutoffHz = 123.5f; in.params.agcTargetDbfs = -9.25f;
  in.params.bypass = 1;
  Command out; roundtrip_ok(in, out);
  EXPECT_EQ(out.type, CmdType::SetParams);
  EXPECT_FLOAT_EQ(out.params.hpfCutoffHz, 123.5f);
  EXPECT_FLOAT_EQ(out.params.agcTargetDbfs, -9.25f);
  EXPECT_EQ(out.params.bypass, 1u);
}
TEST(Codec, SelectCaptureDeviceRoundTripsIdAndSeed) {
  Command in; in.type = CmdType::SelectCaptureDevice;
  in.deviceId = "{0.0.1.00000000}.{5fbcc184}"; in.driftSeedPpm = -26.0;
  Command out; roundtrip_ok(in, out);
  EXPECT_EQ(out.type, CmdType::SelectCaptureDevice);
  EXPECT_EQ(out.deviceId, in.deviceId);
  EXPECT_DOUBLE_EQ(out.driftSeedPpm, -26.0);
}
TEST(Codec, ShutdownRoundTrips) {
  Command in; in.type = CmdType::Shutdown;
  Command out; roundtrip_ok(in, out);
  EXPECT_EQ(out.type, CmdType::Shutdown);
}
TEST(Codec, RejectsTooShort) {
  Command out; uint8_t junk[3] = {1,2,3};
  EXPECT_FALSE(decode(junk, sizeof(junk), out));
}
TEST(Codec, RejectsBadMagic) {
  Command out; uint8_t buf[8] = {'X','X','X','X', 1,0,0,0};
  EXPECT_FALSE(decode(buf, sizeof(buf), out));
}
TEST(Codec, RejectsTruncatedPayload) {
  Command in; in.type = CmdType::SetParams;
  auto bytes = encode(in);
  bytes.resize(bytes.size() - 4);   // chop the params tail
  Command out;
  EXPECT_FALSE(decode(bytes.data(), bytes.size(), out));
}
TEST(Codec, RejectsOldMagicAndRetiredType) {
  // A pre-v1.5 (VCC1/VCC2) peer must be refused rather than misread: Params changed layout.
  Command out;
  uint8_t v1[8] = {'V','C','C','1', 6,0,0,0};
  EXPECT_FALSE(decode(v1, sizeof(v1), out));
  uint8_t v2[8] = {'V','C','C','2', 6,0,0,0};
  EXPECT_FALSE(decode(v2, sizeof(v2), out));
  uint8_t retired[12] = {'V','C','C','3', 4,0,0,0, 1,0,0,0};   // 4 is a retired type
  EXPECT_FALSE(decode(retired, sizeof(retired), out));
}
