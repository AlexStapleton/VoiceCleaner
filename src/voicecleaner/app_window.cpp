#include "app_window.h"
#include "tray.h"
#include "device_enum.h"
#include "resource.h"   // IDI_ACTIVE (window + taskbar icon)
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
static const int  kClientW = 460;   // fixed client width; height flexes with collapsed sections
static const UINT kTimerId = 1;
static const float kRowH = 34.0f;   // dropdown row height
static const int kRingMin = 4;      // below the worker's floor of 3; keeps the value meaningful
static const int kRingMax = 512;    // ~2.5 s buffered (ring_frames x 5 ms)
// Cached once: the same value is returned for the same name across processes, so main.cpp's
// single-instance broadcast lands here (design note, Task 14).
static const UINT kRestoreMsg = RegisterWindowMessageW(L"VoiceCleanerRestore");

static std::wstring toW(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

AppWindow::AppWindow(HINSTANCE hInst, WorkerSupervisor* sup, UiState* state)
    : hInst_(hInst), sup_(sup), state_(state) {}

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
  // Status line: normal weight, left-aligned, word-wrapped (the NoConfig message is long).
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

  // Seed slider-group collapse state, then lay out once to learn the initial client height.
  if (settings_) for (int i = 0; i < 3; ++i) secOpen_[i] = !((settings_->collapsedSections >> i) & 1u);
  layout(D2D1::SizeF((float)kClientW, 0.0f));   // computes contentH_ (Y math is height-independent)

  // Fixed-width, non-resizable window; height flexes with the collapsed sections (contentH_).
  const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
  RECT wr{0, 0, kClientW, (int)contentH_};
  AdjustWindowRectEx(&wr, style, FALSE, 0);
  hwnd_ = CreateWindowExW(0, kClass, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                          wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInst_, this);
  if (!hwnd_) return false;
  // Dark, color-matched title bar so the caption blends with the client background (#1B1C21).
  { BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF caption = RGB(27, 28, 33);      // #1B1C21 — same as the painted background
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
  SetTimer(hwnd_, kTimerId, 100, nullptr);   // ~10 Hz telemetry poll
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
  seedFromActiveProfile();   // seed sliders + UiState from the active profile
}

void AppWindow::seedFromActiveProfile() {
  if (!settings_) return;
  vc::MicProfile* p = settings_->activeProfile();
  if (!p) return;
  sldGain_.setValue(p->outputGainDbfs);
  sldAggr_.setValue(p->aggression);
  sldHpf_.setValue(p->hpfHz);
  sldLim_.setValue(p->limiterDbfs);
  sldDck_.setValue(p->declickStrength);
  sldVfl_.setValue(p->declickVoiceFloor);
  sldGate_.setValue(p->gateStrength);
  sldMix_.setValue(p->mixStrength);
  state_->seedTuning(p->aggression, p->hpfHz, p->outputGainDbfs, p->limiterDbfs,
                     p->declickStrength, p->declickVoiceFloor, p->gateStrength, p->mixStrength);
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

void AppWindow::ensureRenderTarget() {
  if (rt_) return;
  RECT rc; GetClientRect(hwnd_, &rc);
  D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
  d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(hwnd_, size), &rt_);
}

void AppWindow::discardRenderTarget() { if (rt_) { rt_->Release(); rt_ = nullptr; } }

void AppWindow::layout(D2D1_SIZE_F sz) {
  const float pad = 18.0f, gap = 8.0f, h = 40.0f;
  rcMicField_ = D2D1::RectF(pad, 92,  sz.width - pad, 92 + h);
  rcOutField_ = D2D1::RectF(pad, 160, sz.width - pad, 160 + h);
  const float mid = sz.width * 0.5f;
  rcEngineRnn_ = D2D1::RectF(pad,           232, mid - gap * 0.5f, 232 + h);
  rcEngineDfn_ = D2D1::RectF(mid + gap*0.5f, 232, sz.width - pad,  232 + h);
  rcBypass_    = D2D1::RectF(pad, 300, sz.width - pad, 300 + h);
  const float tw = sz.width - pad;

  // Collapsible slider groups. Positions flow from a running cursor so collapsing a group reflows
  // everything below it and shrinks the window (contentH_). The index->parameter mapping is unchanged
  // (see sliderByIndex/helpText); only the Y positions, draw order, and visibility change per group.
  static const int denoise[2] = {7, 1};        // STRENGTH, AGGRESSION
  static const int voice[2]   = {0, 3};        // TARGET LOUDNESS, LIMITER
  static const int cleanup[4] = {2, 6, 4, 5};  // HPF, NOISE GATE, DE-CLICK, SPEECH DECLICK
  const int* groups[3] = {denoise, voice, cleanup};
  const int  counts[3] = {2, 2, 4};
  const D2D1_RECT_F hidden = D2D1::RectF(-1000, -1000, -1000, -1000);

  float y = 350.0f;
  for (int s = 0; s < 3; ++s) {
    secY_[s]     = y;
    rcSecHdr_[s] = D2D1::RectF(pad, y - 4, sz.width - pad, y + 16);   // clickable header row
    if (secOpen_[s]) {
      float t = y + 42.0f;                                            // header -> first track
      for (int k = 0; k < counts[s]; ++k) {
        trackByIndex(groups[s][k]) = D2D1::RectF(pad, t, tw, t + 12);
        t += 44.0f;
      }
      y = t - 22.0f;                                                  // last track bottom + a section gap
    } else {
      for (int k = 0; k < counts[s]; ++k) trackByIndex(groups[s][k]) = hidden;
      y += 24.0f;                                                     // collapsed: header block only
    }
  }

  const float rbTop = y + 14.0f;
  rcRingBox_   = D2D1::RectF(pad, rbTop, pad + 120, rbTop + h);        // editable latency box
  rcRingApply_ = D2D1::RectF(sz.width - pad - 100, rbTop, sz.width - pad, rbTop + h);
  const float yHelp = rbTop + h + 16.0f;
  rcHelp_      = D2D1::RectF(pad, yHelp, sz.width - pad, yHelp + 84);  // hover help strip
  rcRelaunch_  = D2D1::RectF(pad, yHelp, sz.width - pad, yHelp + h);   // only when Down (shares the help band)
  contentH_    = yHelp + 84.0f + 40.0f;                               // + footer band -> total client height
  rcQuit_      = D2D1::RectF(sz.width - pad - 90, contentH_ - 36, sz.width - pad, contentH_ - 10);
  rcLowLat_    = D2D1::RectF(pad, contentH_ - 34, pad + 210, contentH_ - 10);   // footer-left checkbox
}

void AppWindow::paint() {
  ensureRenderTarget();
  if (!rt_) return;
  rt_->BeginDraw();
  rt_->Clear(D2D1::ColorF(0.106f, 0.109f, 0.129f));   // #1B1C21
  D2D1_SIZE_F sz = rt_->GetSize();
  layout(sz);

  ID2D1SolidColorBrush *text=nullptr,*dim=nullptr,*accent=nullptr,*accentHot=nullptr,
                       *panel=nullptr,*panelHot=nullptr,*disabled=nullptr,*amber=nullptr,*section=nullptr;
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.93f,0.93f,0.95f), &text);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.53f,0.55f,0.62f), &dim);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.60f,0.64f,0.78f), &section);   // group headers (brighter than dim)
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.16f,0.51f,0.94f), &accent);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.30f,0.61f,0.98f), &accentHot);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.20f,0.21f,0.25f), &panel);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.27f,0.28f,0.33f), &panelHot);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.15f,0.15f,0.18f), &disabled);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.82f,0.54f,0.16f), &amber);

  const float pad = 18.0f;
  auto box = [&](D2D1_RECT_F r, ID2D1Brush* b) {
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), b);
  };
  auto label = [&](const std::wstring& s, D2D1_RECT_F r, ID2D1Brush* b, IDWriteTextFormat* f) {
    rt_->DrawText(s.c_str(), (UINT32)s.size(), f, r, b);
  };

  // 1. status line (wrapped, left-aligned)
  label(toW(state_->statusLine()), D2D1::RectF(pad, 14, sz.width - pad, 78), text, textFmt_);

  // 2. device pickers (Microphone + Output): caption above a field showing the active name + ▾
  auto drawField = [&](const wchar_t* cap, D2D1_RECT_F r, PickerKind k) {
    label(cap, D2D1::RectF(r.left, r.top - 16, r.right, r.top), dim, capFmt_);
    box(r, panel);
    std::wstring name = toW(picker_.fieldLabel(k)) + L"    ▾";   // name + down-caret
    D2D1_RECT_F tr = D2D1::RectF(r.left + 12, r.top + 9, r.right - 12, r.bottom);
    rt_->DrawText(name.c_str(), (UINT32)name.size(), textFmt_, tr, text);
  };
  drawField(L"MICROPHONE", rcMicField_, PickerKind::Capture);
  drawField(L"OUTPUT",     rcOutField_, PickerKind::Render);

  // 3. engine segmented control (caption + two buttons; active = accent, hover = lighter panel)
  label(L"ENGINE", D2D1::RectF(pad, 216, sz.width - pad, 232), dim, capFmt_);
  const uint32_t ae = state_->activeEngine();
  box(rcEngineRnn_, ae==0 ? accent : (hot_==0 ? panelHot : panel));
  box(rcEngineDfn_, ae==1 ? accent : (hot_==1 ? panelHot : panel));
  label(L"RNNoise",        rcEngineRnn_, text, centerFmt_);
  label(L"DeepFilterNet3", rcEngineDfn_, text, centerFmt_);

  // 4. bypass toggle (amber when bypassed; greyed + reason when no DSP chain)
  label(L"BYPASS", D2D1::RectF(pad, 284, sz.width - pad, 300), dim, capFmt_);
  const bool bEn = state_->bypassEnabled();
  ID2D1Brush* bBox = !bEn ? static_cast<ID2D1Brush*>(disabled)
                   : state_->bypassed() ? static_cast<ID2D1Brush*>(amber)
                   : static_cast<ID2D1Brush*>(hot_==2 ? panelHot : panel);
  box(rcBypass_, bBox);
  std::wstring bLabel = !bEn ? toW(state_->bypassDisabledReason())
                       : state_->bypassed() ? L"Bypassed — click to enable"
                                            : L"Enabled — click to bypass";
  label(bLabel, rcBypass_, bEn ? text : dim, centerFmt_);

  // 5. Relaunch (only when the worker is down) OR the hover help strip (otherwise) — they share the band.
  if (state_->statusKind()==StatusKind::Down) {
    box(rcRelaunch_, hot_==3 ? accentHot : accent);
    std::wstring rlbl = respawn_.reconnecting()
        ? (L"Reconnecting\x2026 (" + std::to_wstring(respawn_.attempts()) + L"/" + std::to_wstring(respawn_.maxAttempts()) + L")")
        : L"Relaunch worker";
    label(rlbl.c_str(), rcRelaunch_, text, centerFmt_);
  } else {
    box(rcHelp_, panel);
    D2D1_RECT_F htr = D2D1::RectF(rcHelp_.left + 12, rcHelp_.top + 8, rcHelp_.right - 12, rcHelp_.bottom - 8);
    std::wstring h = helpText();
    label(h, htr, helpHot_ < 0 ? dim : text, textFmt_);
  }

  // 6. Tuning sliders (caption + value above; track with a draggable thumb)
  auto drawSlider = [&](const wchar_t* cap, int i, bool enabled) {
    D2D1_RECT_F& tr = trackByIndex(i);
    ID2D1Brush* fg = enabled ? text : dim;
    label(cap, D2D1::RectF(tr.left, tr.top - 20, tr.right, tr.top - 4), dim, capFmt_);
    std::wstring val = enabled ? toW(sliderByIndex(i).format()) : L"DFN3 only";
    label(val, D2D1::RectF(tr.left, tr.top - 20, tr.right, tr.top - 4), fg, capFmtRight_);
    box(tr, panel);
    float f = sliderByIndex(i).fraction();
    float cx = tr.left + f * (tr.right - tr.left);
    D2D1_RECT_F thumb = D2D1::RectF(cx - 6, tr.top - 6, cx + 6, tr.bottom + 6);
    box(thumb, enabled ? (grabbed_==i ? accentHot : accent) : disabled);
  };
  // Collapsible section header: chevron (open/collapsed) + caps label + a hairline rule to the edge.
  // The header brightens on hover (hotSec_). Clicking the row toggles the group (see toggleSection).
  auto sectionHdr = [&](const wchar_t* s, int i, float labelW) {
    std::wstring lbl = (secOpen_[i] ? L"\x25BE  " : L"\x25B8  ") + std::wstring(s);   // ▾ open / ▸ collapsed
    label(lbl, D2D1::RectF(pad, secY_[i] - 2, sz.width - pad, secY_[i] + 16),
          hotSec_ == i ? text : section, capFmt_);
    float ly = secY_[i] + 8.0f;
    rt_->DrawLine(D2D1::Point2F(pad + labelW, ly), D2D1::Point2F(sz.width - pad, ly), section, 1.0f);
  };
  sectionHdr(L"DENOISE", 0, 100);
  if (secOpen_[0]) { drawSlider(L"STRENGTH", 7, true); drawSlider(L"AGGRESSION", 1, aggressionEnabled()); }
  sectionHdr(L"VOICE LEVEL", 1, 120);
  if (secOpen_[1]) { drawSlider(L"TARGET LOUDNESS", 0, true); drawSlider(L"LIMITER", 3, true); }
  sectionHdr(L"CLEANUP", 2, 100);
  if (secOpen_[2]) { drawSlider(L"HPF", 2, true); drawSlider(L"NOISE GATE", 6, true);
                     drawSlider(L"DE-CLICK", 4, true); drawSlider(L"SPEECH DECLICK", 5, true); }

  // 6b. Latency (ring frames): editable box + approx-ms readout + Apply (restarts the worker)
  label(L"LATENCY (ring frames)", D2D1::RectF(pad, rcRingBox_.top - 20, sz.width - pad, rcRingBox_.top - 4), dim, capFmt_);
  box(rcRingBox_, ringFocused_ ? panelHot : panel);
  {
    std::wstring shown = ringEdit_;
    if (ringFocused_) shown += L"|";                       // simple caret while focused
    D2D1_RECT_F br = D2D1::RectF(rcRingBox_.left + 12, rcRingBox_.top + 9, rcRingBox_.right - 8, rcRingBox_.bottom);
    rt_->DrawText(shown.c_str(), (UINT32)shown.size(), textFmt_, br, text);
    wchar_t ms[32];
    swprintf(ms, 32, L"\x2248 %d ms", ringParse() * 5);   // U+2248 (almost-equal); delay ~= frames x 5 ms
    D2D1_RECT_F mr = D2D1::RectF(rcRingBox_.right + 12, rcRingBox_.top + 9, rcRingApply_.left - 8, rcRingBox_.bottom);
    rt_->DrawText(ms, (UINT32)wcslen(ms), textFmt_, mr, dim);
  }
  const bool ringDirty = settings_ && ringPendingClamped() != (int)settings_->ringFrames;
  box(rcRingApply_, ringDirty ? (hot_==5 ? accentHot : accent) : disabled);
  label(L"Apply", rcRingApply_, ringDirty ? text : dim, centerFmt_);

  // 7. Quit (footer, right-aligned) — full shutdown, same path as the tray Quit
  box(rcQuit_, hot_==4 ? panelHot : panel);
  label(L"Quit", rcQuit_, text, centerFmt_);

  // 7b. Low-latency checkbox (footer-left) — toggles [global].low_latency (respawns the worker)
  {
    const bool ll = settings_ && settings_->lowLatency;
    D2D1_RECT_F cbx = D2D1::RectF(rcLowLat_.left, rcLowLat_.top + 3, rcLowLat_.left + 16, rcLowLat_.top + 19);
    rt_->DrawRectangle(cbx, hot_==6 ? accentHot : dim, 1.5f);
    if (ll) rt_->FillRectangle(D2D1::RectF(cbx.left + 3, cbx.top + 3, cbx.right - 3, cbx.bottom - 3), accent);
    D2D1_RECT_F cbl = D2D1::RectF(cbx.right + 8, rcLowLat_.top, rcLowLat_.right, rcLowLat_.bottom);
    label(L"LOW LATENCY", cbl, ll ? text : dim, textFmt_);
  }

  // 7. Open dropdown overlay — drawn last, on top of everything
  if (picker_.isOpen()) {
    PickerKind k = picker_.openKind();
    D2D1_RECT_F o = overlayRect();
    box(o, panelHot);
    const auto& drows = picker_.rows(k);
    for (size_t i = 0; i < drows.size(); ++i) {
      float top = o.top + kRowH * (float)i;
      D2D1_RECT_F rr = D2D1::RectF(o.left, top, o.right, top + kRowH);
      const DeviceRow& d = drows[i];
      const bool sel = DevicePicker::selectable(d);
      const bool active = (d.id == picker_.activeId(k));
      std::wstring line = (active ? L"✓ " : L"   ") + toW(d.name);
      if (!sel) line += L"   — 48 kHz only";
      D2D1_RECT_F tr = D2D1::RectF(rr.left + 12, rr.top + 8, rr.right - 12, rr.bottom);
      rt_->DrawText(line.c_str(), (UINT32)line.size(), textFmt_, tr, sel ? text : dim);
    }
  }

  text->Release(); dim->Release(); accent->Release(); accentHot->Release();
  panel->Release(); panelHot->Release(); disabled->Release(); amber->Release(); section->Release();
  if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) discardRenderTarget();
}

