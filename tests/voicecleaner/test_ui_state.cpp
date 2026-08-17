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
