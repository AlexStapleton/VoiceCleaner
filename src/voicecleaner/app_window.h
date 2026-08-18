#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include "ui_state.h"
#include "worker_supervisor.h"
#include "device_picker.h"
#include "settings.h"

namespace vc::ui {

class Tray;   // fwd

// The single Direct2D window. Owns the UiState; drives the supervisor from a 100 ms timer.
class AppWindow {
 public:
  AppWindow(HINSTANCE hInst, WorkerSupervisor* sup, UiState* state);
  ~AppWindow();

  bool create(const wchar_t* title);
  HWND hwnd() const { return hwnd_; }
  void setTray(Tray* t) { tray_ = t; }
  void setSettings(vc::Settings* s, std::string configPath) { settings_ = s; configPath_ = std::move(configPath); }

  void showRestore();          // bring back from tray (single-instance restore + tray click)
  void onCommandBypass();      // shared path for window + tray "Enabled" toggle
  void onCommandEngine(uint32_t engine);
  void requestRelaunch();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT msg, WPARAM w, LPARAM l);
  void ensureRenderTarget();
  void discardRenderTarget();
  void paint();
  void tick();                 // WM_TIMER: poll supervisor -> UiState -> InvalidateRect
  void layout(D2D1_SIZE_F sz); // recompute control rects
  void sendCurrent(const vc::ipc::Params& p);
  int  hitControl(int x, int y) const;   // 0=RNNoise 1=DFN3 2=Bypass 3=Relaunch, -1=none
  void refreshDevices();                 // enumerate WASAPI devices + seed picker active from settings

  HINSTANCE hInst_;
  HWND hwnd_ = nullptr;
  WorkerSupervisor* sup_;
  UiState* state_;
  Tray* tray_ = nullptr;
  vc::Settings* settings_ = nullptr;
  std::string configPath_;
  DevicePicker picker_;

  ID2D1Factory* d2dFactory_ = nullptr;
  IDWriteFactory* dwFactory_ = nullptr;
  IDWriteTextFormat* textFmt_ = nullptr;    // status line: left-aligned, wrapped
  IDWriteTextFormat* centerFmt_ = nullptr;  // control labels: centered both axes
  IDWriteTextFormat* capFmt_ = nullptr;     // section captions: small, centered
  ID2D1HwndRenderTarget* rt_ = nullptr;

  int hot_ = -1;   // control under the cursor (see hitControl); drives hover highlight

  // control rects (client coords), recomputed in layout()
  D2D1_RECT_F rcEngineRnn_{}, rcEngineDfn_{}, rcBypass_{}, rcRelaunch_{};
};

}  // namespace vc::ui
