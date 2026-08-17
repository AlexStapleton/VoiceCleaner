#include "ui_state.h"

namespace vc::ui {

UiState::UiState() = default;

void UiState::setRenderLabel(std::string label) { renderLabel_ = std::move(label); }

const char* UiState::engineName(uint32_t engine) {
  return engine == 1 ? "DeepFilterNet3" : "RNNoise";
}

void UiState::onNoConfig() { kind_ = StatusKind::NoConfig; }

void UiState::onWorkerDown() { kind_ = StatusKind::Down; }

void UiState::onTelemetry(const vc::ipc::Telemetry& t, bool ready) {
  activeEngine_ = t.activeEngine;
  bypass_       = (t.bypass != 0);
  dspChain_     = (t.dspChain != 0);
  // Keep the outgoing snapshot synced to live state (design §5): a later bypass-only toggle then
  // carries the live engine, and the embedded request_engine on the worker becomes a no-op.
  current_.engine = t.activeEngine;
  current_.bypass = t.bypass;
  if (ready) kind_ = bypass_ ? StatusKind::Bypassed : StatusKind::Active;
}

vc::ipc::Params UiState::onSelectEngine(uint32_t engine) {
  current_.engine = engine;
  return current_;
}

vc::ipc::Params UiState::onToggleBypass() {
  current_.bypass = current_.bypass ? 0u : 1u;
  return current_;
}

std::string UiState::bypassDisabledReason() const {
  return "no DSP chain \xE2\x80\x94 bypass unavailable";   // U+2014 em dash, UTF-8
}

std::string UiState::statusLine() const {
  switch (kind_) {
    case StatusKind::Active:
      return "\xE2\x97\x8F Active \xC2\xB7 " + std::string(engineName(activeEngine_)) +
             " \xC2\xB7 \xE2\x86\x92 " + renderLabel_;                       // dot Active mid engine arrow label
    case StatusKind::Bypassed:
      return "\xE2\x97\x8B Bypassed \xC2\xB7 " + std::string(engineName(activeEngine_)) +
             " \xC2\xB7 \xE2\x86\x92 " + renderLabel_;
    case StatusKind::NoConfig:
      return "No microphone configured \xE2\x80\x94 the mic picker arrives in Phase 4";
    case StatusKind::Down:
    default:
      return "\xE2\x9C\x95 Worker not running";
  }
}

}  // namespace vc::ui