static bool hit(D2D1_RECT_F r, int x, int y) {
  return x>=r.left && x<=r.right && y>=r.top && y<=r.bottom;
}

int AppWindow::hitControl(int x, int y) const {
  if (hit(rcEngineRnn_, x, y)) return 0;
  if (hit(rcEngineDfn_, x, y)) return 1;
  if (hit(rcBypass_,    x, y)) return 2;
  if (hit(rcRelaunch_,  x, y)) return 3;
  if (hit(rcQuit_,      x, y)) return 4;
  if (hit(rcRingApply_, x, y)) return 5;
  if (hit(rcLowLat_,    x, y)) return 6;
  return -1;
}

Slider& AppWindow::sliderByIndex(int i) {
  switch (i) { case 0: return sldGain_; case 1: return sldAggr_; case 2: return sldHpf_; case 3: return sldLim_; case 4: return sldDck_; case 5: return sldVfl_; case 6: return sldGate_; default: return sldMix_; }
}
D2D1_RECT_F& AppWindow::trackByIndex(int i) {
  switch (i) { case 0: return rcGain_; case 1: return rcAggr_; case 2: return rcHpf_; case 3: return rcLim_; case 4: return rcDck_; case 5: return rcVfl_; case 6: return rcGate_; default: return rcMix_; }
}
bool AppWindow::aggressionEnabled() const { return state_->activeEngine() == 1; }  // DFN3 only

