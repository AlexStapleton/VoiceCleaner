#pragma once
#include <string>
#include "telemetry.h"       // vc::ipc::Telemetry
#include "telemetry_shm.h"   // vc::ipc::TelemetryReader
#include "command.h"         // vc::ipc::Command
#include "ui_state.h"        // vc::ui::LivenessTracker

namespace vc::ui {

// Outcome of one poll tick, consumed by the window to drive UiState + repaint.
struct PollResult {
  bool alive = false;              // false -> UiState::onWorkerDown()
  bool ready = false;              // true once the worker is running (first seq advance)
  bool launchFailure = false;      // died before ever becoming ready (bad device / not 48 kHz / no model)
  vc::ipc::Telemetry telemetry{};  // last sample (valid when a reader is open)
};

// Owns the worker lifecycle: spawn or attach, liveness supervision, command send, shutdown.
class WorkerSupervisor {
 public:
  WorkerSupervisor(std::wstring moduleDir, std::wstring configPath);
  ~WorkerSupervisor();

  // Probe for a live worker (TelemetryReader::open). If present -> attach; else CreateProcess.
  // Returns false only if spawn itself failed (CreateProcess error); attach/spawn-then-wait
  // readiness is reported via poll() over subsequent ticks.
  bool startOrAttach();

  PollResult poll(uint64_t nowMs);           // call from WM_TIMER (~10 Hz)
  bool send(const vc::ipc::Command& c);       // PipeClient::send to the worker pipe
  void shutdown(uint64_t waitMs);             // send Shutdown, bounded-wait for exit
  bool workerDown() const { return down_; }

 private:
  bool openReader();                          // (re)open the telemetry mapping

  std::wstring moduleDir_;
  std::wstring configPath_;
  const std::wstring pipeName_ = L"\\\\.\\pipe\\voicecleaner";
  const std::wstring shmName_  = L"Local\\voicecleaner-telemetry";

  vc::ipc::TelemetryReader reader_;
  bool readerOpen_ = false;
  void* process_ = nullptr;                   // HANDLE (spawned only; null when attached)
  LivenessTracker liveness_;
  bool down_ = false;
};

}  // namespace vc::ui
