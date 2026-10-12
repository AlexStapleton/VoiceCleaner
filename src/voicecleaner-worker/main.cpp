#include <cstring>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include "com_guard.h"
#include "device_enumerator.h"
#include "utf8.h"
#include "config.h"
#include "bridge.h"
#include "wasapi_capture.h"
#include "channel.h"
#include "wav.h"
#include "pipe.h"

static std::atomic<bool> g_stop{false};
static BOOL WINAPI ctrlHandler(DWORD) { g_stop.store(true); return TRUE; }


// Record path: capture the configured capture device (K688) to a 48 kHz mono PCM16 WAV for N
// seconds with NO processing/render/engine/relay. Mirrors the bridge's device resolution +
// WasapiCapture usage. Returns process exit code (0 ok, non-zero device failure).
static int runRecord(const vc::Config& cfg, const std::string& recordPath, int seconds) {
  if (cfg.captureDeviceId.empty()) {
    std::fprintf(stderr, "error: --record needs a capture device ID (via --config or --capture). Use --list-devices.\n");
    return 1;
  }
  vc::DeviceEnumerator e;
  IMMDevice* capDev = e.resolve(vc::utf8ToWide(cfg.captureDeviceId));
  if (!capDev) {
    std::fprintf(stderr, "error: could not resolve capture device '%s' (check ID via --list-devices)\n",
                 cfg.captureDeviceId.c_str());
    return 1;
  }

  std::vector<float> samples;
  samples.reserve((size_t)48000 * (size_t)(seconds > 0 ? seconds + 1 : 16));  // avoid steady-state realloc
  std::vector<float> mono;
  std::atomic<bool> failed{false};

  vc::WasapiCapture capture;
  auto errCb = [&failed](const char* w){
    std::fprintf(stderr, "audio error: %s\n", w);
    failed.store(true, std::memory_order_release);
  };
  auto sampleCb = [&samples, &mono](const float* interleaved, size_t frames, int channels){
    mono.resize(frames);
    vc::extract_channel(interleaved, frames, channels, 0, mono.data());
    samples.insert(samples.end(), mono.begin(), mono.end());
  };

  if (!capture.start(capDev, sampleCb, errCb)) {
    std::fprintf(stderr, "error: capture failed to start on device '%s'\n", cfg.captureDeviceId.c_str());
    capDev->Release();
    return 1;
  }
  capDev->Release();   // WasapiCapture Activated its own client; device no longer needed

  SetConsoleCtrlHandler(ctrlHandler, TRUE);
  const int dur = seconds > 0 ? seconds : 15;
  std::printf("recording %ds from capture device to %s (Ctrl+C to stop early)...\n", dur, recordPath.c_str());
  auto t0 = std::chrono::steady_clock::now();
  while (!g_stop.load() && !failed.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto el = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    if (el >= dur) break;
  }
  capture.stop();

  if (failed.load(std::memory_order_acquire)) {
    std::fprintf(stderr, "error: recording aborted due to audio device error\n");
    return 2;
  }

  vc::WavData wav; wav.sampleRate = 48000; wav.channels = 1; wav.samples = std::move(samples);
  const size_t n = wav.samples.size();
  if (!vc::wav_write(recordPath, wav, /*asFloat=*/false)) {
    std::fprintf(stderr, "error: failed to write WAV to %s\n", recordPath.c_str());
    return 1;
  }
  std::printf("recorded %zu samples (%.2fs) at 48000 Hz mono -> %s\n",
              n, (double)n / 48000.0, recordPath.c_str());
  return 0;
}

// argv is in the ANSI code page, which can't represent every path (the install folder, or a user
// name in it, may be non-ASCII). Re-read the --config value from the UTF-16 command line and return
// it as UTF-8 ("" if absent).
static std::string configPathFromWideCommandLine() {
  int n = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
  if (!wargv) return {};
  std::string out;
  for (int i = 1; i + 1 < n; ++i) {
    if (std::wcscmp(wargv[i], L"--config") != 0) continue;
    out = vc::wideToUtf8(wargv[i + 1]);
    break;
  }
  LocalFree(wargv);
  return out;
}

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
  bool listDevices = false; std::string configPath; int seconds = 0; std::string recordPath; bool devKeys = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--list-devices")) listDevices = true;
    else if (!std::strcmp(argv[i], "--config") && i + 1 < argc) { ++i; configPath = configPathFromWideCommandLine(); }
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--record") && i + 1 < argc) recordPath = argv[++i];
    else if (!std::strcmp(argv[i], "--dev-keys")) devKeys = true;
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

    // Record mode: capture -> WAV only. No render/engine/relay; render device not required.
    if (!recordPath.empty()) return runRecord(cfg, recordPath, seconds);

    if (cfg.captureDeviceId.empty() || cfg.renderDeviceId.empty()) {
      std::fprintf(stderr, "error: need capture and render device IDs (via --config or --capture/--render). Use --list-devices.\n");
      return 1;
    }

    vc::Bridge bridge(cfg);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    if (!bridge.start()) return 1;

    // Command channel: a named-pipe server dispatching worker-control commands (params,
    // engine selection via SetParams.engine, shutdown). Must outlive the main loop; stopped
    // explicitly below (its destructor also calls stop(), which is safe/idempotent).
    vc::ipc::PipeServer pipe;
    pipe.start(L"\\\\.\\pipe\\voicecleaner", [&bridge](const vc::ipc::Command& c){
      switch (c.type) {
        case vc::ipc::CmdType::SetParams:
          bridge.params().publish(c.params);
          bridge.request_engine((int)c.params.engine);   // lean: install-on-demand + crossfade; warm: instant crossfade; passthrough: no-op
          break;
        case vc::ipc::CmdType::ResetCounters:
          bridge.reset_counters();   // zero dropout/frame-time/ring-high-water for a clean measurement window
          break;
        case vc::ipc::CmdType::Shutdown:
          g_stop.store(true);
          break;
        case vc::ipc::CmdType::SelectCaptureDevice:
          bridge.reselectCapture(c.deviceId, c.driftSeedPpm);
          break;
        case vc::ipc::CmdType::SelectRenderDevice:
          bridge.reselectRender(c.deviceId);
          break;
        default: break;
      }
    });

    // Interim warm-standby switch trigger (NOT the command channel): a non-RT stdin
    // reader that maps 1->engine 0 (RNNoise), 2->engine 1 (DFN3). Only spawned when
    // --dev-keys is passed; normal runs never start this thread. Blocking on std::cin, so on
    // shutdown we set g_stop and detach (join would block until a keypress).
    std::thread switchThread;
    if (devKeys) {
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
      bridge.reclaimRetiredEngines();   // free engines the RT relay retired after a lean switch
      if (bridge.device_lost()) {
        std::fprintf(stderr, "fatal: audio device lost/invalidated — exiting "
                             "(the VoiceCleaner app restarts the worker).\n");
        rc = 2;
        break;
      }
      if (seconds > 0) {
        auto el = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        if (el >= seconds) break;
      }
    }
    std::printf("stopping...\n");
    pipe.stop();
    bridge.stop();
    // The stdin reader blocks in std::getline; detach so shutdown never waits on a keypress.
    if (switchThread.joinable()) switchThread.detach();
    return rc;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "fatal: %s\n", ex.what());
    return 1;
  }
}
