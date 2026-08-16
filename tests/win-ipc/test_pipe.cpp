#include <gtest/gtest.h>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <windows.h>
#include <atomic>
#include "pipe.h"
using namespace vc::ipc;

TEST(PipeServer, ReceivesEncodedCommandFromClient) {
  const std::wstring name = L"\\\\.\\pipe\\voicecleaner-test-1";
  std::mutex m; std::condition_variable cv; std::vector<Command> got;
  PipeServer srv;
  ASSERT_TRUE(srv.start(name, [&](const Command& c){
    { std::lock_guard<std::mutex> lk(m); got.push_back(c); } cv.notify_all();
  }));

  Command out; out.type = CmdType::SelectCaptureDevice;
  out.deviceId = "{dev-id}"; out.driftSeedPpm = -26.0;
  // The server may need a moment to reach ConnectNamedPipe; retry the client briefly.
  bool sent = false;
  for (int i = 0; i < 50 && !sent; ++i) {
    sent = PipeClient::send(name, out);
    if (!sent) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(sent);

  std::unique_lock<std::mutex> lk(m);
  ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(2), [&]{ return !got.empty(); }));
  EXPECT_EQ(got[0].type, CmdType::SelectCaptureDevice);
  EXPECT_EQ(got[0].deviceId, "{dev-id}");
  EXPECT_DOUBLE_EQ(got[0].driftSeedPpm, -26.0);
  srv.stop();
}

TEST(PipeServer, StopDoesNotHangWithIdleConnectedClient) {
  const std::wstring name = L"\\\\.\\pipe\\voicecleaner-test-2";
  PipeServer srv;
  ASSERT_TRUE(srv.start(name, [](const Command&){}));
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 50 && h == INVALID_HANDLE_VALUE; ++i) {
    h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_NE(h, INVALID_HANDLE_VALUE);          // client connected, then stays idle (no writes)
  std::atomic<bool> stopped{false};
  std::thread t([&]{ srv.stop(); stopped.store(true); });
  for (int i = 0; i < 300 && !stopped.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(stopped.load());                  // stop() returned promptly (no deadlock)
  CloseHandle(h);
  t.join();
}
