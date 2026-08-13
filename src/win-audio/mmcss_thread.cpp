#include "mmcss_thread.h"
#include <windows.h>
#include <avrt.h>
namespace vc {
MmcssThread::MmcssThread() {
  DWORD taskIndex = 0;
  HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  if (h != nullptr) {
    AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
    handle_ = h;
  }
}
MmcssThread::~MmcssThread() {
  if (handle_ != nullptr) {
    AvRevertMmThreadCharacteristics(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}
}
