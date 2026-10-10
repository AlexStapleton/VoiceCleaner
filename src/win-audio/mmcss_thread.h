#pragma once
namespace vc {
// Register the CURRENT thread with MMCSS "Pro Audio" at AVRT_PRIORITY_CRITICAL.
// RAII: reverts on destruction. Leaking an MMCSS registration degrades the whole
// system, so this must always revert. Construct it on the thread you want registered.
// Also enables FTZ/DAZ (denormal flushing) on the current thread; that part is NOT reverted.
class MmcssThread {
public:
  MmcssThread();
  ~MmcssThread();
  MmcssThread(const MmcssThread&) = delete;
  MmcssThread& operator=(const MmcssThread&) = delete;
  bool ok() const { return handle_ != nullptr; }
private:
  void* handle_ = nullptr;   // HANDLE from AvSetMmThreadCharacteristicsW
};
}
