#pragma once
#include <cstddef>
namespace vc {
// VirtualLock a region into the working set and pre-touch every page (write one byte per page)
// so the RT threads never take a soft page fault on this memory. Best-effort: returns false if
// VirtualLock fails (e.g. working-set limit) but still pre-touches. Pair with rt_unlock.
bool rt_lock_and_touch(void* addr, size_t bytes);
void rt_unlock(void* addr, size_t bytes);
}
