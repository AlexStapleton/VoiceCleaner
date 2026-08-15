#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <windows.h>
#include "com_guard.h"
#include "device_enumerator.h"
#include "config.h"
#include "bridge.h"

static std::atomic<bool> g_stop{false};
static BOOL WINAPI ctrlHandler(DWORD) { g_stop.store(true); return TRUE; }

static const char* kindStr(vc::SampleKind k) {
  switch (k) { case vc::SampleKind::Float32: return "float32";
               case vc::SampleKind::Pcm16: return "pcm16"; default: return "unsupported"; }
}
static void printList(const char* label, EDataFlow flow, vc::DeviceEnumerator& e) {
  std::printf("%s:\n", label);
  for (const auto& d : e.list(flow))
    std::printf("  %ls\t%ls\t%uHz\t%uch\t%s\n", d.name.c_str(), d.id.c_str(), d.sampleRate, d.channels, kindStr(d.kind));
}

int main(int argc, char** argv) {
  bool listDevices = false; std::string configPath; int seconds = 0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--list-devices")) listDevices = true;
    else if (!std::strcmp(argv[i], "--config") && i + 1 < argc) configPath = argv[++i];
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
  }

  try {
    vc::ComGuard com;
    if (listDevices) {
      vc::DeviceEnumerator e;
      printList("RENDER", eRender, e);
      printList("CAPTURE", eCapture, e);
      return 0;
    }

    vc::Config cfg;
    if (!configPath.empty()) cfg = vc::Config::load(configPath);
    cfg.apply_cli(argc, argv);
    if (cfg.captureDeviceId.empty() || cfg.renderDeviceId.empty()) {
      std::fprintf(stderr, "error: need capture and render device IDs (via --config or --capture/--render). Use --list-devices.\n");
      return 1;
    }

    vc::Bridge bridge(cfg);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    if (!bridge.start()) return 1;

    // Interim warm-standby switch trigger (NOT the §8.5 command channel): a non-RT stdin
    // reader that maps 1->engine 0 (RNNoise), 2->engine 1 (DFN3). Only spawned when
    // warm_standby is set; normal single-engine runs never start this thread. Blocking on
    // std::cin, so on shutdown we set g_stop and detach (join would block until a keypress).
    std::thread switchThread;
    if (cfg.warmStandby) {
      switchThread = std::thread([&bridge]{
        std::string line;
        while (!g_stop.load() && std::getline(std::cin, line)) {
          if (line.find('1') != std::string::npos)      { bridge.request_engine(0); std::printf("switch -> RNNoise\n"); }
          else if (line.find('2') != std::string::npos) { bridge.request_engine(1); std::printf("switch -> DFN3\n"); }
        }
      });
    }

    int rc = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (bridge.device_lost()) {
        std::fprintf(stderr, "fatal: audio device lost/invalidated — stopping the run visibly. "
                             "(Automatic reconnect is Phase 1; re-run once the device is back.)\n");
        rc = 2;
        break;
      }
      if (seconds > 0) {
        auto el = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        if (el >= seconds) break;
      }
    }
    std::printf("stopping...\n");
    bridge.stop();
    // The stdin reader blocks in std::getline; detach so shutdown never waits on a keypress.
    if (switchThread.joinable()) switchThread.detach();
    return rc;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "fatal: %s\n", ex.what());
    return 1;
  }
}
