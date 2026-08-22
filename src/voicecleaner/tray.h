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
  HICON iconFor(const UiState& s) const;   // Down -> down, bypassed -> gray, else active

  HINSTANCE hInst_;
  HWND hwnd_;
  NOTIFYICONDATAW nid_{};
  bool added_ = false;
  // State-variant tray icons (loaded once in add(), destroyed in the dtor). icoCur_ tracks the
  // icon currently shown so update() only issues NIM_MODIFY with NIF_ICON when it actually changes.
  HICON icoActive_ = nullptr, icoBypassed_ = nullptr, icoDown_ = nullptr, icoCur_ = nullptr;
};

}  // namespace vc::ui
