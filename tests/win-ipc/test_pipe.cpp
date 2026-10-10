#include <gtest/gtest.h>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <windows.h>
#include <atomic>
#include <thread>
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

// Regression (UI slider drag): back-to-back sends hit the window where the single-instance server is
// recycling its pipe. Before the client retry those sends failed (PIPE_BUSY / FILE_NOT_FOUND) and the
// value was silently dropped. Every send must now succeed and arrive, in order.
TEST(PipeClient, BackToBackSendsAreAllDelivered) {
  const std::wstring name = L"\\\\.\\pipe\\voicecleaner-test-burst";
  std::mutex m; std::condition_variable cv; std::vector<double> got;
  PipeServer srv;
  ASSERT_TRUE(srv.start(name, [&](const Command& c){
    { std::lock_guard<std::mutex> lk(m); got.push_back(c.driftSeedPpm); } cv.notify_all();
  }));

  Command c; c.type = CmdType::SelectCaptureDevice; c.deviceId = "x";
  c.driftSeedPpm = -1.0;   // warm-up message: wait for the server to come up
  bool up = false;
  for (int i = 0; i < 50 && !up; ++i) {
    up = PipeClient::send(name, c);
    if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(up);

  constexpr int kN = 200;
  for (int i = 0; i < kN; ++i) {
    c.driftSeedPpm = (double)i;
    ASSERT_TRUE(PipeClient::send(name, c)) << "send " << i << " was dropped";
  }

  std::unique_lock<std::mutex> lk(m);
  ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(5), [&]{ return got.size() == (size_t)kN + 1; }));
  for (int i = 0; i < kN; ++i) EXPECT_DOUBLE_EQ(got[(size_t)i + 1], (double)i);
  lk.unlock();
  srv.stop();
}
