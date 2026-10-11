#include "mmcss_thread.h"
#include <windows.h>
#include <avrt.h>
#include <xmmintrin.h>   // _MM_SET_FLUSH_ZERO_MODE
#include <pmmintrin.h>   // _MM_SET_DENORMALS_ZERO_MODE
namespace vc {
MmcssThread::MmcssThread() {
  // Denormal guard (blueprint §9.5): flush-to-zero + denormals-are-zero for this thread's SSE math.
  // In silence the IIR state (DC blocker, HPF, declicker/AGC envelopes, RNNoise GRU) decays into the
  // denormal range, where each op costs ~100x; measured ~25x on DC blocker + HPF alone. MXCSR is
  // per-thread, and every audio thread (capture, render, relay) constructs this first, so setting it
  // here covers the whole RT path, including ORT (DFN3 runs on the relay thread: intra-op threads = 1).
  // Deliberately NOT restored on destruction: the thread stays an audio thread until it exits.
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

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
