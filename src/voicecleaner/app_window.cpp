#include "app_window.h"
#include "tray.h"
#include "device_enum.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace vc::ui {

static const wchar_t* kClass = L"VoiceCleanerWindow";
static const UINT kTimerId = 1;
static const float kRowH = 34.0f;   // dropdown row height
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
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassW(&wc);

  // Fixed, non-resizable window sized so the client area is exactly kClientW x kClientH.
  const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
  RECT wr{0, 0, 460, 664};
  AdjustWindowRectEx(&wr, style, FALSE, 0);
  hwnd_ = CreateWindowExW(0, kClass, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                          wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInst_, this);
  if (!hwnd_) return false;
  SetTimer(hwnd_, kTimerId, 100, nullptr);   // ~10 Hz telemetry poll
  refreshDevices();                           // initial enumeration + active seed
  ShowWindow(hwnd_, SW_SHOW);
  return true;
}

void AppWindow::refreshDevices() {
  std::vector<DeviceRow> cap, ren;
  enumerateDevices(cap, ren);
  picker_.setDevices(std::move(cap), std::move(ren));
  if (!settings_) return;
  picker_.setActive(settings_->activeCaptureId, settings_->renderId);
  if (vc::MicProfile* p = settings_->activeProfile()) {   // seed sliders + UiState from the active profile
    sldGain_.setValue(p->outputGainDbfs);
    sldAggr_.setValue(p->aggression);
    sldHpf_.setValue(p->hpfHz);
    sldLim_.setValue(p->limiterDbfs);
    sldDck_.setValue(p->declickStrength);
    state_->seedTuning(p->aggression, p->hpfHz, p->outputGainDbfs, p->limiterDbfs, p->declickStrength);
  }
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
  rcGain_ = D2D1::RectF(pad, 372, tw, 384);   // 12px track; caption row is drawn above it
  rcAggr_ = D2D1::RectF(pad, 416, tw, 428);
  rcHpf_  = D2D1::RectF(pad, 460, tw, 472);
  rcLim_  = D2D1::RectF(pad, 504, tw, 516);
  rcDck_  = D2D1::RectF(pad, 548, tw, 560);
  rcRelaunch_  = D2D1::RectF(pad, 580, sz.width - pad, 580 + h);   // only when Down
  rcQuit_      = D2D1::RectF(sz.width - pad - 90, sz.height - 36, sz.width - pad, sz.height - 10);
}

void AppWindow::paint() {
  ensureRenderTarget();
  if (!rt_) return;
  rt_->BeginDraw();
  rt_->Clear(D2D1::ColorF(0.106f, 0.109f, 0.129f));   // #1B1C21
  D2D1_SIZE_F sz = rt_->GetSize();
  layout(sz);

  ID2D1SolidColorBrush *text=nullptr,*dim=nullptr,*accent=nullptr,*accentHot=nullptr,
                       *panel=nullptr,*panelHot=nullptr,*disabled=nullptr,*amber=nullptr;
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.93f,0.93f,0.95f), &text);
  rt_->CreateSolidColorBrush(D2D1::ColorF(0.53f,0.55f,0.62f), &dim);
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

  // 5. Relaunch — only when the worker is down
  if (state_->statusKind()==StatusKind::Down) {
    box(rcRelaunch_, hot_==3 ? accentHot : accent);
    label(L"Relaunch worker", rcRelaunch_, text, centerFmt_);
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
  drawSlider(L"OUTPUT GAIN", 0, true);
  drawSlider(L"AGGRESSION",  1, aggressionEnabled());
  drawSlider(L"HPF",         2, true);
  drawSlider(L"LIMITER",     3, true);
  drawSlider(L"DE-CLICK",    4, true);

  // 7. Quit (footer, right-aligned) — full shutdown, same path as the tray Quit
  box(rcQuit_, hot_==4 ? panelHot : panel);
  label(L"Quit", rcQuit_, text, centerFmt_);

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
  panel->Release(); panelHot->Release(); disabled->Release(); amber->Release();
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
  return -1;
}

Slider& AppWindow::sliderByIndex(int i) {
  switch (i) { case 0: return sldGain_; case 1: return sldAggr_; case 2: return sldHpf_; case 3: return sldLim_; default: return sldDck_; }
}
D2D1_RECT_F& AppWindow::trackByIndex(int i) {
  switch (i) { case 0: return rcGain_; case 1: return rcAggr_; case 2: return rcHpf_; case 3: return rcLim_; default: return rcDck_; }
}
bool AppWindow::aggressionEnabled() const { return state_->activeEngine() == 1; }  // DFN3 only

int AppWindow::trackHit(int x, int y) const {
  const D2D1_RECT_F rc[5] = { rcGain_, rcAggr_, rcHpf_, rcLim_, rcDck_ };
  for (int i = 0; i < 5; ++i) {
    if (i == 1 && !aggressionEnabled()) continue;   // aggression not draggable on RNNoise
    if (x >= rc[i].left && x <= rc[i].right && y >= rc[i].top - 12 && y <= rc[i].bottom + 12) return i;
  }
  return -1;
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
    default: return state_->onSetDeclick(s.value());
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
  settings_->save(configPath_);
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
    const vc::MicProfile* p = settings_->activeProfile();
    uint32_t eng = (p && p->engine == "dfn3") ? 1u : 0u;
    sendCurrent(state_->onSelectEngine(eng));              // apply the mic's engine
  } else {
    sup_->sendSelectRender(id);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::requestRelaunch() {
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
  PollResult r = sup_->poll(GetTickCount64());
  if (!r.alive) state_->onWorkerDown();
  else {
    state_->onTelemetry(r.telemetry, r.ready);
    if (r.ready && !pushedTuning_) {   // apply the seeded profile tuning once the worker is running
      pushedTuning_ = true;
      vc::ipc::Command c; c.type = vc::ipc::CmdType::SetParams; c.params = state_->current();
      sup_->send(c);
    }
  }
  if (tray_) tray_->update(*state_);
  InvalidateRect(hwnd_, nullptr, FALSE);
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
      if (picker_.isOpen()) {                          // overlay takes priority
        int row = overlayHit(x, y);
        if (row >= 0) applyPick(picker_.openKind(), (size_t)row);
        else picker_.close();                          // click-away dismisses
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (hit(rcMicField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Capture); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
      if (hit(rcOutField_, x, y)) { refreshDevices(); picker_.open(PickerKind::Render);  InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
      { int s = trackHit(x, y);
        if (s >= 0) { grabbed_ = s; setSliderFromX(s, x); sendCurrent(applySlider(s)); InvalidateRect(hwnd_, nullptr, FALSE); return 0; } }
      switch (hitControl(x, y)) {
        case 0: onCommandEngine(0); break;
        case 1: onCommandEngine(1); break;
        case 2: onCommandBypass(); break;   // onCommandBypass() itself no-ops when disabled
        case 3: if (state_->statusKind()==StatusKind::Down) requestRelaunch(); break;
        case 4: sup_->shutdown(3000); DestroyWindow(hwnd_); break;   // full quit
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (w == VK_ESCAPE && picker_.isOpen()) { picker_.close(); InvalidateRect(hwnd_, nullptr, FALSE); }
      return 0;
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
      TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0 };
      TrackMouseEvent(&tme);                                             // so WM_MOUSELEAVE fires
      return 0;
    }
    case WM_LBUTTONUP:
      if (grabbed_ >= 0) { persistTuning(); grabbed_ = -1; }   // persist on drag-end
      return 0;
    case WM_MOUSELEAVE:
      if (hot_ != -1) { hot_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l)==HTCLIENT && hot_ != -1) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
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
