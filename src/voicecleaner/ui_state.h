#pragma once
#include <cstdint>
#include <string>
#include "params.h"      // vc::ipc::Params
#include "telemetry.h"   // vc::ipc::Telemetry

namespace vc::ui {

// What the status line is showing. All ASCII so it is trivially testable; the painter maps
// the kind to a colored dot glyph and statusLine() builds the full display string.
enum class StatusKind : uint32_t { Active, Bypassed, Down, NoConfig };

// Pure UI state model (blueprint §5). Fed telemetry + user events; computes the display and the
// control-enablement state. Owns one Params `current_` seeded from Params::defaults() (which equals
// the worker's runtime values), and keeps current_.engine / current_.bypass synced to telemetry so
// a bypass-only toggle never carries a stale engine (design §5, gap-#1 guard).
class UiState {
 public:
  UiState();

  void setRenderLabel(std::string label);   // e.g. "CABLE Input"; used in the status line

  // ---- events ----
  void onNoConfig();                                        // pre-flight failed: no config / no devices
  void onTelemetry(const vc::ipc::Telemetry& t, bool ready);  // ready == LivenessTracker reports Ready
  void onWorkerDown();                                      // supervisor declared death / launch failure

  // ---- user actions: mutate current_, return the Params the caller sends via SetParams ----
  vc::ipc::Params onSelectEngine(uint32_t engine);   // 0 = RNNoise, 1 = DFN3
  vc::ipc::Params onToggleBypass();                   // flips current_.bypass; engine stays synced

  // ---- queries (paint) ----
  StatusKind statusKind() const { return kind_; }
  std::string statusLine() const;
  uint32_t activeEngine() const { return activeEngine_; }   // segmented-control highlight (authoritative)
  bool bypassed() const { return bypass_; }
  bool bypassEnabled() const { return dspChain_; }
  std::string bypassDisabledReason() const;
  const vc::ipc::Params& current() const { return current_; }

 private:
  static const char* engineName(uint32_t engine);   // "RNNoise" / "DeepFilterNet3"

  vc::ipc::Params current_ = vc::ipc::Params::defaults();
  StatusKind kind_ = StatusKind::Down;   // pre-ready shows Down until the first ready telemetry
  std::string renderLabel_ = "CABLE Input";
  uint32_t activeEngine_ = 0;
  bool bypass_ = false;
  bool dspChain_ = true;
};

}  // namespace vc::ui
