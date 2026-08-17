#pragma once
#include <string>
#include "telemetry.h"
namespace vc::ipc {
// Worker side: owns a named file-mapping and writes the Telemetry block (single writer, relaxed).
class TelemetryWriter {
public:
  ~TelemetryWriter();
  bool create(const std::wstring& name);   // false on failure
  void write(const Telemetry& t) noexcept; // memcpy into the mapped view
private:
  void* handle_ = nullptr;   // HANDLE
  Telemetry* view_ = nullptr;
};
// UI side: opens the same mapping read-only and copies the block out.
class TelemetryReader {
public:
  ~TelemetryReader();
  bool open(const std::wstring& name);     // false if the mapping doesn't exist
  Telemetry read() const noexcept;         // returns a copy (torn reads tolerated)
  void close() noexcept;                    // release the mapping; safe to call repeatedly
private:
  void* handle_ = nullptr;
  const Telemetry* view_ = nullptr;
};
}  // namespace vc::ipc
