#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <array>
#include <string>
#include "ui_state.h"
#include "worker_supervisor.h"
#include "device_picker.h"
#include "settings.h"
#include "slider.h"
#include "slider_table.h"
#include "respawn_policy.h"
#include "param_outbox.h"

namespace vc::ui {

class Tray;   // fwd

// Registered window message a second instance broadcasts so the running one restores its window.
inline constexpr const wchar_t* kRestoreMsgName = L"VoiceCleanerRestore";

// The single Direct2D window. Owns the UiState; drives the supervisor from a 100 ms timer.
class AppWindow {
 public:
  AppWindow(HINSTANCE hInst, WorkerSupervisor* sup, UiState* state);
  ~AppWindow();

  bool create(const wchar_t* title);
  HWND hwnd() const { return hwnd_; }
  void setTray(Tray* t) { tray_ = t; }
  void setSettings(vc::Settings* s, std::string configPath) { settings_ = s; configPath_ = std::move(configPath); }
  void markSpawned() { spawned_ = true; }   // main.cpp calls this when it spawns at startup

  void showRestore();          // bring back from tray (single-instance restore + tray click)
  void onCommandBypass();      // shared path for window + tray "Enabled" toggle
  void requestRelaunch();

 private:
  // Clickable buttons (hover highlight + click action).
  enum class Control { None = -1, Bypass, Relaunch, Quit, RingApply };
  // What the help strip explains. Sliders are 0..kSliderCount-1 (their SliderId).
  enum HelpTarget : int { kHelpNone = -1, kHelpMic = 100, kHelpOutput, kHelpLatency, kHelpBypass };

  // Solid-colour brushes, created with the render target and released with it.
  struct Brushes {
    ID2D1SolidColorBrush *text = nullptr, *dim = nullptr, *section = nullptr, *accent = nullptr,
                         *accentHot = nullptr, *panel = nullptr, *panelHot = nullptr,
                         *disabled = nullptr, *amber = nullptr;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT msg, WPARAM w, LPARAM l);
  void onMouseDown(int x, int y);
  void onMouseMove(int x, int y);

  // ---- drawing ----
  void ensureRenderTarget();
  void discardRenderTarget();
  void paint();
  void layout(D2D1_SIZE_F sz);             // recompute control rects + contentH_
  void box(D2D1_RECT_F r, ID2D1Brush* b);
  void label(const std::wstring& s, D2D1_RECT_F r, ID2D1Brush* b, IDWriteTextFormat* f);
  void drawStatus(float width);
  void drawPickerField(const wchar_t* caption, D2D1_RECT_F r, PickerKind k);
  void drawBypass(float width);
  void drawHelpOrRelaunch();
  void drawSliderGroups(float width);
  void drawSlider(SliderId id);
  void drawLatency(float width);
  void drawFooter();
  void drawPickerOverlay();

  // ---- hit testing ----
  Control hitControl(int x, int y) const;
  int  trackHit(int x, int y) const;       // slider whose track band contains (x,y), or -1
  int  helpTargetAt(int x, int y) const;   // HelpTarget / SliderId under (x,y)
  static std::wstring helpTextFor(int target);   // plain-English "what + how" for a HelpTarget / SliderId
  float measureHelpHeight() const;         // help-strip height that fits the longest help text
  D2D1_RECT_F overlayRect() const;         // the open dropdown's bounding rect
  int  overlayHit(int x, int y) const;     // row index under (x,y) in the open overlay, or -1

  // ---- actions ----
  void tick();                             // WM_TIMER: poll supervisor -> UiState -> InvalidateRect
  void invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }
  void sendCurrent(const vc::ipc::Params& p, bool throttled = false);   // stage p + try to deliver
  void flushParams(bool force);            // deliver outbox_.pending() if due; keeps it pending on failure
  void refreshDevices();                   // enumerate WASAPI devices + seed picker active from settings
  void applyPick(PickerKind k, size_t rowIndex);   // persist + spawn/switch
  void setSliderFromX(SliderId id, int x); // set a slider's value from click/drag x
  vc::ipc::Params applySlider(SliderId id);  // push the slider's value into UiState -> Params
  void persistTuning();                    // write the active profile on drag-end
  void seedFromActiveProfile();            // load the active mic's tuning into sliders + UiState
  void startWorker();                      // spawn/attach; the active tuning is re-pushed once ready
  void respawnWorker();                    // full stop + fresh spawn so ring_frames is re-read
  void quit();                             // stop the worker and close the app
  void toggleSection(int i);               // collapse/expand slider group i (persist + resize)
  void resizeToContent();                  // resize the window to the current laid-out height
  void applyRing();                        // commit+clamp the latency box, persist, respawn
  void commitRingEdit();                   // normalize ringEdit_ to the clamped pending value
  int  ringParse() const;                  // ringEdit_ digits -> int (0 if empty/non-digit)
  int  ringPendingClamped() const;         // ringParse() clamped to [vc::kRingFramesMin, vc::kRingFramesMax]

