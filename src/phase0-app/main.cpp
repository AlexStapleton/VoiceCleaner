#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
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

    auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (seconds > 0) {
        auto el = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        if (el >= seconds) break;
      }
    }
    std::printf("stopping...\n");
    bridge.stop();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "fatal: %s\n", ex.what());
    return 1;
  }
}