int AppWindow::trackHit(int x, int y) const {
  const D2D1_RECT_F rc[8] = { rcGain_, rcAggr_, rcHpf_, rcLim_, rcDck_, rcVfl_, rcGate_, rcMix_ };
  for (int i = 0; i < 8; ++i) {
    if (i == 1 && !aggressionEnabled()) continue;   // aggression not draggable on RNNoise
    if (x >= rc[i].left && x <= rc[i].right && y >= rc[i].top - 12 && y <= rc[i].bottom + 12) return i;
  }
  return -1;
}
int AppWindow::helpTargetAt(int x, int y) const {
  // Sliders first (same horizontal band as the track, extended up to cover the caption row).
  const D2D1_RECT_F rc[8] = { rcGain_, rcAggr_, rcHpf_, rcLim_, rcDck_, rcVfl_, rcGate_, rcMix_ };
  for (int i = 0; i < 8; ++i)
    if (x >= rc[i].left && x <= rc[i].right && y >= rc[i].top - 20 && y <= rc[i].bottom + 8) return i;
  if (hit(rcMicField_, x, y))  return 100;
  if (hit(rcOutField_, x, y))  return 101;
  if (hit(rcEngineRnn_, x, y)) return 200;
  if (hit(rcEngineDfn_, x, y)) return 201;
  if (hit(rcBypass_, x, y))    return 202;
  if (hit(rcRingBox_, x, y) || hit(rcRingApply_, x, y)) return 102;
  if (hit(rcLowLat_, x, y)) return 103;
  return -1;
}

