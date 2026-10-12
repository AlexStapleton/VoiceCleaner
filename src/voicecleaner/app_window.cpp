#include "app_window.h"
#include "tray.h"
#include "device_enum.h"
#include "resource.h"   // IDI_ACTIVE (window + taskbar icon)
#include "utf8.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <dwmapi.h>     // DwmSetWindowAttribute (dark, color-matched title bar)
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

// Present on Win10 2004+/Win11 SDKs, but guard so an older SDK still compiles.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace vc::ui {

static const wchar_t* kClass = L"VoiceCleanerWindow";
static const UINT kTimerId = 1;
static const UINT kTimerMs = 100;          // ~10 Hz telemetry poll
static const DWORD kShutdownWaitMs = 3000; // how long Quit / a respawn waits for the worker to exit
// Cached once: the same value is returned for the same name across processes, so main.cpp's
// single-instance broadcast lands here.
static const UINT kRestoreMsg = RegisterWindowMessageW(kRestoreMsgName);

// ---- layout (client px). Fixed width; the height flexes with the collapsed slider groups. ----
static const int   kClientW = 460;
static const float kPad = 18.0f;           // left/right margin
static const float kCtlH = 40.0f;          // field / button height
static const float kSegGap = 8.0f;         // gap between the two engine buttons
static const float kCaptionH = 16.0f;      // caption row above a field
static const float kMicY = 92.0f, kOutY = 160.0f, kEngineY = 232.0f, kBypassY = 300.0f;
static const float kStatusTop = 14.0f, kStatusBottom = 78.0f;
static const float kGroupsTop = 350.0f;    // first slider-group header
static const float kHeaderToTrack = 42.0f; // group header -> its first track
static const float kTrackH = 12.0f;
static const float kTrackPitch = 44.0f;    // track -> next track
static const float kCollapsedH = 24.0f;    // a collapsed group's header block
static const float kHelpMinH = 84.0f;      // hover help strip (grown to fit the longest text)
static const float kHelpInsetX = 12.0f, kHelpInsetY = 8.0f;   // text inset inside the help strip
static const float kFooterH = 40.0f;       // Low-latency checkbox + Quit
static const float kRowH = 34.0f;          // dropdown row height
static const D2D1_RECT_F kHidden = {-1000, -1000, -1000, -1000};   // collapsed slider tracks

// Background #1B1C21: painted client area and the DWM caption share it so the title bar blends in.
static const BYTE kBgR = 27, kBgG = 28, kBgB = 33;

static std::wstring toW(const std::string& s) { return vc::utf8ToWide(s); }

static bool hit(D2D1_RECT_F r, int x, int y) {
  return x>=r.left && x<=r.right && y>=r.top && y<=r.bottom;
}

AppWindow::AppWindow(HINSTANCE hInst, WorkerSupervisor* sup, UiState* state)
    : hInst_(hInst), sup_(sup), state_(state) {
  for (const SliderSpec& s : kSliderSpecs) slider(s.id) = Slider(s.lo, s.hi, s.def, s.fmt);
}

AppWindow::~AppWindow() {
  discardRenderTarget();
  if (capFmtRight_) capFmtRight_->Release();
  if (capFmt_) capFmt_->Release();
  if (centerFmt_) centerFmt_->Release();
  if (textFmt_) textFmt_->Release();
  if (dwFactory_) dwFactory_->Release();
  if (d2dFactory_) d2dFactory_->Release();
}

bool AppWindow::create(const wchar_t* title) {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&dwFactory_)))) return false;
  // Status line: normal weight, left-aligned, word-wrapped (the output device name can be long).
  dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               15.0f, L"en-us", &textFmt_);
  textFmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  textFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

  // Control labels: semibold, centered horizontally AND vertically inside their box.
  dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               15.0f, L"en-us", &centerFmt_);
  centerFmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  centerFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  centerFmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

  // Section captions: small, centered vertically, left-aligned.
  dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               11.0f, L"en-us", &capFmt_);
  capFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  // Slider value: small, right-aligned (shares the caption row with the left-aligned name).
  dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               11.0f, L"en-us", &capFmtRight_);
  capFmtRight_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  capFmtRight_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

  WNDCLASSW wc{};
  wc.lpfnWndProc = &AppWindow::WndProc;
  wc.hInstance = hInst_;
  wc.hIcon = LoadIconW(hInst_, MAKEINTRESOURCEW(IDI_ACTIVE));   // taskbar / Alt-Tab / default
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassW(&wc);

  helpH_ = measureHelpHeight();

  // Seed slider-group collapse state, then lay out once to learn the initial client height.
  if (settings_)
    for (int i = 0; i < kSliderGroupCount; ++i) secOpen_[i] = !((settings_->collapsedSections >> i) & 1u);
  layout(D2D1::SizeF((float)kClientW, 0.0f));   // computes contentH_ (Y math is height-independent)

  // Fixed-width, non-resizable window; height flexes with the collapsed sections (contentH_).
  const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
  RECT wr{0, 0, kClientW, (int)contentH_};
  AdjustWindowRectEx(&wr, style, FALSE, 0);
  hwnd_ = CreateWindowExW(0, kClass, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                          wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInst_, this);
  if (!hwnd_) return false;
  // Dark, color-matched title bar so the caption blends with the client background.
  { BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF caption = RGB(kBgR, kBgG, kBgB);
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    COLORREF capText = RGB(237, 237, 242);   // #EDEDF2 — light caption text
    DwmSetWindowAttribute(hwnd_, DWMWA_TEXT_COLOR, &capText, sizeof(capText)); }
  // Crisp per-size icons for the title bar (small) and Alt-Tab (big); class hIcon covers the taskbar.
  HICON hbig = (HICON)LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_ACTIVE), IMAGE_ICON,
      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
  if (hbig) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)hbig);
  HICON hsmall = (HICON)LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_ACTIVE), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
  if (hsmall) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)hsmall);
  SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);
  refreshDevices();                           // initial enumeration + active seed
  if (settings_) ringEdit_ = std::to_wstring((int)settings_->ringFrames);   // seed the latency box
  ShowWindow(hwnd_, SW_SHOW);
  return true;
}

