#include "tray.h"
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace vc::ui {

static std::wstring toW(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

Tray::Tray(HINSTANCE hInst, HWND hwnd) : hInst_(hInst), hwnd_(hwnd) {}
Tray::~Tray() { remove(); }

bool Tray::add() {
  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid_.uCallbackMessage = WM_TRAY;
  nid_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wcscpy_s(nid_.szTip, L"VoiceCleaner");
  added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
  return added_;
}

void Tray::update(const UiState& s) {
  if (!added_) return;
  std::wstring tip = L"VoiceCleaner \xE2\x80\x94 " + toW(s.statusLine());
  if (tip.size() > 127) tip.resize(127);
  wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
  nid_.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::remove() {
  if (added_) { Shell_NotifyIconW(NIM_DELETE, &nid_); added_ = false; }
}

void Tray::showMenu(const UiState& s) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | (s.bypassed() ? 0 : MF_CHECKED) |
                 (s.bypassEnabled() ? 0 : MF_GRAYED), kTrayCmdEnabled, L"Enabled");
  HMENU eng = CreatePopupMenu();
  AppendMenuW(eng, MF_STRING | (s.activeEngine()==0 ? MF_CHECKED : 0), kTrayCmdEngineRnn, L"RNNoise");
  AppendMenuW(eng, MF_STRING | (s.activeEngine()==1 ? MF_CHECKED : 0), kTrayCmdEngineDfn, L"DeepFilterNet3");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)eng, L"Engine");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kTrayCmdOpen, L"Open VoiceCleaner");
  AppendMenuW(m, MF_STRING, kTrayCmdQuit, L"Quit");

  POINT pt; GetCursorPos(&pt);
  SetForegroundWindow(hwnd_);   // required so the menu dismisses on click-away
  TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
  DestroyMenu(m);
}

}  // namespace vc::ui
