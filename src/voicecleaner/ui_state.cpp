#include "ui_state.h"
#include <cstdio>

namespace vc::ui {

UiState::UiState() = default;

void UiState::setRenderLabel(std::string label) { renderLabel_ = std::move(label); }

void UiState::onNoConfig() { kind_ = StatusKind::NoConfig; }

void UiState::onWorkerDown() { kind_ = StatusKind::Down; }

void UiState::onTelemetry(const vc::ipc::Telemetry& t, bool ready) {
  bypass_       = (t.bypass != 0);
  dspChain_     = (t.dspChain != 0);
  current_.bypass = t.bypass;
  telVadActive_ = (t.vadActive != 0);
  telAgcGainDb_ = t.agcGainDb;
  if (ready) kind_ = bypass_ ? StatusKind::Bypassed : StatusKind::Active;
}

vc::ipc::Params UiState::onToggleBypass() {
  current_.bypass = current_.bypass ? 0u : 1u;
  return current_;
}

void UiState::seedTuning(float hpfHz, float gainDbfs, float limiterDbfs, float declick, float declickVoiceFloor, float gate, float mix, float maxBoostDb) {
  current_.hpfCutoffHz        = hpfHz;
  current_.agcTargetDbfs      = gainDbfs;
  current_.limiterCeilingDbfs = limiterDbfs;
  current_.declickStrength    = declick;
  current_.declickVoiceFloor  = declickVoiceFloor;
  current_.gateStrength       = gate;
  current_.mixStrength        = mix;
  current_.agcMaxGainDb       = maxBoostDb;
}
vc::ipc::Params UiState::onSetHpf(float v)         { current_.hpfCutoffHz = v;        return current_; }
vc::ipc::Params UiState::onSetOutputGain(float v)  { current_.agcTargetDbfs = v;      return current_; }
vc::ipc::Params UiState::onSetLimiter(float v)     { current_.limiterCeilingDbfs = v; return current_; }
vc::ipc::Params UiState::onSetDeclick(float v)     { current_.declickStrength = v;    return current_; }
vc::ipc::Params UiState::onSetDeclickVoiceFloor(float v) { current_.declickVoiceFloor = v; return current_; }
vc::ipc::Params UiState::onSetGate(float v)        { current_.gateStrength = v;       return current_; }
vc::ipc::Params UiState::onSetMix(float v)         { current_.mixStrength = v;        return current_; }
vc::ipc::Params UiState::onSetMaxBoost(float v)    { current_.agcMaxGainDb = v;       return current_; }

std::string UiState::bypassDisabledReason() const {
  return "no DSP chain \xE2\x80\x94 bypass unavailable";   // U+2014 em dash, UTF-8
}

std::string UiState::statusLine() const {
  switch (kind_) {
    case StatusKind::Active: {
      // Append a live readout: VAD dot (● when the AGC gate is open) + current AGC gain in dB, so
      // the user can see auto-leveling engaging while they speak / move the TARGET LOUDNESS slider.
      char buf[64];
      std::snprintf(buf, sizeof(buf), " \xC2\xB7 VAD %s \xC2\xB7 AGC %+.1f dB",
                    telVadActive_ ? "\xE2\x97\x8F" : "\xE2\x97\x8B", (double)telAgcGainDb_);
      return "\xE2\x97\x8F Active \xC2\xB7 \xE2\x86\x92 " + renderLabel_ + buf;   // dot Active mid arrow label + readout
    }
    case StatusKind::Bypassed:
      return "\xE2\x97\x8B Bypassed \xC2\xB7 \xE2\x86\x92 " + renderLabel_;
    case StatusKind::NoConfig:
      return "Choose a microphone and an output below to start";
    case StatusKind::Down:
    default:
      return "\xE2\x9C\x95 Worker not running";
  }
}

LivenessTracker::LivenessTracker(uint64_t readyTimeoutMs, uint64_t stallTimeoutMs)
    : readyTimeoutMs_(readyTimeoutMs), stallTimeoutMs_(stallTimeoutMs) {}

LivenessTracker::Status LivenessTracker::onSample(bool shmOpen, uint32_t seq, uint64_t nowMs) {
  if (!started_) { started_ = true; startMs_ = nowMs; }
  if (status_ == Status::Down) return status_;   // terminal

  if (status_ == Status::Ready) {
    if (!shmOpen) { status_ = Status::Down; return status_; }        // shm lost
    if (seq != lastSeq_) { lastSeq_ = seq; lastAdvanceMs_ = nowMs; } // still advancing
    else if (nowMs - lastAdvanceMs_ > stallTimeoutMs_) status_ = Status::Down;  // stalled
    return status_;
  }

  // Connecting.
  if (shmOpen) {
    if (!haveBaseline_) { haveBaseline_ = true; lastSeq_ = seq; }
    else if (seq != lastSeq_) {
      lastSeq_ = seq; lastAdvanceMs_ = nowMs; everReady_ = true;
      status_ = Status::Ready; return status_;
    }
  }
  if (nowMs - startMs_ > readyTimeoutMs_) status_ = Status::Down;    // never became ready
  return status_;
}

std::wstring buildWorkerCommandLine(const std::wstring& moduleDir, const std::wstring& configPath) {
  return L"\"" + moduleDir + L"\\voicecleaner-worker.exe\" --config \"" + configPath + L"\"";
}

}  // namespace vc::ui
