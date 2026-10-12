#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include "ui_state.h"
#include "worker_supervisor.h"
#include "device_picker.h"
#include "settings.h"
#include "slider.h"
#include "respawn_policy.h"
#include "param_outbox.h"

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
  void markSpawned() { spawned_ = true; }   // main.cpp calls this when it spawns at startup

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
  void sendCurrent(const vc::ipc::Params& p, bool throttled = false);   // stage p + try to deliver
  void flushParams(bool force);          // deliver outbox_.pending() if due; keeps it pending on failure
  int  hitControl(int x, int y) const;   // 0=RNNoise 1=DFN3 2=Bypass 3=Relaunch, -1=none
  void refreshDevices();                 // enumerate WASAPI devices + seed picker active from settings
  D2D1_RECT_F overlayRect() const;       // the open dropdown's bounding rect
  int  overlayHit(int x, int y) const;   // row index under (x,y) in the open overlay, or -1
  void applyPick(PickerKind k, size_t rowIndex);   // persist + spawn/switch
  Slider& sliderByIndex(int i);
  D2D1_RECT_F& trackByIndex(int i);
  int  trackHit(int x, int y) const;               // slider index whose track band contains (x,y), or -1
  int  helpTargetAt(int x, int y) const;           // control under (x,y) for the help strip (see helpText)
  std::wstring helpText() const;                   // plain-English "what + how" for helpHot_
  void setSliderFromX(int i, int x);               // set slider i's value from click/drag x
  bool aggressionEnabled() const;                  // false when active engine is RNNoise
  vc::ipc::Params applySlider(int i);              // push slider i's value into UiState -> Params
  void persistTuning();                            // write the active profile on drag-end
  void seedFromActiveProfile();                    // load the active mic's tuning into sliders + UiState
  void respawnWorker();                            // full stop + fresh spawn so ring_frames is re-read
  void toggleLowLatency();                          // flip [global].low_latency, persist, respawn
  void toggleSection(int i);                        // collapse/expand slider group i (persist + resize)
  void resizeToContent();                           // resize the window to the current laid-out height
  void applyRing();                                // commit+clamp the latency box, persist, respawn
  void commitRingEdit();                           // normalize ringEdit_ to the clamped pending value
  int  ringParse() const;                          // ringEdit_ digits -> int (0 if empty/non-digit)
  int  ringPendingClamped() const;                 // ringParse() clamped to [vc::kRingFramesMin, vc::kRingFramesMax]

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

  int hot_ = -1;   // control under the cursor (see hitControl); drives hover highlight
  int helpHot_ = -1;   // control under the cursor for the help strip (see helpTargetAt/helpText)
  int hotSec_ = -1;    // slider-group header under the cursor (hover highlight)
  bool secOpen_[3] = {true, true, true};   // DENOISE / VOICE LEVEL / CLEANUP expanded? (seeded from settings)
  float contentH_ = 956.0f;                // laid-out client height (drives the window resize)
  std::wstring ringEdit_;      // latency box edit buffer (digits); seeded from settings_->ringFrames
  bool ringFocused_ = false;   // latency box has keyboard focus (caret shown)

  // control rects (client coords), recomputed in layout()
  D2D1_RECT_F rcMicField_{}, rcOutField_{};
  D2D1_RECT_F rcEngineRnn_{}, rcEngineDfn_{}, rcBypass_{}, rcRelaunch_{}, rcQuit_{}, rcHelp_{};
  D2D1_RECT_F rcRingBox_{}, rcRingApply_{};   // editable latency box + Apply button
  D2D1_RECT_F rcLowLat_{};                    // low-latency checkbox (footer-left)
  D2D1_RECT_F rcSecHdr_[3]{};                 // clickable slider-group header rows

  Slider sldGain_{-30.0f, -6.0f, -16.0f, SliderFmt::Dbfs};
  Slider sldAggr_{0.0f, 1.0f, 0.0f, SliderFmt::Ratio2};
  Slider sldHpf_{40.0f, 200.0f, 80.0f, SliderFmt::Hz};
  Slider sldLim_{-6.0f, 0.0f, -1.0f, SliderFmt::Db};
  Slider sldDck_{0.0f, 1.0f, 0.0f, SliderFmt::Ratio2};
  Slider sldVfl_{0.0f, 1.0f, 0.0f, SliderFmt::Ratio2};
  Slider sldGate_{0.0f, 1.0f, 0.0f, SliderFmt::Ratio2};
  Slider sldMix_{0.0f, 1.0f, 1.0f, SliderFmt::Ratio2};
  Slider sldBoost_{0.0f, 30.0f, 18.0f, SliderFmt::Db};   // AGC max boost (MAX BOOST)
  D2D1_RECT_F rcGain_{}, rcAggr_{}, rcHpf_{}, rcLim_{}, rcDck_{}, rcVfl_{}, rcGate_{}, rcMix_{}, rcBoost_{};   // track rects
  float secY_[3] = {356.0f, 464.0f, 572.0f};   // section-header baselines: DENOISE / VOICE LEVEL / CLEANUP
  int grabbed_ = -1;        // slider being dragged: 0=gain 1=aggr 2=hpf 3=lim, -1=none
};

}  // namespace vc::ui