void AppWindow::refreshDevices() {
  std::vector<DeviceRow> cap, ren;
  enumerateDevices(cap, ren);
  picker_.setDevices(std::move(cap), std::move(ren));
  if (!settings_) return;
  picker_.setActive(settings_->activeCaptureId, settings_->renderId);
  state_->setRenderLabel(picker_.fieldLabel(PickerKind::Render));   // status line "-> <output>"
  seedFromActiveProfile();   // seed sliders + UiState from the active profile
}

void AppWindow::seedFromActiveProfile() {
  if (!settings_) return;
  vc::MicProfile* p = settings_->activeProfile();
  if (!p) return;
  for (const SliderSpec& s : kSliderSpecs) slider(s.id).setValue(p->*s.profileField);
  state_->seedTuning(p->aggression, p->hpfHz, p->outputGainDbfs, p->limiterDbfs,
                     p->declickStrength, p->declickVoiceFloor, p->gateStrength, p->mixStrength,
                     p->maxBoostDb);
}

LRESULT CALLBACK AppWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCT*>(l);
    SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    reinterpret_cast<AppWindow*>(cs->lpCreateParams)->hwnd_ = h;
  }
  auto self = reinterpret_cast<AppWindow*>(GetWindowLongPtr(h, GWLP_USERDATA));
  if (self) return self->handle(m, w, l);
  return DefWindowProc(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

void AppWindow::ensureRenderTarget() {
  if (rt_) return;
  RECT rc; GetClientRect(hwnd_, &rc);
  D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
  if (FAILED(d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                 D2D1::HwndRenderTargetProperties(hwnd_, size), &rt_))) { rt_ = nullptr; return; }
  // Brushes belong to the render target: create them once here instead of on every paint.
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.93f,0.93f,0.95f), &br_.text);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.53f,0.55f,0.62f), &br_.dim);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.60f,0.64f,0.78f), &br_.section);   // group headers (brighter than dim)
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.16f,0.51f,0.94f), &br_.accent);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.30f,0.61f,0.98f), &br_.accentHot);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.20f,0.21f,0.25f), &br_.panel);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.27f,0.28f,0.33f), &br_.panelHot);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.15f,0.15f,0.18f), &br_.disabled);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.82f,0.54f,0.16f), &br_.amber);
}

void AppWindow::discardRenderTarget() {
  for (ID2D1SolidColorBrush** b : {&br_.text, &br_.dim, &br_.section, &br_.accent, &br_.accentHot,
                                   &br_.panel, &br_.panelHot, &br_.disabled, &br_.amber})
    if (*b) { (*b)->Release(); *b = nullptr; }
  if (rt_) { rt_->Release(); rt_ = nullptr; }
}

void AppWindow::layout(D2D1_SIZE_F sz) {
  const float right = sz.width - kPad;
  rcMicField_ = D2D1::RectF(kPad, kMicY, right, kMicY + kCtlH);
  rcOutField_ = D2D1::RectF(kPad, kOutY, right, kOutY + kCtlH);
  const float mid = sz.width * 0.5f;
  rcEngineRnn_ = D2D1::RectF(kPad, kEngineY, mid - kSegGap * 0.5f, kEngineY + kCtlH);
  rcEngineDfn_ = D2D1::RectF(mid + kSegGap * 0.5f, kEngineY, right, kEngineY + kCtlH);
  rcBypass_    = D2D1::RectF(kPad, kBypassY, right, kBypassY + kCtlH);

  // Collapsible slider groups. Positions flow from a running cursor so collapsing a group reflows
  // everything below it and shrinks the window (contentH_).
  float y = kGroupsTop;
  for (int g = 0; g < kSliderGroupCount; ++g) {
    const SliderGroup& grp = kSliderGroups[g];
    secY_[g]     = y;
    rcSecHdr_[g] = D2D1::RectF(kPad, y - 4, right, y + 16);   // clickable header row
    if (secOpen_[g]) {
      float t = y + kHeaderToTrack;
      for (int k = 0; k < grp.count; ++k) {
        track(grp.ids[k]) = D2D1::RectF(kPad, t, right, t + kTrackH);
        t += kTrackPitch;
      }
      y = t - kTrackPitch * 0.5f;                              // last track + half a pitch of gap
    } else {
      for (int k = 0; k < grp.count; ++k) track(grp.ids[k]) = kHidden;
      y += kCollapsedH;
    }
  }

  const float rbTop = y + 14.0f;
  rcRingBox_   = D2D1::RectF(kPad, rbTop, kPad + 120, rbTop + kCtlH);        // editable latency box
  rcRingApply_ = D2D1::RectF(right - 100, rbTop, right, rbTop + kCtlH);
  const float yHelp = rbTop + kCtlH + 16.0f;
  rcHelp_      = D2D1::RectF(kPad, yHelp, right, yHelp + helpH_);            // hover help strip
  rcRelaunch_  = D2D1::RectF(kPad, yHelp, right, yHelp + kCtlH);             // only when Down (shares the help band)
  contentH_    = yHelp + helpH_ + kFooterH;                                  // total client height
  rcQuit_      = D2D1::RectF(right - 90, contentH_ - 36, right, contentH_ - 10);
  rcLowLat_    = D2D1::RectF(kPad, contentH_ - 34, kPad + 210, contentH_ - 10);   // footer-left checkbox
}

