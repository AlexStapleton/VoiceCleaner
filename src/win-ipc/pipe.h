#pragma once
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include "command.h"
namespace vc::ipc {
// Inbound message-mode named-pipe server. Runs a non-RT reader thread; decodes each message with the
// vc-ipc codec and invokes onCommand on that thread. One client at a time (the UI).
class PipeServer {
public:
  using Handler = std::function<void(const Command&)>;
  PipeServer() = default;
  ~PipeServer();
  bool start(const std::wstring& name, Handler onCommand);  // false if the pipe can't be created
  void stop();
private:
  void run();
  std::wstring name_;
  Handler handler_;
  std::thread th_;
  void* stopEvent_ = nullptr;   // HANDLE (manual-reset)
  std::atomic<bool> running_{false};
};
// Client: connect + send one encoded Command per call. Returns false if no server / write fails.
class PipeClient {
public:
  static bool send(const std::wstring& name, const Command& c);
};
}  // namespace vc::ipc
