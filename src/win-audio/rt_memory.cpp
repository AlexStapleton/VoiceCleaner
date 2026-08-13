#include "rt_memory.h"
#include <windows.h>
namespace vc {
bool rt_lock_and_touch(void* addr, size_t bytes) {
  if (addr == nullptr || bytes == 0) return false;
  const bool locked = VirtualLock(addr, bytes) != 0;
  // Pre-touch every 4 KiB page regardless of lock success, so first-touch faults happen now.
  volatile unsigned char* p = static_cast<volatile unsigned char*>(addr);
  const size_t page = 4096;
  for (size_t off = 0; off < bytes; off += page) {
    p[off] = p[off];   // read-modify-write forces the page resident
  }
  // touch the final byte too, in case bytes is not a page multiple
  p[bytes - 1] = p[bytes - 1];
  return locked;
}
void rt_unlock(void* addr, size_t bytes) {
  if (addr == nullptr || bytes == 0) return;
  VirtualUnlock(addr, bytes);
}
}