void AppWindow::box(D2D1_RECT_F r, ID2D1Brush* b) {
  rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), b);
}

void AppWindow::label(const std::wstring& s, D2D1_RECT_F r, ID2D1Brush* b, IDWriteTextFormat* f) {
  rt_->DrawText(s.c_str(), (UINT32)s.size(), f, r, b);
}

void AppWindow::paint() {
  ensureRenderTarget();
  if (!rt_) return;
  rt_->BeginDraw();
  rt_->Clear(D2D1::ColorF(kBgR / 255.0f, kBgG / 255.0f, kBgB / 255.0f));
  const D2D1_SIZE_F sz = rt_->GetSize();
  layout(sz);

  drawStatus(sz.width);
  drawPickerField(L"MICROPHONE", rcMicField_, PickerKind::Capture);
  drawPickerField(L"OUTPUT",     rcOutField_, PickerKind::Render);
  drawEngineAndBypass(sz.width);
  drawHelpOrRelaunch();
  drawSliderGroups(sz.width);
  drawLatency(sz.width);
  drawFooter();
  drawPickerOverlay();   // last: the open dropdown sits on top of everything

  if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) discardRenderTarget();
}

void AppWindow::drawStatus(float width) {
  label(toW(state_->statusLine()), D2D1::RectF(kPad, kStatusTop, width - kPad, kStatusBottom), br_.text, textFmt_);
}

// Device picker field: caption above a box showing the active device name + a down caret.
void AppWindow::drawPickerField(const wchar_t* caption, D2D1_RECT_F r, PickerKind k) {
  label(caption, D2D1::RectF(r.left, r.top - kCaptionH, r.right, r.top), br_.dim, capFmt_);
  box(r, br_.panel);
  label(toW(picker_.fieldLabel(k)) + L"    \x25BE",
        D2D1::RectF(r.left + 12, r.top + 9, r.right - 12, r.bottom), br_.text, textFmt_);
}

void AppWindow::drawEngineAndBypass(float width) {
  // Engine segmented control: active = accent, hover = lighter panel.
  label(L"ENGINE", D2D1::RectF(kPad, kEngineY - kCaptionH, width - kPad, kEngineY), br_.dim, capFmt_);
  const uint32_t ae = state_->activeEngine();
  box(rcEngineRnn_, ae==0 ? br_.accent : (hot_==Control::EngineRnn ? br_.panelHot : br_.panel));
  box(rcEngineDfn_, ae==1 ? br_.accent : (hot_==Control::EngineDfn ? br_.panelHot : br_.panel));
  label(L"RNNoise",        rcEngineRnn_, br_.text, centerFmt_);
  label(L"DeepFilterNet3", rcEngineDfn_, br_.text, centerFmt_);

  // Bypass toggle: amber when bypassed; greyed + reason when there is no DSP chain.
  label(L"BYPASS", D2D1::RectF(kPad, kBypassY - kCaptionH, width - kPad, kBypassY), br_.dim, capFmt_);
  const bool bEn = state_->bypassEnabled();
  ID2D1Brush* bBox = !bEn ? static_cast<ID2D1Brush*>(br_.disabled)
                   : state_->bypassed() ? static_cast<ID2D1Brush*>(br_.amber)
                   : static_cast<ID2D1Brush*>(hot_==Control::Bypass ? br_.panelHot : br_.panel);
  box(rcBypass_, bBox);
  const std::wstring bLabel = !bEn ? toW(state_->bypassDisabledReason())
                            : state_->bypassed() ? L"Bypassed \x2014 click to enable"
                                                 : L"Enabled \x2014 click to bypass";
  label(bLabel, rcBypass_, bEn ? br_.text : br_.dim, centerFmt_);
}

