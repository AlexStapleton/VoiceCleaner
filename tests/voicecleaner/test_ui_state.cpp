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
            "\xE2\x97\x8F Active \xC2\xB7 DeepFilterNet3 \xC2\xB7 \xE2\x86\x92 CABLE Input"
            " \xC2\xB7 VAD \xE2\x97\x8B \xC2\xB7 AGC +0.0 dB");
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

TEST(UiState, BypassToggleCarriesLiveEngineNotStaleDefault) {
  // Params::defaults().engine == 1 (DFN3). Worker is live on engine 0 (RNNoise).
  UiState ui;
  ui.onTelemetry(tel(/*engine*/0, /*bypass*/0, /*dspChain*/1, /*seq*/1), /*ready*/true);
  vc::ipc::Params sent = ui.onToggleBypass();
  EXPECT_EQ(sent.engine, 0u);   // synced to live engine, NOT the stale default 1
  EXPECT_EQ(sent.bypass, 1u);
}

TEST(UiState, SelectEngineSetsOutgoingEngine) {
  UiState ui;
  ui.onTelemetry(tel(/*engine*/1, 0, 1, 1), true);
  vc::ipc::Params sent = ui.onSelectEngine(0u);
  EXPECT_EQ(sent.engine, 0u);
}

TEST(UiState, BypassToggleFlipsBackAndForth) {
  UiState ui;
  ui.onTelemetry(tel(1, /*bypass*/0, 1, 1), true);
  EXPECT_EQ(ui.onToggleBypass().bypass, 1u);   // off -> on
  ui.onTelemetry(tel(1, /*bypass*/1, 1, 2), true);   // worker confirms bypassed
  EXPECT_EQ(ui.onToggleBypass().bypass, 0u);   // on -> off
}

TEST(UiState, ToggleFromAttachedBypassedTurnsItOff) {
  // Attach to a worker already bypassed (fresh UiState, no prior toggle). The first toggle must
  // turn bypass OFF, which only works if current_.bypass is synced from telemetry — the mirror of
  // the engine-sync invariant, on the bypass field.
  UiState ui;
  ui.onTelemetry(tel(/*engine*/1, /*bypass*/1, /*dspChain*/1, /*seq*/1), /*ready*/true);
  vc::ipc::Params sent = ui.onToggleBypass();
  EXPECT_EQ(sent.bypass, 0u);
}

TEST(UiState, SwitchThenBypassKeepsSelectedEngineDuringTelemetryLag) {
  // User is live on RNNoise (0), selects DFN3 (1), but telemetry hasn't caught up yet (still 0).
  // A bypass toggle in that window must carry engine 1 (the user's choice), not the stale 0.
  UiState ui;
  ui.onTelemetry(tel(/*engine*/0, /*bypass*/0, /*dspChain*/1, /*seq*/1), /*ready*/true);
  ui.onSelectEngine(1u);                                   // click DFN3 (SetParams engine=1 sent)
  ui.onTelemetry(tel(/*engine*/0, 0, 1, /*seq*/2), true);  // stale tick: worker not yet reporting 1
  vc::ipc::Params sent = ui.onToggleBypass();
  EXPECT_EQ(sent.engine, 1u);   // selected engine held, NOT reverted to stale 0
  EXPECT_EQ(sent.bypass, 1u);
}

TEST(UiState, EngineSyncResumesAfterTelemetryConfirmsSelection) {
  // Once telemetry confirms the selected engine, normal telemetry-authoritative sync resumes.
  UiState ui;
  ui.onTelemetry(tel(0, 0, 1, 1), true);
  ui.onSelectEngine(1u);
  ui.onTelemetry(tel(/*engine*/1, 0, 1, 2), true);   // confirmed -> latch clears
  // A later genuine change to engine 0 (e.g. reported by the worker) must now sync through again.
  ui.onTelemetry(tel(/*engine*/0, 0, 1, 3), true);
  vc::ipc::Params sent = ui.onToggleBypass();
  EXPECT_EQ(sent.engine, 0u);   // sync resumed after confirmation
}

using S = LivenessTracker::Status;

TEST(LivenessTracker, PresentThenReadyOnFirstSeqAdvance) {
  LivenessTracker lt;   // defaults: readyTimeout 5000, stallTimeout 3000
  EXPECT_EQ(lt.onSample(/*shmOpen*/false, /*seq*/0, /*now*/0),   S::Connecting);  // shm not up yet
  EXPECT_EQ(lt.onSample(/*shmOpen*/true,  /*seq*/5, /*now*/900), S::Connecting);  // baseline
  EXPECT_EQ(lt.onSample(/*shmOpen*/true,  /*seq*/6, /*now*/1900), S::Ready);      // first advance
}

TEST(LivenessTracker, LaunchFailureWhenNeverReady) {
  LivenessTracker lt;
  EXPECT_EQ(lt.onSample(true, 5, 0),    S::Connecting);   // baseline, no advance
  EXPECT_EQ(lt.onSample(true, 5, 4000), S::Connecting);   // still within readyTimeout
  EXPECT_EQ(lt.onSample(true, 5, 5001), S::Down);         // exceeded readyTimeout -> launch failure
}

TEST(LivenessTracker, DiesWhenSeqStallsAfterReady) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);          // baseline
  lt.onSample(true, 6, 1000);       // Ready (lastAdvance = 1000)
  EXPECT_EQ(lt.onSample(true, 6, 2000), S::Ready);   // stalled but within stallTimeout
  EXPECT_EQ(lt.onSample(true, 6, 4001), S::Down);    // > 3000 since last advance -> dead
}

TEST(LivenessTracker, DiesWhenShmLostAfterReady) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);
  lt.onSample(true, 6, 1000);       // Ready
  EXPECT_EQ(lt.onSample(/*shmOpen*/false, 0, 1100), S::Down);
}

