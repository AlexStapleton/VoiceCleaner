#include "app_window.h"
#include "tray.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace vc::ui {

static const wchar_t* kClass = L"VoiceCleanerWindow";
static const UINT kTimerId = 1;
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
  if (textFmt_) textFmt_->Release();
  if (dwFactory_) dwFactory_->Release();
  if (d2dFactory_) d2dFactory_->Release();
}

bool AppWindow::create(const wchar_t* title) {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&dwFactory_)))) return false;
  dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               16.0f, L"en-us", &textFmt_);

  WNDCLASSW wc{};
  wc.lpfnWndProc = &AppWindow::WndProc;
  wc.hInstance = hInst_;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassW(&wc);

  hwnd_ = CreateWindowExW(0, kClass, title, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                          CW_USEDEFAULT, CW_USEDEFAULT, 460, 260, nullptr, nullptr, hInst_, this);
  if (!hwnd_) return false;
  SetTimer(hwnd_, kTimerId, 100, nullptr);   // ~10 Hz telemetry poll
  ShowWindow(hwnd_, SW_SHOW);
  return true;
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
  const float pad = 16.0f;
  rcEngineRnn_ = D2D1::RectF(pad,        56, sz.width/2, 92);
  rcEngineDfn_ = D2D1::RectF(sz.width/2, 56, sz.width - pad, 92);
  rcBypass_    = D2D1::RectF(pad, 110, sz.width - pad, 150);
  rcRelaunch_  = D2D1::RectF(pad, 170, sz.width - pad, 206);
}

void AppWindow::paint() {
  ensureRenderTarget();
  if (!rt_) return;
  rt_->BeginDraw();
  rt_->Clear(D2D1::ColorF(0.12f, 0.12f, 0.14f));
  D2D1_SIZE_F sz = rt_->GetSize();
  layout(sz);

  ID2D1SolidColorBrush* text = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.92f,0.92f,0.94f), &text);
  ID2D1SolidColorBrush* accent = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.20f,0.55f,0.95f), &accent);
  ID2D1SolidColorBrush* panel = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.22f,0.22f,0.26f), &panel);
  ID2D1SolidColorBrush* dim = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.45f,0.45f,0.48f), &dim);
  ID2D1SolidColorBrush* disabledBg = nullptr; rt_->CreateSolidColorBrush(D2D1::ColorF(0.18f,0.18f,0.20f), &disabledBg);

  auto drawText = [&](const std::wstring& s, D2D1_RECT_F r, ID2D1Brush* b) {
    rt_->DrawText(s.c_str(), (UINT32)s.size(), textFmt_, r, b);
  };
  auto fillBox = [&](D2D1_RECT_F r, ID2D1Brush* b) {
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), b);
  };

  // 1. status line
  drawText(toW(state_->statusLine()), D2D1::RectF(16, 16, sz.width-16, 48), text);

  // 2. engine segmented control (highlight the active engine)
  fillBox(rcEngineRnn_, state_->activeEngine()==0 ? accent : panel);
  fillBox(rcEngineDfn_, state_->activeEngine()==1 ? accent : panel);
  drawText(L"  RNNoise", rcEngineRnn_, text);
  drawText(L"  DeepFilterNet3", rcEngineDfn_, text);

  // 3. bypass toggle (disabled + reason when no chain)
  const bool bEn = state_->bypassEnabled();
  fillBox(rcBypass_, bEn ? static_cast<ID2D1Brush*>(panel) : static_cast<ID2D1Brush*>(disabledBg));
  std::wstring bLabel = !bEn ? toW(state_->bypassDisabledReason())
                             : (state_->bypassed() ? L"  Bypassed \xE2\x80\x94 click to enable"
                                                   : L"  Enabled \xE2\x80\x94 click to bypass");
  drawText(bLabel, rcBypass_, bEn ? text : dim);

  // 4. Relaunch — only when the worker is down
  if (state_->statusKind()==StatusKind::Down) {
    fillBox(rcRelaunch_, accent);
    drawText(L"  Relaunch worker", rcRelaunch_, text);
  }

  text->Release(); accent->Release(); panel->Release(); dim->Release(); disabledBg->Release();
  if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) discardRenderTarget();
}

static bool hit(D2D1_RECT_F r, int x, int y) {
  return x>=r.left && x<=r.right && y>=r.top && y<=r.bottom;
}

void AppWindow::sendCurrent(const vc::ipc::Params& p) {
  vc::ipc::Command c; c.type = vc::ipc::CmdType::SetParams; c.params = p;
  sup_->send(c);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::onCommandEngine(uint32_t engine) { sendCurrent(state_->onSelectEngine(engine)); }
void AppWindow::onCommandBypass() { if (state_->bypassEnabled()) sendCurrent(state_->onToggleBypass()); }

void AppWindow::requestRelaunch() {
  sup_->startOrAttach();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::showRestore() {
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void AppWindow::tick() {
  if (state_->statusKind() == StatusKind::NoConfig) {   // pre-flight NoConfig is terminal this phase — never poll/spawn (design §8)
    if (tray_) tray_->update(*state_);
    return;
  }
  PollResult r = sup_->poll(GetTickCount64());
  if (!r.alive) state_->onWorkerDown();
  else          state_->onTelemetry(r.telemetry, r.ready);
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
      if (hit(rcEngineRnn_, x, y)) onCommandEngine(0);
      else if (hit(rcEngineDfn_, x, y)) onCommandEngine(1);
      else if (hit(rcBypass_, x, y)) onCommandBypass();
      else if (state_->statusKind()==StatusKind::Down && hit(rcRelaunch_, x, y)) requestRelaunch();
      return 0;
    }
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
