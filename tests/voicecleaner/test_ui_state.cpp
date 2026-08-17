#include <gtest/gtest.h>
#include "ui_state.h"
using namespace vc::ui;
using vc::ipc::Telemetry;

static Telemetry tel(uint32_t engine, uint32_t bypass, uint32_t dspChain, uint32_t seq) {
  Telemetry t{}; t.activeEngine = engine; t.bypass = bypass; t.dspChain = dspChain; t.seq = seq;
  t.workerState = (uint32_t)vc::ipc::WorkerState::Running;
  return t;
}

TEST(UiState, ActiveStatusLineFromReadyTelemetry) {
  UiState ui; ui.setRenderLabel("CABLE Input");
  ui.onTelemetry(tel(/*engine*/1, /*bypass*/0, /*dspChain*/1, /*seq*/1), /*ready*/true);
  EXPECT_EQ(ui.statusKind(), StatusKind::Active);
  EXPECT_EQ(ui.statusLine(),
            "\xE2\x97\x8F Active \xC2\xB7 DeepFilterNet3 \xC2\xB7 \xE2\x86\x92 CABLE Input");
  EXPECT_EQ(ui.activeEngine(), 1u);
}

TEST(UiState, BypassedStatusLineAndDisplay) {
  UiState ui; ui.setRenderLabel("CABLE Input");
  ui.onTelemetry(tel(/*engine*/0, /*bypass*/1, /*dspChain*/1, /*seq*/1), /*ready*/true);
  EXPECT_EQ(ui.statusKind(), StatusKind::Bypassed);
  EXPECT_TRUE(ui.bypassed());
  EXPECT_EQ(ui.statusLine(),
            "\xE2\x97\x8B Bypassed \xC2\xB7 RNNoise \xC2\xB7 \xE2\x86\x92 CABLE Input");
}

TEST(UiState, PreReadyTelemetryDoesNotFlipToActive) {
  UiState ui;
  ui.onTelemetry(tel(1, 0, 1, 1), /*ready*/false);   // present but not yet ready
  EXPECT_EQ(ui.statusKind(), StatusKind::Down);       // stays Down until ready
  EXPECT_EQ(ui.activeEngine(), 1u);                   // display fields still track telemetry
}

TEST(UiState, BypassControlDisabledWhenNoChain) {
  UiState ui;
  ui.onTelemetry(tel(/*engine*/0, /*bypass*/0, /*dspChain*/0, /*seq*/1), /*ready*/true);
  EXPECT_FALSE(ui.bypassEnabled());
  EXPECT_EQ(ui.bypassDisabledReason(), "no DSP chain \xE2\x80\x94 bypass unavailable");
}

TEST(UiState, BypassControlEnabledWithChain) {
  UiState ui;
  ui.onTelemetry(tel(0, 0, /*dspChain*/1, 1), /*ready*/true);
  EXPECT_TRUE(ui.bypassEnabled());
}

TEST(UiState, AttachReflectsTrueBypassAndCapability) {
  // Attach to a worker already bypassed, with a chain: the very first telemetry must show it.
  UiState ui;
  ui.onTelemetry(tel(/*engine*/1, /*bypass*/1, /*dspChain*/1, /*seq*/42), /*ready*/true);
  EXPECT_TRUE(ui.bypassed());
  EXPECT_TRUE(ui.bypassEnabled());
  EXPECT_EQ(ui.statusKind(), StatusKind::Bypassed);
}

TEST(UiState, WorkerDownStatus) {
  UiState ui;
  ui.onTelemetry(tel(1, 0, 1, 1), true);   // was Active
  ui.onWorkerDown();
  EXPECT_EQ(ui.statusKind(), StatusKind::Down);
  EXPECT_EQ(ui.statusLine(), "\xE2\x9C\x95 Worker not running");
}

TEST(UiState, NoConfigStatus) {
  UiState ui;
  ui.onNoConfig();
  EXPECT_EQ(ui.statusKind(), StatusKind::NoConfig);
  EXPECT_EQ(ui.statusLine(),
            "No microphone configured \xE2\x80\x94 the mic picker arrives in Phase 4");
}
