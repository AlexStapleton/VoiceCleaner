#include <gtest/gtest.h>
#include "telemetry_shm.h"
using namespace vc::ipc;

TEST(TelemetryShm, WriterToReaderRoundTrip) {
  const std::wstring name = L"Local\\voicecleaner-test-telemetry";
  TelemetryWriter w; ASSERT_TRUE(w.create(name));
  TelemetryReader r; ASSERT_TRUE(r.open(name));
  Telemetry t{}; t.inputRms = 0.25f; t.dropouts = 3; t.activeEngine = 1;
  t.driftRatioPpm = -26.0; t.seq = 7; t.bypass = 1; t.dspChain = 1;
  w.write(t);
  Telemetry got = r.read();
  EXPECT_FLOAT_EQ(got.inputRms, 0.25f);
  EXPECT_EQ(got.dropouts, 3u);
  EXPECT_EQ(got.activeEngine, 1u);
  EXPECT_DOUBLE_EQ(got.driftRatioPpm, -26.0);
  EXPECT_EQ(got.seq, 7u);
  EXPECT_EQ(got.bypass, 1u);
  EXPECT_EQ(got.dspChain, 1u);
}
TEST(TelemetryShm, ReaderFailsWhenMappingAbsent) {
  TelemetryReader r; EXPECT_FALSE(r.open(L"Local\\voicecleaner-test-absent"));
}
