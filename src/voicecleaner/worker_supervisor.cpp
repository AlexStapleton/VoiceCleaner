#include "worker_supervisor.h"
#include <windows.h>
#include "pipe.h"   // vc::ipc::PipeClient

namespace vc::ui {

WorkerSupervisor::WorkerSupervisor(std::wstring moduleDir, std::wstring configPath)
    : moduleDir_(std::move(moduleDir)), configPath_(std::move(configPath)) {}

WorkerSupervisor::~WorkerSupervisor() {
  if (process_) { CloseHandle(static_cast<HANDLE>(process_)); process_ = nullptr; }
}

bool WorkerSupervisor::openReader() {
  if (readerOpen_) return true;
  readerOpen_ = reader_.open(shmName_);
  return readerOpen_;
}

bool WorkerSupervisor::startOrAttach() {
  // Reset any prior (possibly dead) session so Relaunch re-spawns/re-attaches cleanly.
  reader_.close();
  readerOpen_ = false;
  down_ = false;
  liveness_ = LivenessTracker{};
  if (process_) { CloseHandle(static_cast<HANDLE>(process_)); process_ = nullptr; }

  if (openReader()) return true;   // a worker is already present -> attach (no process handle)

  // Spawn. CreateProcessW requires a mutable command-line buffer.
  std::wstring cmd = buildWorkerCommandLine(moduleDir_, configPath_);
  std::wstring mutableCmd = cmd;   // CreateProcessW may modify in place
  STARTUPINFOW si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, moduleDir_.c_str(), &si, &pi);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  return true;   // readiness confirmed later via poll() (first seq advance)
}

PollResult WorkerSupervisor::poll(uint64_t nowMs) {
  PollResult r;
  if (down_) { r.alive = false; return r; }

  // A spawned process that has exited is authoritative death.
  if (process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0) {
    down_ = true;
    r.alive = false;
    r.launchFailure = !liveness_.everReady();
    return r;
  }

  const bool shm = openReader();
  uint32_t seq = 0;
  if (shm) { r.telemetry = reader_.read(); seq = r.telemetry.seq; }

  auto st = liveness_.onSample(shm, seq, nowMs);
  r.ready = (st == LivenessTracker::Status::Ready);
  if (st == LivenessTracker::Status::Down) {
    down_ = true;
    r.alive = false;
    r.launchFailure = !liveness_.everReady();
  } else {
    r.alive = true;
  }
  return r;
}

bool WorkerSupervisor::send(const vc::ipc::Command& c) {
  return vc::ipc::PipeClient::send(pipeName_, c);   // failure only corroborates death; poll() decides
}

bool WorkerSupervisor::sendSelectCapture(const std::string& id, double driftSeedPpm) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SelectCaptureDevice;
  c.deviceId = id; c.driftSeedPpm = driftSeedPpm;
  return vc::ipc::PipeClient::send(pipeName_, c);
}

bool WorkerSupervisor::sendSelectRender(const std::string& id) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SelectRenderDevice;
  c.deviceId = id;
  return vc::ipc::PipeClient::send(pipeName_, c);
}

void WorkerSupervisor::shutdown(uint64_t waitMs) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::Shutdown;
  vc::ipc::PipeClient::send(pipeName_, c);
  if (process_) {
    WaitForSingleObject(static_cast<HANDLE>(process_), (DWORD)waitMs);
  } else {
    // Attached: wait for the shm mapping to disappear (best-effort bounded wait).
    reader_.close();        // release our handle so the worker's section can vanish
    readerOpen_ = false;
    const uint64_t step = 100;
    for (uint64_t waited = 0; waited < waitMs; waited += step) {
      vc::ipc::TelemetryReader probe;
      if (!probe.open(shmName_)) break;
      Sleep((DWORD)step);
    }
  }
}

}  // namespace vc::ui