// Relaunch (only when the worker is down) OR the hover help strip (otherwise) — they share the band.
void AppWindow::drawHelpOrRelaunch() {
  if (state_->statusKind() == StatusKind::Down) {
    box(rcRelaunch_, hot_==Control::Relaunch ? br_.accentHot : br_.accent);
    const std::wstring rlbl = respawn_.reconnecting()
        ? (L"Reconnecting\x2026 (" + std::to_wstring(respawn_.attempts()) + L"/" + std::to_wstring(respawn_.maxAttempts()) + L")")
        : L"Relaunch worker";
    label(rlbl, rcRelaunch_, br_.text, centerFmt_);
  } else {
    box(rcHelp_, br_.panel);
    label(helpTextFor(helpHot_), D2D1::RectF(rcHelp_.left + kHelpInsetX, rcHelp_.top + kHelpInsetY,
                                             rcHelp_.right - kHelpInsetX, rcHelp_.bottom - kHelpInsetY),
          helpHot_ == kHelpNone ? br_.dim : br_.text, textFmt_);
  }
}

// Collapsible group headers (chevron + caps title + hairline rule; brightens on hover) and the
// sliders of every open group.
void AppWindow::drawSliderGroups(float width) {
  for (int g = 0; g < kSliderGroupCount; ++g) {
    const SliderGroup& grp = kSliderGroups[g];
    const std::wstring title = (secOpen_[g] ? L"\x25BE  " : L"\x25B8  ") + std::wstring(grp.title);   // ▾ open / ▸ collapsed
    label(title, D2D1::RectF(kPad, secY_[g] - 2, width - kPad, secY_[g] + 16),
          hotSec_ == g ? br_.text : br_.section, capFmt_);
    const float ly = secY_[g] + 8.0f;
    rt_->DrawLine(D2D1::Point2F(kPad + grp.titleWidth, ly), D2D1::Point2F(width - kPad, ly), br_.section, 1.0f);
    if (secOpen_[g])
      for (int k = 0; k < grp.count; ++k) drawSlider(grp.ids[k]);
  }
}

// Caption + value above the track; the thumb is draggable.
void AppWindow::drawSlider(SliderId id) {
  const SliderSpec& spec = sliderSpec(id);
  const D2D1_RECT_F& tr = track(id);
  const bool enabled = sliderEnabled(id);
  const D2D1_RECT_F capRow = D2D1::RectF(tr.left, tr.top - 20, tr.right, tr.top - 4);
  label(spec.caption, capRow, br_.dim, capFmt_);
  label(enabled ? toW(slider(id).format()) : std::wstring(L"DFN3 only"), capRow,
        enabled ? br_.text : br_.dim, capFmtRight_);
  box(tr, br_.panel);
  const float cx = tr.left + slider(id).fraction() * (tr.right - tr.left);
  box(D2D1::RectF(cx - 6, tr.top - 6, cx + 6, tr.bottom + 6),
      enabled ? (grabbed_ == (int)id ? br_.accentHot : br_.accent) : br_.disabled);
}

// Latency (ring frames): editable box + approx-ms readout + Apply (restarts the worker).
void AppWindow::drawLatency(float width) {
  label(L"LATENCY (ring frames)", D2D1::RectF(kPad, rcRingBox_.top - 20, width - kPad, rcRingBox_.top - 4), br_.dim, capFmt_);
  box(rcRingBox_, ringFocused_ ? br_.panelHot : br_.panel);
  label(ringEdit_ + (ringFocused_ ? L"|" : L""),   // simple caret while focused
        D2D1::RectF(rcRingBox_.left + 12, rcRingBox_.top + 9, rcRingBox_.right - 8, rcRingBox_.bottom), br_.text, textFmt_);
  wchar_t ms[32];
  swprintf(ms, 32, L"\x2248 %d ms", ringParse() * 5);   // delay ~= frames x 5 ms (ring primed half full)
  label(ms, D2D1::RectF(rcRingBox_.right + 12, rcRingBox_.top + 9, rcRingApply_.left - 8, rcRingBox_.bottom), br_.dim, textFmt_);
  const bool ringDirty = settings_ && ringPendingClamped() != (int)settings_->ringFrames;
  box(rcRingApply_, ringDirty ? (hot_==Control::RingApply ? br_.accentHot : br_.accent) : br_.disabled);
  label(L"Apply", rcRingApply_, ringDirty ? br_.text : br_.dim, centerFmt_);
}

// Footer: Low-latency checkbox (left) and Quit (right; full shutdown, same path as the tray Quit).
void AppWindow::drawFooter() {
  box(rcQuit_, hot_==Control::Quit ? br_.panelHot : br_.panel);
  label(L"Quit", rcQuit_, br_.text, centerFmt_);

  const bool ll = settings_ && settings_->lowLatency;
  const D2D1_RECT_F cbx = D2D1::RectF(rcLowLat_.left, rcLowLat_.top + 3, rcLowLat_.left + 16, rcLowLat_.top + 19);
  rt_->DrawRectangle(cbx, hot_==Control::LowLatency ? br_.accentHot : br_.dim, 1.5f);
  if (ll) rt_->FillRectangle(D2D1::RectF(cbx.left + 3, cbx.top + 3, cbx.right - 3, cbx.bottom - 3), br_.accent);
  label(L"LOW LATENCY", D2D1::RectF(cbx.right + 8, rcLowLat_.top, rcLowLat_.right, rcLowLat_.bottom),
        ll ? br_.text : br_.dim, textFmt_);
}

