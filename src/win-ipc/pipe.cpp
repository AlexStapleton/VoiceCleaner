#include "pipe.h"
#include <windows.h>
#include <vector>
namespace vc::ipc {

PipeServer::~PipeServer() { stop(); }

bool PipeServer::start(const std::wstring& name, Handler onCommand) {
  name_ = name; handler_ = std::move(onCommand);
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
  if (!stopEvent_) return false;
  running_.store(true);
  th_ = std::thread([this]{ run(); });
  return true;
}

void PipeServer::run() {
  while (running_.load()) {
    HANDLE pipe = CreateNamedPipeW(
        name_.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 0, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;

    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = ConnectNamedPipe(pipe, &ov);
    DWORD err = GetLastError();
    if (!ok && err == ERROR_IO_PENDING) {
      HANDLE hs[2] = { ov.hEvent, static_cast<HANDLE>(stopEvent_) };
      DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
      if (w != WAIT_OBJECT_0) { CloseHandle(ov.hEvent); CloseHandle(pipe); break; }
    } else if (!ok && err != ERROR_PIPE_CONNECTED) {
      CloseHandle(ov.hEvent); CloseHandle(pipe);
      if (!running_.load()) break; else continue;
    }
    CloseHandle(ov.hEvent);

    // Read messages until the client disconnects.
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
      DWORD n = 0;
      BOOL r = ReadFile(pipe, buf.data(), (DWORD)buf.size(), &n, nullptr);
      if (!r || n == 0) break;
      Command c;
      if (decode(buf.data(), n, c) && handler_) handler_(c);
      if (!running_.load()) break;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void PipeServer::stop() {
  if (!running_.exchange(false)) { if (th_.joinable()) th_.join(); return; }
  if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
  if (th_.joinable()) th_.join();
  if (stopEvent_) { CloseHandle(static_cast<HANDLE>(stopEvent_)); stopEvent_ = nullptr; }
}

bool PipeClient::send(const std::wstring& name, const Command& c) {
  HANDLE pipe = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  auto bytes = encode(c);
  DWORD written = 0;
  BOOL ok = WriteFile(pipe, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
  CloseHandle(pipe);
  return ok && written == bytes.size();
}
}  // namespace vc::ipc
