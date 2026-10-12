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

// Settings live next to the exe (exeDir\settings.toml), so the app is portable: the whole install is
// one folder. Updates don't touch the file because the release zip doesn't contain one.
// v1.5 kept settings in %LOCALAPPDATA%\VoiceCleaner instead; if that file exists it is the newest
// copy (v1.5 copied the exe-folder file there and saved only there), so move it back once and remove
// the then-empty folder. If the move fails (e.g. a read-only install folder), keep using it there so
// nothing is lost.
static std::wstring settingsPath(const std::wstring& exeDir) {
  const std::wstring path = exeDir + L"\\settings.toml";
  PWSTR base = nullptr;
  std::wstring v15Dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) v15Dir = std::wstring(base) + L"\\VoiceCleaner";
  CoTaskMemFree(base);
  if (v15Dir.empty()) return path;
  const std::wstring v15File = v15Dir + L"\\settings.toml";
  if (!fileExists(v15File)) return path;
  if (!MoveFileExW(v15File.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return v15File;
  RemoveDirectoryW(v15Dir.c_str());   // only succeeds if empty; harmless otherwise
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