TEST(LivenessTracker, StaysReadyWhileSeqKeepsAdvancing) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);
  lt.onSample(true, 6, 1000);
  EXPECT_EQ(lt.onSample(true, 7, 2000), S::Ready);
  EXPECT_EQ(lt.onSample(true, 8, 6000), S::Ready);   // advancing resets the stall clock
}

TEST(LivenessTracker, EverReadyFalseAfterLaunchFailure) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);
  lt.onSample(true, 5, 5001);   // Down via readyTimeout, never became ready
  EXPECT_EQ(lt.status(), S::Down);
  EXPECT_FALSE(lt.everReady());
}

TEST(LivenessTracker, EverReadyTrueAfterPostReadyDeath) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);
  lt.onSample(true, 6, 1000);   // Ready
  lt.onSample(false, 0, 1100);  // Down via shm loss
  EXPECT_EQ(lt.status(), S::Down);
  EXPECT_TRUE(lt.everReady());
}

TEST(LivenessTracker, DownIsTerminalIgnoresFurtherAdvances) {
  LivenessTracker lt;
  lt.onSample(true, 5, 0);
  lt.onSample(true, 5, 5001);   // Down
  EXPECT_EQ(lt.onSample(true, 6, 6000), S::Down);   // advancing seq after Down: stays Down
  EXPECT_EQ(lt.onSample(true, 7, 7000), S::Down);
}

TEST(SpawnArgs, ExactWorkerCommandLine) {
  std::wstring cmd = buildWorkerCommandLine(L"C:\\app", L"C:\\app\\settings.toml");
  EXPECT_EQ(cmd, L"\"C:\\app\\voicecleaner-worker.exe\" --config \"C:\\app\\settings.toml\"");
}

TEST(UiState, TuningSettersMutateCurrentAndReturnIt) {
  UiState ui;
  EXPECT_FLOAT_EQ(ui.onSetAggression(0.7f).aggression, 0.7f);
  EXPECT_FLOAT_EQ(ui.onSetHpf(120.0f).hpfCutoffHz, 120.0f);
  EXPECT_FLOAT_EQ(ui.onSetOutputGain(-18.0f).agcTargetDbfs, -18.0f);
  EXPECT_FLOAT_EQ(ui.onSetLimiter(-2.0f).limiterCeilingDbfs, -2.0f);
  EXPECT_FLOAT_EQ(ui.current().aggression, 0.7f);
  EXPECT_FLOAT_EQ(ui.current().agcTargetDbfs, -18.0f);
}
TEST(UiState, SeedTuningSetsAllFourAndSurvivesBypassToggle) {
  UiState ui;
  ui.seedTuning(/*aggr*/0.4f, /*hpf*/90.0f, /*gain*/-16.0f, /*lim*/-3.0f, /*declick*/0.0f, /*voiceFloor*/0.0f, /*gate*/0.0f);
  EXPECT_FLOAT_EQ(ui.current().aggression, 0.4f);
  EXPECT_FLOAT_EQ(ui.current().hpfCutoffHz, 90.0f);
  EXPECT_FLOAT_EQ(ui.current().agcTargetDbfs, -16.0f);
  EXPECT_FLOAT_EQ(ui.current().limiterCeilingDbfs, -3.0f);
  vc::ipc::Params sent = ui.onToggleBypass();
  EXPECT_FLOAT_EQ(sent.aggression, 0.4f);
  EXPECT_FLOAT_EQ(sent.agcTargetDbfs, -16.0f);
}
TEST(UiState, OnSetDeclickMutatesCurrent) {
  UiState ui;
  EXPECT_FLOAT_EQ(ui.onSetDeclick(0.6f).declickStrength, 0.6f);
  EXPECT_FLOAT_EQ(ui.current().declickStrength, 0.6f);
}
TEST(UiState, StatusLineShowsAgcReadoutWhenActive) {
  UiState ui;
  vc::ipc::Telemetry t{};
  t.activeEngine = 0; t.bypass = 0; t.dspChain = 1;
  t.vadActive = 1; t.agcGainDb = 6.0f;
  ui.onTelemetry(t, /*ready*/true);
  std::string s = ui.statusLine();
  EXPECT_NE(s.find("VAD"), std::string::npos);
  EXPECT_NE(s.find("AGC"), std::string::npos);
  EXPECT_NE(s.find("+6.0"), std::string::npos);   // gain value rendered
}
TEST(UiState, OnSetDeclickVoiceFloorMutatesCurrent) {
  UiState ui;
  EXPECT_FLOAT_EQ(ui.onSetDeclickVoiceFloor(0.3f).declickVoiceFloor, 0.3f);
  EXPECT_FLOAT_EQ(ui.current().declickVoiceFloor, 0.3f);
}
TEST(UiState, OnSetGateMutatesCurrent) {
  UiState ui;
  EXPECT_FLOAT_EQ(ui.onSetGate(0.5f).gateStrength, 0.5f);
  EXPECT_FLOAT_EQ(ui.current().gateStrength, 0.5f);
}
TEST(UiState, SeedTuningCarriesDeclick) {
  UiState ui;
  ui.seedTuning(/*aggr*/0.4f, /*hpf*/90.0f, /*gain*/-16.0f, /*lim*/-3.0f, /*declick*/0.7f, /*voiceFloor*/0.5f, /*gate*/0.3f);
  EXPECT_FLOAT_EQ(ui.current().declickStrength, 0.7f);
  EXPECT_FLOAT_EQ(ui.current().declickVoiceFloor, 0.5f);
  EXPECT_FLOAT_EQ(ui.current().gateStrength, 0.3f);
  EXPECT_FLOAT_EQ(ui.current().aggression, 0.4f);   // still carries the others
}
