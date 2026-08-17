#pragma once
#include <windows.h>
#include <shellapi.h>
#include "ui_state.h"

namespace vc::ui {

// Tray command IDs routed via WM_COMMAND.
enum : UINT {
  kTrayCmdEnabled = 100,   // toggle bypass
  kTrayCmdEngineRnn = 101,
  kTrayCmdEngineDfn = 102,
  kTrayCmdOpen = 103,
  kTrayCmdQuit = 104,
};
static const UINT WM_TRAY = WM_APP + 1;   // tray callback message

class Tray {
 public:
  Tray(HINSTANCE hInst, HWND hwnd);
  ~Tray();
  bool add();
  void update(const UiState& s);   // icon tooltip reflects active/bypassed + engine
  void remove();
  void showMenu(const UiState& s); // right-click context menu

 private:
  HINSTANCE hInst_;
  HWND hwnd_;
  NOTIFYICONDATAW nid_{};
  bool added_ = false;
};

}  // namespace vc::ui
