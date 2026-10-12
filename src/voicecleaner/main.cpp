#include <windows.h>
#include <shlobj.h>          // SHGetKnownFolderPath
#include <string>
#include "settings.h"        // vc::Settings
#include "utf8.h"            // vc::wideToUtf8
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

static bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Per-user settings file: %LOCALAPPDATA%\VoiceCleaner\settings.toml. Kept out of the install folder so
// unzipping an update over it can never overwrite the user's devices and mic profiles (and the app
// still works from a read-only folder). Local, not roaming: the device IDs are specific to this PC.
// Versions before 1.5 kept the file next to the exe; it is copied over once, on first run. Falls back
// to the exe folder if the per-user folder can't be resolved or created.
static std::wstring settingsPath(const std::wstring& exeDir) {
  const std::wstring legacy = exeDir + L"\\settings.toml";
  PWSTR base = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = std::wstring(base) + L"\\VoiceCleaner";
  CoTaskMemFree(base);
  if (dir.empty()) return legacy;
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return legacy;
  const std::wstring path = dir + L"\\settings.toml";
  if (!fileExists(path) && fileExists(legacy)) CopyFileW(legacy.c_str(), path.c_str(), /*failIfExists=*/TRUE);
  return path;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  // Same value across processes for the same name — the single-instance broadcast below is caught
  // by the already-running instance's AppWindow::handle.
  g_restoreMsg = RegisterWindowMessageW(kRestoreMsgName);

  // Single instance: if one already runs, broadcast restore and exit.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutex);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    PostMessageW(HWND_BROADCAST, g_restoreMsg, 0, 0);
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const std::wstring dir = moduleDir();
  const std::wstring cfgPath = settingsPath(dir);
  const std::string cfgUtf8 = vc::wideToUtf8(cfgPath);   // Settings/toml++ take UTF-8 paths (user names can be non-ASCII)

  UiState state;   // the output name in the status line is set from the device picker (AppWindow)
  WorkerSupervisor sup(dir, cfgPath);
  AppWindow win(hInst, &sup, &state);

  vc::Settings settings = vc::Settings::load(cfgUtf8);   // tolerant: missing/bad => empty
  win.setSettings(&settings, cfgUtf8);                    // seed the picker before create()

  if (!win.create(L"VoiceCleaner")) { CoUninitialize(); return 1; }

  Tray tray(hInst, win.hwnd());
  tray.add();
  win.setTray(&tray);

  // First-run: spawn only when both devices are already chosen; else show the pickers (NoConfig).
  if (!settings.activeCaptureId.empty() && !settings.renderId.empty()) { sup.startOrAttach(); win.markSpawned(); }
  else                                                                 state.onNoConfig();

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

  tray.remove();
  CoUninitialize();
  if (mutex) CloseHandle(mutex);
  return 0;
}