void AppWindow::drawPickerOverlay() {
  if (!picker_.isOpen()) return;
  const PickerKind k = picker_.openKind();
  const D2D1_RECT_F o = overlayRect();
  box(o, br_.panelHot);
  const auto& rows = picker_.rows(k);
  for (size_t i = 0; i < rows.size(); ++i) {
    const float top = o.top + kRowH * (float)i;
    const DeviceRow& d = rows[i];
    const bool sel = DevicePicker::selectable(d);
    const bool active = (d.id == picker_.activeId(k));
    std::wstring line = (active ? L"\x2713 " : L"   ") + toW(d.name);
    if (!sel) line += L"   \x2014 48 kHz only";
    label(line, D2D1::RectF(o.left + 12, top + 8, o.right - 12, top + kRowH), sel ? br_.text : br_.dim, textFmt_);
  }
}

// ---------------------------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------------------------

AppWindow::Control AppWindow::hitControl(int x, int y) const {
  if (hit(rcEngineRnn_, x, y)) return Control::EngineRnn;
  if (hit(rcEngineDfn_, x, y)) return Control::EngineDfn;
  if (hit(rcBypass_,    x, y)) return Control::Bypass;
  if (hit(rcRelaunch_,  x, y)) return Control::Relaunch;
  if (hit(rcQuit_,      x, y)) return Control::Quit;
  if (hit(rcRingApply_, x, y)) return Control::RingApply;
  if (hit(rcLowLat_,    x, y)) return Control::LowLatency;
  return Control::None;
}

bool AppWindow::sliderEnabled(SliderId id) const {
  return !sliderSpec(id).dfn3Only || state_->activeEngine() == 1;
}

int AppWindow::trackHit(int x, int y) const {
  for (const SliderSpec& s : kSliderSpecs) {
    if (!sliderEnabled(s.id)) continue;   // e.g. aggression is not draggable on RNNoise
    const D2D1_RECT_F& r = track(s.id);
    if (x >= r.left && x <= r.right && y >= r.top - 12 && y <= r.bottom + 12) return (int)s.id;
  }
  return -1;
}

int AppWindow::helpTargetAt(int x, int y) const {
  // Sliders first (same horizontal band as the track, extended up to cover the caption row).
  for (const SliderSpec& s : kSliderSpecs) {
    const D2D1_RECT_F& r = track(s.id);
    if (x >= r.left && x <= r.right && y >= r.top - 20 && y <= r.bottom + 8) return (int)s.id;
  }
  if (hit(rcMicField_, x, y))  return kHelpMic;
  if (hit(rcOutField_, x, y))  return kHelpOutput;
  if (hit(rcEngineRnn_, x, y)) return kHelpEngineRnn;
  if (hit(rcEngineDfn_, x, y)) return kHelpEngineDfn;
  if (hit(rcBypass_, x, y))    return kHelpBypass;
  if (hit(rcRingBox_, x, y) || hit(rcRingApply_, x, y)) return kHelpLatency;
  if (hit(rcLowLat_, x, y))    return kHelpLowLatency;
  return kHelpNone;
}

std::wstring AppWindow::helpTextFor(int target) {
  if (target >= 0 && target < kSliderCount) return kSliderSpecs[target].help;
  switch (target) {
    case kHelpMic:        return L"MICROPHONE \x2014 the input device being cleaned. Click to choose a different mic.";
    case kHelpOutput:     return L"OUTPUT \x2014 where the cleaned audio is sent (e.g. CABLE Input, which apps pick as your mic). Click to choose.";
    case kHelpEngineRnn:  return L"ENGINE: RNNoise \x2014 lighter, low-latency denoiser. Best at keyboard/mouse clicks \x2014 use this in a clicky room.";
    case kHelpEngineDfn:  return L"ENGINE: DeepFilterNet3 \x2014 stronger on steady background noise, but passes short clicks (use DE-CLICK or RNNoise for those).";
    case kHelpBypass:     return L"BYPASS \x2014 pass your mic through untouched with denoising off. Click to toggle (A/B your setup).";
    case kHelpLatency:    return L"LATENCY \x2014 audio buffer depth in frames (1 frame = 10 ms audio; buffered delay \x2248 frames \x00D7 5 ms). Higher tolerates glitches better but adds delay. Applying restarts the audio engine (brief dropout).";
    case kHelpLowLatency: return L"LOW LATENCY \x2014 run RNNoise at its native 20 ms engine delay instead of matching DeepFilterNet3\x2019s 30 ms (saves 10 ms). Switching to DFN3 later causes a one-time timing shift. Toggling restarts the audio engine (brief dropout).";
    default:              return L"Hover a slider or control to see what it does and how to use it.";
  }
}

// The help strip must fit its longest text: lay out every help string at the strip's text width
// with the strip's font and take the tallest (a fixed height let 5-line texts spill into the footer).
float AppWindow::measureHelpHeight() const {
  const float textW = (float)kClientW - 2.0f * kPad - 2.0f * kHelpInsetX;
  float tallest = 0.0f;
  auto measure = [&](int target) {
    const std::wstring s = helpTextFor(target);
    IDWriteTextLayout* tl = nullptr;
    if (SUCCEEDED(dwFactory_->CreateTextLayout(s.c_str(), (UINT32)s.size(), textFmt_, textW, 10000.0f, &tl))) {
      DWRITE_TEXT_METRICS m{};
      if (SUCCEEDED(tl->GetMetrics(&m)) && m.height > tallest) tallest = m.height;
      tl->Release();
    }
  };
  for (int i = 0; i < kSliderCount; ++i) measure(i);
  for (int t = kHelpMic; t <= kHelpBypass; ++t) measure(t);
  measure(kHelpNone);
  const float h = tallest + 2.0f * kHelpInsetY + 4.0f;   // + a little air below the last line
  return h > kHelpMinH ? h : kHelpMinH;
}

