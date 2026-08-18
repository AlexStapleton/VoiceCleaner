#include <windows.h>
#include <string>
#include "settings.h"        // vc::Settings
#include "app_window.h"
#include "tray.h"
#include "worker_supervisor.h"
#include "ui_state.h"

using namespace vc::ui;

static const wchar_t* kMutex = L"Local\\voicecleaner-ui";
static UINT g_restoreMsg = 0;

static std::wstring moduleDir() {
  wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf);
  size_t slash = p.find_last_of(L'\\');
  return slash == std::wstring::npos ? p : p.substr(0, slash);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  // Same value across processes for the same name — the single-instance broadcast below is caught
  // by the already-running instance's AppWindow::handle (design note, Task 14).
  g_restoreMsg = RegisterWindowMessageW(L"VoiceCleanerRestore");

  // Single instance: if one already runs, broadcast restore and exit.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutex);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    PostMessageW(HWND_BROADCAST, g_restoreMsg, 0, 0);
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const std::wstring dir = moduleDir();
  const std::wstring cfgPath = dir + L"\\settings.toml";

  std::string cfgUtf8;
  cfgUtf8.reserve(cfgPath.size());
  for (wchar_t c : cfgPath) cfgUtf8.push_back(static_cast<char>(c));   // explicit narrow (ASCII path)

  UiState state; state.setRenderLabel("CABLE Input");
  WorkerSupervisor sup(dir, cfgPath);
  AppWindow win(hInst, &sup, &state);

  vc::Settings settings = vc::Settings::load(cfgUtf8);   // tolerant: missing/bad => empty
  win.setSettings(&settings, cfgUtf8);                    // seed the picker before create()

  if (!win.create(L"VoiceCleaner")) { CoUninitialize(); return 1; }

  Tray tray(hInst, win.hwnd());
  tray.add();
  win.setTray(&tray);

  // First-run: spawn only when both devices are already chosen; else show the pickers (NoConfig).
  if (!settings.activeCaptureId.empty() && !settings.renderId.empty()) sup.startOrAttach();
  else                                                                 state.onNoConfig();

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

  tray.remove();
  CoUninitialize();
  if (mutex) CloseHandle(mutex);
  return 0;
}