  Slider& slider(SliderId id) { return sliders_[(size_t)id]; }
  const Slider& slider(SliderId id) const { return sliders_[(size_t)id]; }
  D2D1_RECT_F& track(SliderId id) { return tracks_[(size_t)id]; }
  const D2D1_RECT_F& track(SliderId id) const { return tracks_[(size_t)id]; }

  HINSTANCE hInst_;
  HWND hwnd_ = nullptr;
  WorkerSupervisor* sup_;
  UiState* state_;
  Tray* tray_ = nullptr;
  vc::Settings* settings_ = nullptr;
  std::string configPath_;
  DevicePicker picker_;
  bool spawned_ = false;   // worker spawned/attached (startup or via a device pick)
  bool pushedTuning_ = false;   // seeded profile tuning sent to the worker once it became ready
  RespawnPolicy respawn_;  // auto-respawn-with-backoff on post-ready worker death (device loss)
  ParamOutbox outbox_;     // newest Params not yet delivered to the worker (throttle + retry, see param_outbox.h)

  // Gate the WM_TIMER repaint. The worker publishes telemetry (meter + all aggregates) only
  // ~1 Hz, so a fixed 10 Hz repaint drew the same pixels 9 times out of 10. Repaint from a tick
  // only when a paint-relevant input changed: telemetry seq advanced, the status kind changed, or
  // the respawn attempt count changed. Interaction (hover/drag/clicks/pickers) still invalidates on
  // its own events, so the window stays responsive while idle ticks cost nothing.
  uint32_t   lastPaintSeq_ = 0xFFFFFFFFu;         // sentinel -> forces a paint on the first tick
  StatusKind lastPaintKind_ = StatusKind::Down;
  int        lastPaintAttempts_ = -1;

  ID2D1Factory* d2dFactory_ = nullptr;
  IDWriteFactory* dwFactory_ = nullptr;
  IDWriteTextFormat* textFmt_ = nullptr;    // status line: left-aligned, wrapped
  IDWriteTextFormat* centerFmt_ = nullptr;  // control labels: centered both axes
  IDWriteTextFormat* capFmt_ = nullptr;     // section captions: small, centered
  IDWriteTextFormat* capFmtRight_ = nullptr;// slider value: small, right-aligned
  ID2D1HwndRenderTarget* rt_ = nullptr;
  Brushes br_;

  Control hot_ = Control::None;   // button under the cursor; drives hover highlight
  int helpHot_ = kHelpNone;       // HelpTarget / SliderId under the cursor for the help strip
  int hotSec_ = -1;               // slider-group header under the cursor (hover highlight)
  int grabbed_ = -1;              // SliderId being dragged, or -1
  std::array<bool, kSliderGroupCount> secOpen_{true, true, true};   // groups expanded? (seeded from settings)
  float contentH_ = 0.0f;         // laid-out client height (drives the window resize); set by layout()
  float helpH_ = 84.0f;           // help-strip height; create() grows it to fit the longest text
  std::wstring ringEdit_;         // latency box edit buffer (digits); seeded from settings_->ringFrames
  bool ringFocused_ = false;      // latency box has keyboard focus (caret shown)

  // control rects (client coords), recomputed in layout()
  D2D1_RECT_F rcMicField_{}, rcOutField_{};
  D2D1_RECT_F rcBypass_{}, rcRelaunch_{}, rcQuit_{}, rcHelp_{};
  D2D1_RECT_F rcRingBox_{}, rcRingApply_{};   // editable latency box + Apply button
  std::array<D2D1_RECT_F, kSliderGroupCount> rcSecHdr_{};   // clickable slider-group header rows
  std::array<float, kSliderGroupCount> secY_{};             // slider-group header baselines

  std::array<Slider, kSliderCount> sliders_;       // value models, built from kSliderSpecs
  std::array<D2D1_RECT_F, kSliderCount> tracks_{}; // track rects (off-screen while collapsed)
};

}  // namespace vc::ui