std::wstring AppWindow::helpText() const {
  switch (helpHot_) {
    case 0:   return L"TARGET LOUDNESS — auto-levels your voice to this level while you speak (an AGC, not a raw volume). It only acts when the VAD dot in the status line is lit. Watch the AGC readout move as you drag.";
    case 1:   return L"AGGRESSION — how hard DeepFilterNet3 scrubs noise (DFN3 only). Higher is cleaner but can sound processed.";
    case 2:   return L"HPF — high-pass filter. Cuts low rumble, handling thumps and AC hum below the set frequency. Raise to remove more low end.";
    case 3:   return L"LIMITER — a ceiling that stops loud peaks from clipping. Lower the ceiling for safer, quieter peaks.";
    case 4:   return L"DE-CLICK — ducks short clicks (keyboard/mouse) in the gaps between words. Raise until clicks fade; too high can soften consonants.";
    case 5:   return L"SPEECH DECLICK — how much de-click applies while you talk. 0 = never touch your voice (safest). Raise only if clicks ride on your speech.";
    case 6:   return L"NOISE GATE — silences the gaps between words so the amplified noise floor / typing static drops out when you're not speaking. Raise until the gaps go quiet; too high can clip soft word endings.";
    case 7:   return L"STRENGTH — how hard the denoiser works. Lower blends your natural (un-denoised) voice back in to soften musical-noise / 'laser' artifacts (RNNoise); 100% = full denoise.";
    case 100: return L"MICROPHONE — the input device being cleaned. Click to choose a different mic.";
    case 101: return L"OUTPUT — where the cleaned audio is sent (e.g. CABLE Input, which apps pick as your mic). Click to choose.";
    case 200: return L"ENGINE: RNNoise — lighter, low-latency denoiser. Best at keyboard/mouse clicks — use this in a clicky room.";
    case 201: return L"ENGINE: DeepFilterNet3 — stronger on steady background noise, but passes short clicks (use DE-CLICK or RNNoise for those).";
    case 202: return L"BYPASS — pass your mic through untouched with denoising off. Click to toggle (A/B your setup).";
    case 102: return L"LATENCY — audio buffer depth in frames (1 frame = 10 ms audio; buffered delay \x2248 frames \x00D7 5 ms). Higher tolerates glitches better but adds delay. Applying restarts the audio engine (brief dropout).";
    case 103: return L"LOW LATENCY — run RNNoise at its native \x223C 0 ms engine delay (drops the \x223C 30 ms DFN3 alignment pin). Switching to DFN3 later causes a one-time timing shift. Toggling restarts the audio engine (brief dropout).";
    default:  return L"Hover a slider or control to see what it does and how to use it.";
  }
}

