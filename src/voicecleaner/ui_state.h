#pragma once
#include <cstdint>
#include <string>
#include "params.h"      // vc::ipc::Params
#include "telemetry.h"   // vc::ipc::Telemetry

namespace vc::ui {

// What the status line is showing. All ASCII so it is trivially testable; the painter maps
// the kind to a colored dot glyph and statusLine() builds the full display string.
enum class StatusKind : uint32_t { Active, Bypassed, Down, NoConfig };

// Pure UI state model. Fed telemetry + user events; computes the display and the
// control-enablement state. Owns one Params `current_` seeded from Params::defaults() (which equals
// the worker's runtime values), and keeps current_.bypass synced to telemetry.
class UiState {
 public:
  UiState();

  void setRenderLabel(std::string label);   // chosen output device name, e.g. "CABLE Input (VB-Audio Virtual Cable)"

  // ---- events ----
  void onNoConfig();                                        // pre-flight failed: no config / no devices
  void onTelemetry(const vc::ipc::Telemetry& t, bool ready);  // ready == LivenessTracker reports Ready
  void onWorkerDown();                                      // supervisor declared death / launch failure

  // ---- user actions: mutate current_, return the Params the caller sends via SetParams ----
  vc::ipc::Params onToggleBypass();                   // flips current_.bypass
  void seedTuning(float hpfHz, float gainDbfs, float limiterDbfs, float declick, float declickVoiceFloor, float gate, float mix, float maxBoostDb = 18.0f);  // attach seed
  vc::ipc::Params onSetHpf(float v);
  vc::ipc::Params onSetOutputGain(float v);
  vc::ipc::Params onSetLimiter(float v);
  vc::ipc::Params onSetDeclick(float v);
  vc::ipc::Params onSetDeclickVoiceFloor(float v);
  vc::ipc::Params onSetGate(float v);
  vc::ipc::Params onSetMix(float v);
  vc::ipc::Params onSetMaxBoost(float v);   // AGC max boost (dB)

  // ---- queries (paint) ----
  StatusKind statusKind() const { return kind_; }
  std::string statusLine() const;
  bool bypassed() const { return bypass_; }
  bool bypassEnabled() const { return dspChain_; }
  std::string bypassDisabledReason() const;
  const vc::ipc::Params& current() const { return current_; }

 private:
  vc::ipc::Params current_ = vc::ipc::Params::defaults();
  StatusKind kind_ = StatusKind::Down;   // pre-ready shows Down until the first ready telemetry
  std::string renderLabel_;
  bool bypass_ = false;
  bool dspChain_ = true;
  bool  telVadActive_ = false;   // last telemetry VAD state (readout)
  float telAgcGainDb_ = 0.0f;    // last telemetry AGC gain in dB (readout)
};

// Pure liveness/readiness policy over (shmOpen, seq, nowMs) samples. Terminal at Down.
class LivenessTracker {
 public:
  enum class Status : uint32_t { Connecting, Ready, Down };
  explicit LivenessTracker(uint64_t readyTimeoutMs = 5000, uint64_t stallTimeoutMs = 3000);
  Status onSample(bool shmOpen, uint32_t seq, uint64_t nowMs);
  Status status() const { return status_; }
  bool everReady() const { return everReady_; }   // distinguishes launch failure from later death

 private:
  uint64_t readyTimeoutMs_;
  uint64_t stallTimeoutMs_;
  Status status_ = Status::Connecting;
  bool started_ = false;        // startMs_ initialized
  bool haveBaseline_ = false;   // baseline seq captured
  bool everReady_ = false;
  uint64_t startMs_ = 0;
  uint64_t lastAdvanceMs_ = 0;
  uint32_t lastSeq_ = 0;
};

// Exact worker command line: "<dir>\voicecleaner-worker.exe" --config "<configPath>".
// No forced flags — the worker honors settings.toml (dsp_chain / ring_frames).
std::wstring buildWorkerCommandLine(const std::wstring& moduleDir, const std::wstring& configPath);

}  // namespace vc::ui
