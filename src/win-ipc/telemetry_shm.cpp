#include "telemetry_shm.h"
#include <windows.h>
#include <cstring>
namespace vc::ipc {

TelemetryWriter::~TelemetryWriter() {
  if (view_) UnmapViewOfFile(view_);
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}
bool TelemetryWriter::create(const std::wstring& name) {
  if (view_)   { UnmapViewOfFile(view_); view_ = nullptr; }
  if (handle_) { CloseHandle(static_cast<HANDLE>(handle_)); handle_ = nullptr; }
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, sizeof(Telemetry), name.c_str());
  if (!h) return false;
  void* v = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(Telemetry));
  if (!v) { CloseHandle(h); return false; }
  handle_ = h; view_ = static_cast<Telemetry*>(v);
  std::memset(view_, 0, sizeof(Telemetry));
  return true;
}
void TelemetryWriter::write(const Telemetry& t) noexcept {
  if (view_) std::memcpy(view_, &t, sizeof(Telemetry));
}

void TelemetryReader::close() noexcept {
  if (view_)   { UnmapViewOfFile(const_cast<Telemetry*>(view_)); view_ = nullptr; }
  if (handle_) { CloseHandle(static_cast<HANDLE>(handle_)); handle_ = nullptr; }
}
TelemetryReader::~TelemetryReader() { close(); }
bool TelemetryReader::open(const std::wstring& name) {
  close();   // idempotent: release any prior mapping before re-binding (fixes reopen leak)
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (!h) return false;
  void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(Telemetry));
  if (!v) { CloseHandle(h); return false; }
  handle_ = h; view_ = static_cast<const Telemetry*>(v);
  return true;
}
Telemetry TelemetryReader::read() const noexcept {
  Telemetry t{};
  if (view_) std::memcpy(&t, view_, sizeof(Telemetry));
  return t;
}
}  // namespace vc::ipc