void AppWindow::setSliderFromX(int i, int x) {
  D2D1_RECT_F& r = trackByIndex(i);
  float t = (r.right > r.left) ? ((float)x - r.left) / (r.right - r.left) : 0.0f;
  sliderByIndex(i).setFraction(t);
}
vc::ipc::Params AppWindow::applySlider(int i) {
  Slider& s = sliderByIndex(i);
  switch (i) {
    case 0: return state_->onSetOutputGain(s.value());
    case 1: return state_->onSetAggression(s.value());
    case 2: return state_->onSetHpf(s.value());
    case 3: return state_->onSetLimiter(s.value());
    case 4: return state_->onSetDeclick(s.value());
    case 5: return state_->onSetDeclickVoiceFloor(s.value());
    case 6: return state_->onSetGate(s.value());
    default: return state_->onSetMix(s.value());
  }
}
void AppWindow::persistTuning() {
  if (!settings_) return;
  vc::MicProfile* p = settings_->activeProfile();
  if (!p) return;
  p->outputGainDbfs = sldGain_.value();
  p->aggression     = sldAggr_.value();
  p->hpfHz          = sldHpf_.value();
  p->limiterDbfs    = sldLim_.value();
  p->declickStrength = sldDck_.value();
  p->declickVoiceFloor = sldVfl_.value();
  p->gateStrength = sldGate_.value();
  p->mixStrength = sldMix_.value();
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

int AppWindow::ringPendingClamped() const {
  int v = ringParse();
  if (v < kRingMin) v = kRingMin;
  if (v > kRingMax) v = kRingMax;
  return v;
}

void AppWindow::commitRingEdit() {
  ringEdit_ = std::to_wstring(ringPendingClamped());
}

void AppWindow::respawnWorker() {
  sup_->shutdown(3000);      // fully stop the current worker process
  pushedTuning_ = false;     // re-push the active mic's tuning to the fresh worker (tick() readiness path)
  sup_->startOrAttach();     // spawns fresh -> reads the new ring_frames
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::toggleLowLatency() {
  if (!settings_) return;
  settings_->lowLatency = !settings_->lowLatency;   // global; read by the bridge at worker start
  settings_->save(configPath_);
  if (spawned_) respawnWorker();   // first-run (no worker yet): value takes effect at first spawn
  else InvalidateRect(hwnd_, nullptr, FALSE);
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
  if (i < 0 || i > 2) return;
  secOpen_[i] = !secOpen_[i];
  if (settings_) {
    if (secOpen_[i]) settings_->collapsedSections &= ~(1u << i);
    else             settings_->collapsedSections |=  (1u << i);
    settings_->save(configPath_);
  }
  resizeToContent();
  InvalidateRect(hwnd_, nullptr, FALSE);
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

void AppWindow::sendCurrent(const vc::ipc::Params& p) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SetParams; c.params = p;
  sup_->send(c);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::onCommandEngine(uint32_t engine) { sendCurrent(state_->onSelectEngine(engine)); }
void AppWindow::onCommandBypass() { if (state_->bypassEnabled()) sendCurrent(state_->onToggleBypass()); }

void AppWindow::applyPick(PickerKind k, size_t rowIndex) {
  const std::vector<DeviceRow>& rows = picker_.rows(k);
  if (rowIndex >= rows.size()) { picker_.close(); return; }
  const DeviceRow chosen = rows[rowIndex];          // copy before pick() mutates picker state
  std::string id = picker_.pick(k, rowIndex);       // "" if non-selectable / unchanged
  if (id.empty()) { InvalidateRect(hwnd_, nullptr, FALSE); return; }
  if (!settings_) { picker_.close(); return; }

  if (k == PickerKind::Capture) {
    settings_->ensureProfile(id, chosen.name);
    settings_->activeCaptureId = id;
  } else {
    settings_->renderId = id;
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
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::requestRelaunch() {
  respawn_.reset();          // manual relaunch clears any auto-respawn episode
  pushedTuning_ = false;
  sup_->startOrAttach();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::showRestore() {
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void AppWindow::tick() {
  if (state_->statusKind() == StatusKind::NoConfig && !spawned_) {   // no worker yet: show pickers, don't poll (design §8)
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
      vc::ipc::Command c; c.type = vc::ipc::CmdType::SetParams; c.params = state_->current();
      sup_->send(c);
    }
  }
  // Auto-respawn on a post-ready death (e.g. device loss); launch failures do not loop (design C3).
  if (respawn_.onTick(r.alive, r.ready, !r.launchFailure, nowMs) == RespawnPolicy::Action::Respawn) {
    pushedTuning_ = false;             // re-push the seeded tuning once the fresh worker is ready
    sup_->startOrAttach();
  }
  if (tray_) tray_->update(*state_);
  // F5: only repaint when a paint-relevant input actually changed. All tick-driven visuals derive
  // from the telemetry snapshot (keyed by seq, which bumps ~1 Hz on each aggregate publish), the
  // status kind, or the respawn attempt count. Interaction repaints happen on their own WM events.
  const uint32_t seq = r.alive ? r.telemetry.seq : 0u;
  const StatusKind kind = state_->statusKind();
  const int attempts = respawn_.attempts();
  if (seq != lastPaintSeq_ || kind != lastPaintKind_ || attempts != lastPaintAttempts_) {
    lastPaintSeq_ = seq; lastPaintKind_ = kind; lastPaintAttempts_ = attempts;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

LRESULT AppWindow::handle(UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_TIMER: if (w==kTimerId) tick(); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd_, &ps); paint(); EndPaint(hwnd_, &ps); return 0; }
    case WM_SIZE:
      if (rt_) { RECT rc; GetClientRect(hwnd_, &rc); rt_->Resize(D2D1::SizeU(rc.right, rc.bottom)); }
      return 0;
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
      if (ringFocused_ && !hit(rcRingBox_, x, y)) { commitRingEdit(); ringFocused_ = false; }   // click-away commits
      if (picker_.isOpen()) {                          // overlay takes priority
        int row = overlayHit(x, y);
        if (row >= 0) applyPick(picker_.openKind(), (size_t)row);
        else picker_.close();                          // click-away dismisses
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (hit(rcMicField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Capture); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
      if (hit(rcOutField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Render);  InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
      if (hit(rcRingBox_, x, y)) { ringFocused_ = true; InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
      for (int i = 0; i < 3; ++i) if (hit(rcSecHdr_[i], x, y)) { toggleSection(i); return 0; }
      { int s = trackHit(x, y);
        if (s >= 0) { grabbed_ = s; setSliderFromX(s, x); sendCurrent(applySlider(s)); InvalidateRect(hwnd_, nullptr, FALSE); return 0; } }
      switch (hitControl(x, y)) {
        case 0: onCommandEngine(0); break;
        case 1: onCommandEngine(1); break;
        case 2: onCommandBypass(); break;   // onCommandBypass() itself no-ops when disabled
        case 3: if (state_->statusKind()==StatusKind::Down) requestRelaunch(); break;
        case 4: sup_->shutdown(3000); DestroyWindow(hwnd_); break;   // full quit
        case 5: applyRing(); break;   // latency Apply -> persist + respawn
        case 6: toggleLowLatency(); break;   // low-latency checkbox -> persist + respawn
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (w == VK_ESCAPE && picker_.isOpen()) { picker_.close(); InvalidateRect(hwnd_, nullptr, FALSE); }
      return 0;
    case WM_CHAR:
      if (ringFocused_) {
        wchar_t c = (wchar_t)w;
        if (c >= L'0' && c <= L'9') { if (ringEdit_.size() < 4) ringEdit_.push_back(c); }
        else if (c == L'\b') { if (!ringEdit_.empty()) ringEdit_.pop_back(); }   // Backspace
        else if (c == L'\r') { applyRing(); ringFocused_ = false; }              // Enter commits + applies
        else if (c == 27)    { commitRingEdit(); ringFocused_ = false; }         // Esc cancels edit
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_MOUSEMOVE: {
      if (grabbed_ >= 0) {   // dragging a slider
        setSliderFromX(grabbed_, GET_X_LPARAM(l));
        sendCurrent(applySlider(grabbed_));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      int h = hitControl(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      if (h==2 && !state_->bypassEnabled()) h = -1;                       // disabled bypass: no hover
      if (h==3 && state_->statusKind()!=StatusKind::Down) h = -1;         // relaunch only when down
      if (h != hot_) { hot_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
      int hp = helpTargetAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));           // hovered control for the help strip
      if (hp != helpHot_) { helpHot_ = hp; InvalidateRect(hwnd_, nullptr, FALSE); }
      int hsec = -1;                                                     // hovered slider-group header
      for (int i = 0; i < 3; ++i) if (hit(rcSecHdr_[i], GET_X_LPARAM(l), GET_Y_LPARAM(l))) hsec = i;
      if (hsec != hotSec_) { hotSec_ = hsec; InvalidateRect(hwnd_, nullptr, FALSE); }
      TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0 };
      TrackMouseEvent(&tme);                                             // so WM_MOUSELEAVE fires
      return 0;
    }
    case WM_LBUTTONUP:
      if (grabbed_ >= 0) { persistTuning(); grabbed_ = -1; }   // persist on drag-end
      return 0;
    case WM_MOUSELEAVE:
      if (hot_ != -1 || helpHot_ != -1 || hotSec_ != -1) { hot_ = -1; helpHot_ = -1; hotSec_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l)==HTCLIENT && (hot_ != -1 || hotSec_ != -1)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
      break;
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case kTrayCmdEnabled:  onCommandBypass(); break;
        case kTrayCmdEngineRnn: onCommandEngine(0); break;
        case kTrayCmdEngineDfn: onCommandEngine(1); break;
        case kTrayCmdOpen:     showRestore(); break;
        case kTrayCmdQuit:     sup_->shutdown(3000); DestroyWindow(hwnd_); break;
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
