#include "pipe.h"
#include <windows.h>
#include <chrono>
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
    if (pipe == INVALID_HANDLE_VALUE) {
      // Transient failure: back off, but stay responsive to stop(). Don't die permanently.
      if (WaitForSingleObject(static_cast<HANDLE>(stopEvent_), 100) == WAIT_OBJECT_0) break;
      continue;
    }
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // --- Connect (overlapped, cancelable via stopEvent_) ---
    bool connected = false;
    BOOL ok = ConnectNamedPipe(pipe, &ov);
    DWORD err = GetLastError();
    if (ok || err == ERROR_PIPE_CONNECTED) {
      connected = true;
    } else if (err == ERROR_IO_PENDING) {
      HANDLE hs[2] = { ov.hEvent, static_cast<HANDLE>(stopEvent_) };
      DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
      if (w == WAIT_OBJECT_0) { DWORD d; connected = GetOverlappedResult(pipe, &ov, &d, FALSE) != 0; }
      else { CancelIoEx(pipe, &ov); CloseHandle(ov.hEvent); CloseHandle(pipe); break; }
    }
    if (!connected) {
      CloseHandle(ov.hEvent); CloseHandle(pipe);
      if (!running_.load()) break; else continue;
    }

    // --- Read messages (overlapped, cancelable) until disconnect or stop ---
    std::vector<uint8_t> buf(64 * 1024);
    bool stopping = false;
    for (;;) {
      ResetEvent(ov.hEvent);
      DWORD n = 0;
      BOOL r = ReadFile(pipe, buf.data(), (DWORD)buf.size(), &n, &ov);
      if (!r) {
        DWORD re = GetLastError();
        if (re == ERROR_IO_PENDING) {
          HANDLE hs[2] = { ov.hEvent, static_cast<HANDLE>(stopEvent_) };
          DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
          if (w != WAIT_OBJECT_0) { CancelIoEx(pipe, &ov); stopping = true; break; }
          if (!GetOverlappedResult(pipe, &ov, &n, FALSE)) break;  // client disconnect / error
        } else {
          break;  // ERROR_BROKEN_PIPE (disconnect) or other; also ERROR_MORE_DATA (msg > 64KB, N/A for our tiny commands)
        }
      }
      if (n == 0) break;
      Command c;
      if (decode(buf.data(), n, c) && handler_) handler_(c);
      if (!running_.load()) { stopping = true; break; }
    }
    CancelIoEx(pipe, &ov);   // ensure no IO outstanding before close (harmless if none)
    DisconnectNamedPipe(pipe);
    CloseHandle(ov.hEvent);
    CloseHandle(pipe);
    if (stopping) break;
  }
}

void PipeServer::stop() {
  if (!running_.exchange(false)) { if (th_.joinable()) th_.join(); return; }
  if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
  if (th_.joinable()) th_.join();
  if (stopEvent_) { CloseHandle(static_cast<HANDLE>(stopEvent_)); stopEvent_ = nullptr; }
}

bool PipeClient::send(const std::wstring& name, const Command& c) {
  // The server is single-instance and recycles the pipe after every client (disconnect, close,
  // CreateNamedPipeW again), and it runs the command handler before recycling. A send landing in that
  // window sees ERROR_PIPE_BUSY (instance still serving the previous client) or ERROR_FILE_NOT_FOUND
  // (old instance closed, new one not created yet). Retry briefly instead of dropping the command.
  // Budgets stay small because this runs on the UI thread: FILE_NOT_FOUND before the server has been
  // seen at all usually means no worker, so it gives up after kNotFoundMs; once BUSY proves a live
  // server, retries continue up to kBusyMs. Callers that must not lose a value retry later (the UI's
  // ParamOutbox), so a timeout here is not a lost update.
  using clock = std::chrono::steady_clock;
  constexpr auto kNotFoundMs = std::chrono::milliseconds(20);
  constexpr auto kBusyMs     = std::chrono::milliseconds(100);
  const auto t0 = clock::now();
  bool serverSeen = false;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (;;) {
    pipe = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    const DWORD err = GetLastError();
    const auto elapsed = clock::now() - t0;
    if (err == ERROR_PIPE_BUSY) {
      serverSeen = true;
      if (elapsed >= kBusyMs) return false;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(kBusyMs - elapsed).count();
      WaitNamedPipeW(name.c_str(), (DWORD)(left > 0 ? left : 1));   // returns early if no instance exists
    } else if (err == ERROR_FILE_NOT_FOUND) {
      if (elapsed >= (serverSeen ? kBusyMs : kNotFoundMs)) return false;
      Sleep(1);
    } else {
      return false;
    }
  }
  auto bytes = encode(c);
  DWORD written = 0;
  BOOL ok = WriteFile(pipe, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
  CloseHandle(pipe);
  return ok && written == bytes.size();
}
}  // namespace vc::ipc
