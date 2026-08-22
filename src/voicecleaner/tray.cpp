#include "tray.h"
#include "resource.h"   // IDI_ACTIVE / IDI_BYPASSED / IDI_DOWN
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

// Load a resource icon at the system small-icon size (the tray renders at ~16 px).
static HICON loadSmallIcon(HINSTANCE h, int id) {
  return (HICON)LoadImageW(h, MAKEINTRESOURCEW(id), IMAGE_ICON,
                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
}

Tray::Tray(HINSTANCE hInst, HWND hwnd) : hInst_(hInst), hwnd_(hwnd) {}
Tray::~Tray() {
  remove();
  if (icoActive_)   DestroyIcon(icoActive_);
  if (icoBypassed_) DestroyIcon(icoBypassed_);
  if (icoDown_)     DestroyIcon(icoDown_);
}

HICON Tray::iconFor(const UiState& s) const {
  if (s.statusKind() == StatusKind::Down) return icoDown_ ? icoDown_ : icoActive_;
  if (s.bypassed())                       return icoBypassed_ ? icoBypassed_ : icoActive_;
  return icoActive_;
}

bool Tray::add() {
  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid_.uCallbackMessage = WM_TRAY;
  icoActive_   = loadSmallIcon(hInst_, IDI_ACTIVE);
  icoBypassed_ = loadSmallIcon(hInst_, IDI_BYPASSED);
  icoDown_     = loadSmallIcon(hInst_, IDI_DOWN);
  nid_.hIcon = icoActive_ ? icoActive_ : LoadIcon(nullptr, IDI_APPLICATION);
  icoCur_ = nid_.hIcon;
  wcscpy_s(nid_.szTip, L"VoiceCleaner");
  added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
  return added_;
}

void Tray::update(const UiState& s) {
  if (!added_) return;
  std::wstring tip = L"VoiceCleaner \x2014 " + toW(s.statusLine());   // U+2014 em dash (wide literal)
  if (tip.size() > 127) tip.resize(127);
  wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
  HICON want = iconFor(s);
  const bool iconChanged = want && want != icoCur_;   // swap only on a real change (avoid NIM_MODIFY churn)
  nid_.uFlags = NIF_TIP | (iconChanged ? NIF_ICON : 0u);
  if (iconChanged) { nid_.hIcon = want; icoCur_ = want; }
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