D2D1_RECT_F AppWindow::overlayRect() const {
  const D2D1_RECT_F& field = (picker_.openKind()==PickerKind::Capture) ? rcMicField_ : rcOutField_;
  const auto& rows = picker_.rows(picker_.openKind());
  float h = kRowH * (rows.empty() ? 1.0f : (float)rows.size());
  return D2D1::RectF(field.left, field.bottom + 2, field.right, field.bottom + 2 + h);
}

int AppWindow::overlayHit(int x, int y) const {
  if (!picker_.isOpen()) return -1;
  D2D1_RECT_F o = overlayRect();
  if (x < o.left || x > o.right || y < o.top || y > o.bottom) return -1;
  int idx = (int)(((float)y - o.top) / kRowH);
  const auto& rows = picker_.rows(picker_.openKind());
  return (idx >= 0 && idx < (int)rows.size()) ? idx : -1;
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------

void AppWindow::setSliderFromX(SliderId id, int x) {
  const D2D1_RECT_F& r = track(id);
  const float t = (r.right > r.left) ? ((float)x - r.left) / (r.right - r.left) : 0.0f;
  slider(id).setFraction(t);
}

vc::ipc::Params AppWindow::applySlider(SliderId id) {
  return (state_->*sliderSpec(id).apply)(slider(id).value());
}

void AppWindow::persistTuning() {
  if (!settings_) return;
  vc::MicProfile* p = settings_->activeProfile();
  if (!p) return;
  for (const SliderSpec& s : kSliderSpecs) p->*s.profileField = slider(s.id).value();
  settings_->save(configPath_);
}

int AppWindow::ringParse() const {
  if (ringEdit_.empty()) return 0;
  int v = 0;
  for (wchar_t c : ringEdit_) {
    if (c < L'0' || c > L'9') return 0;
    v = v * 10 + (int)(c - L'0');
    if (v > 100000) return 100000;   // guard overflow on absurd input
  }
  return v;
}

int AppWindow::ringPendingClamped() const { return (int)vc::clampRingFrames(ringParse()); }

void AppWindow::commitRingEdit() {
  ringEdit_ = std::to_wstring(ringPendingClamped());
}

void AppWindow::startWorker() {
  pushedTuning_ = false;     // re-push the active mic's tuning once the worker is ready (tick())
  sup_->startOrAttach();
}

void AppWindow::respawnWorker() {
  sup_->shutdown(kShutdownWaitMs);   // fully stop the current worker process
  startWorker();                     // spawns fresh -> re-reads settings.toml (ring_frames, low_latency)
  invalidate();
}

void AppWindow::quit() {
  sup_->shutdown(kShutdownWaitMs);
  DestroyWindow(hwnd_);
}

void AppWindow::toggleLowLatency() {
  if (!settings_) return;
  settings_->lowLatency = !settings_->lowLatency;   // global; read by the bridge at worker start
  settings_->save(configPath_);
  if (spawned_) respawnWorker();   // first-run (no worker yet): value takes effect at first spawn
  else invalidate();
}

void AppWindow::resizeToContent() {
  layout(D2D1::SizeF((float)kClientW, 0.0f));   // recompute contentH_ for the current collapse state
  RECT wr{0, 0, kClientW, (int)contentH_};
  const DWORD style = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
  AdjustWindowRectEx(&wr, style, FALSE, 0);
  SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // keep top-left; height follows content
}

void AppWindow::toggleSection(int i) {
  if (i < 0 || i >= kSliderGroupCount) return;
  secOpen_[i] = !secOpen_[i];
  if (settings_) {
    if (secOpen_[i]) settings_->collapsedSections &= ~(1u << i);
    else             settings_->collapsedSections |=  (1u << i);
    settings_->save(configPath_);
  }
  resizeToContent();
  invalidate();
}

void AppWindow::applyRing() {
  if (!settings_) return;
  commitRingEdit();
  int v = ringPendingClamped();
  if ((size_t)v == settings_->ringFrames) return;   // no change -> nothing to restart
  settings_->ringFrames = (size_t)v;
  settings_->save(configPath_);
  if (spawned_) respawnWorker();   // first-run (no worker yet): value takes effect at first spawn
}

void AppWindow::sendCurrent(const vc::ipc::Params& p, bool throttled) {
  outbox_.stage(p);
  flushParams(/*force=*/!throttled);
  invalidate();
}

// Deliver the newest staged Params. A send can fail when it lands while the worker's single-instance
// pipe is being recycled or the worker is busy (e.g. mid device-reselect); the value then stays
// pending and is retried by the next drag move, the drag end, or tick(), instead of being dropped.
void AppWindow::flushParams(bool force) {
  if (!spawned_ || state_->statusKind() == StatusKind::Down) return;   // no worker: the ready-path re-push covers it
  const uint64_t now = GetTickCount64();
  if (!outbox_.due(now, force)) return;
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SetParams; c.params = outbox_.pending();
  outbox_.onAttempt(now, sup_->send(c));
}

void AppWindow::onCommandEngine(uint32_t engine) {
  sendCurrent(state_->onSelectEngine(engine));
  // Remember the choice in the active mic's profile: the worker starts on it, and switching mics
  // (applyPick) restores each mic's own engine.
  if (vc::MicProfile* p = settings_ ? settings_->activeProfile() : nullptr) {
    const char* name = engine == 1 ? "dfn3" : "rnnoise";
    if (p->engine != name) { p->engine = name; settings_->save(configPath_); }
  }
}

void AppWindow::onCommandBypass() { if (state_->bypassEnabled()) sendCurrent(state_->onToggleBypass()); }

void AppWindow::applyPick(PickerKind k, size_t rowIndex) {
  const std::vector<DeviceRow>& rows = picker_.rows(k);
  if (rowIndex >= rows.size()) { picker_.close(); return; }
  const DeviceRow chosen = rows[rowIndex];          // copy before pick() mutates picker state
  std::string id = picker_.pick(k, rowIndex);       // "" if non-selectable / unchanged
  if (id.empty()) { invalidate(); return; }
  if (!settings_) { picker_.close(); return; }

  if (k == PickerKind::Capture) {
    settings_->ensureProfile(id, chosen.name);
    settings_->activeCaptureId = id;
  } else {
    settings_->renderId = id;
    state_->setRenderLabel(chosen.name);
  }
  settings_->save(configPath_);

  if (!spawned_) {
    // First run: spawn once BOTH devices are chosen; tick() then starts polling.
    if (!settings_->activeCaptureId.empty() && !settings_->renderId.empty()) {
      sup_->startOrAttach();
      spawned_ = true;
    }
  } else if (k == PickerKind::Capture) {
    sup_->sendSelectCapture(id, /*driftSeed*/0.0);         // live pipeline rebuild on the new mic
    seedFromActiveProfile();                                // load the new mic's sliders + tuning into state_
    const vc::MicProfile* p = settings_->activeProfile();
    uint32_t eng = (p && p->engine == "dfn3") ? 1u : 0u;
    sendCurrent(state_->onSelectEngine(eng));              // push the new mic's engine + tuning live
  } else {
    sup_->sendSelectRender(id);
  }
  invalidate();
}

void AppWindow::requestRelaunch() {
  respawn_.reset();          // manual relaunch clears any auto-respawn episode
  startWorker();
  invalidate();
}

void AppWindow::showRestore() {
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void AppWindow::tick() {
  if (state_->statusKind() == StatusKind::NoConfig && !spawned_) {   // no worker yet: show pickers, don't poll
    if (tray_) tray_->update(*state_);
    return;
  }
  const uint64_t nowMs = GetTickCount64();
  PollResult r = sup_->poll(nowMs);
  if (!r.alive) state_->onWorkerDown();
  else {
    state_->onTelemetry(r.telemetry, r.ready);
    if (r.ready && !pushedTuning_) {   // apply the seeded profile tuning once the worker is running
      pushedTuning_ = true;
      sendCurrent(state_->current());   // via the outbox, so a failed push is retried below
    } else if (r.ready) {
      flushParams(/*force=*/false);      // retry any value whose earlier send failed or was throttled
    }
  }
  // Auto-respawn on a post-ready death (e.g. device loss); launch failures do not loop.
  if (respawn_.onTick(r.alive, r.ready, !r.launchFailure, nowMs) == RespawnPolicy::Action::Respawn)
    startWorker();
  if (tray_) tray_->update(*state_);
  // Only repaint when a paint-relevant input actually changed. All tick-driven visuals derive
  // from the telemetry snapshot (keyed by seq, which bumps ~1 Hz on each aggregate publish), the
  // status kind, or the respawn attempt count. Interaction repaints happen on their own WM events.
  const uint32_t seq = r.alive ? r.telemetry.seq : 0u;
  const StatusKind kind = state_->statusKind();
  const int attempts = respawn_.attempts();
  if (seq != lastPaintSeq_ || kind != lastPaintKind_ || attempts != lastPaintAttempts_) {
    lastPaintSeq_ = seq; lastPaintKind_ = kind; lastPaintAttempts_ = attempts;
    invalidate();
  }
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void AppWindow::onMouseDown(int x, int y) {
  if (ringFocused_ && !hit(rcRingBox_, x, y)) { commitRingEdit(); ringFocused_ = false; }   // click-away commits
  if (picker_.isOpen()) {                          // overlay takes priority
    int row = overlayHit(x, y);
    if (row >= 0) applyPick(picker_.openKind(), (size_t)row);
    else picker_.close();                          // click-away dismisses
    invalidate();
    return;
  }
  if (hit(rcMicField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Capture); invalidate(); return; }
  if (hit(rcOutField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Render);  invalidate(); return; }
  if (hit(rcRingBox_, x, y))  { ringFocused_ = true; invalidate(); return; }
  for (int i = 0; i < kSliderGroupCount; ++i) if (hit(rcSecHdr_[i], x, y)) { toggleSection(i); return; }
  if (int s = trackHit(x, y); s >= 0) {
    grabbed_ = s;
    setSliderFromX((SliderId)s, x);
    sendCurrent(applySlider((SliderId)s));
    return;
  }
  switch (hitControl(x, y)) {
    case Control::EngineRnn:  onCommandEngine(0); break;
    case Control::EngineDfn:  onCommandEngine(1); break;
    case Control::Bypass:     onCommandBypass(); break;   // no-ops when disabled
    case Control::Relaunch:   if (state_->statusKind()==StatusKind::Down) requestRelaunch(); break;
    case Control::Quit:       quit(); break;
    case Control::RingApply:  applyRing(); break;         // persist + respawn
    case Control::LowLatency: toggleLowLatency(); break;  // persist + respawn
    case Control::None:       break;
  }
}

void AppWindow::onMouseMove(int x, int y) {
  if (grabbed_ >= 0) {   // dragging a slider
    setSliderFromX((SliderId)grabbed_, x);
    sendCurrent(applySlider((SliderId)grabbed_), /*throttled=*/true);   // drag: <= 1 send / 30 ms
    return;
  }
  Control h = hitControl(x, y);
  if (h==Control::Bypass && !state_->bypassEnabled()) h = Control::None;                  // disabled bypass: no hover
  if (h==Control::Relaunch && state_->statusKind()!=StatusKind::Down) h = Control::None;  // relaunch only when down
  if (h != hot_) { hot_ = h; invalidate(); }
  const int hp = helpTargetAt(x, y);                                 // hovered control for the help strip
  if (hp != helpHot_) { helpHot_ = hp; invalidate(); }
  int hsec = -1;                                                     // hovered slider-group header
  for (int i = 0; i < kSliderGroupCount; ++i) if (hit(rcSecHdr_[i], x, y)) hsec = i;
  if (hsec != hotSec_) { hotSec_ = hsec; invalidate(); }
  TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0 };
  TrackMouseEvent(&tme);                                             // so WM_MOUSELEAVE fires
}

LRESULT AppWindow::handle(UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_TIMER: if (w==kTimerId) tick(); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd_, &ps); paint(); EndPaint(hwnd_, &ps); return 0; }
    case WM_SIZE:
      if (rt_) { RECT rc; GetClientRect(hwnd_, &rc); rt_->Resize(D2D1::SizeU(rc.right, rc.bottom)); }
      return 0;
    case WM_LBUTTONDOWN: onMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
    case WM_MOUSEMOVE:   onMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
    case WM_LBUTTONUP:
      if (grabbed_ >= 0) { flushParams(/*force=*/true); persistTuning(); grabbed_ = -1; invalidate(); }   // deliver final value + persist
      return 0;
    case WM_KEYDOWN:
      if (w == VK_ESCAPE && picker_.isOpen()) { picker_.close(); invalidate(); }
      return 0;
    case WM_CHAR:
      if (ringFocused_) {
        wchar_t c = (wchar_t)w;
        if (c >= L'0' && c <= L'9') { if (ringEdit_.size() < 4) ringEdit_.push_back(c); }
        else if (c == L'\b') { if (!ringEdit_.empty()) ringEdit_.pop_back(); }   // Backspace
        else if (c == L'\r') { applyRing(); ringFocused_ = false; }              // Enter commits + applies
        else if (c == 27)    { commitRingEdit(); ringFocused_ = false; }         // Esc cancels edit
        invalidate();
        return 0;
      }
      break;
    case WM_MOUSELEAVE:
      if (hot_ != Control::None || helpHot_ != kHelpNone || hotSec_ != -1) {
        hot_ = Control::None; helpHot_ = kHelpNone; hotSec_ = -1; invalidate();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l)==HTCLIENT && (hot_ != Control::None || hotSec_ != -1)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
      break;
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case kTrayCmdEnabled:   onCommandBypass(); break;
        case kTrayCmdEngineRnn: onCommandEngine(0); break;
        case kTrayCmdEngineDfn: onCommandEngine(1); break;
        case kTrayCmdOpen:      showRestore(); break;
        case kTrayCmdQuit:      quit(); break;
      }
      return 0;
    case WM_TRAY:
      if (LOWORD(l)==WM_LBUTTONUP || LOWORD(l)==WM_LBUTTONDBLCLK) showRestore();
      else if (LOWORD(l)==WM_RBUTTONUP && tray_) tray_->showMenu(*state_);
      return 0;
    case WM_SYSCOMMAND:
      if ((w & 0xFFF0) == SC_MINIMIZE) { ShowWindow(hwnd_, SW_HIDE); return 0; }  // minimize -> tray
      break;
    case WM_CLOSE: ShowWindow(hwnd_, SW_HIDE); return 0;  // close -> tray (worker keeps running)
    case WM_DESTROY: KillTimer(hwnd_, kTimerId); PostQuitMessage(0); return 0;
    default:
      if (msg == kRestoreMsg) { showRestore(); return 0; }
      break;
  }
  return DefWindowProc(hwnd_, msg, w, l);
}

}  // namespace vc::ui
