#pragma once
#include <atomic>
#include <cstdint>
namespace vc {
class DropoutCounter {
public:
  void record_underrun() noexcept { if (on_.load(std::memory_order_relaxed)) under_.fetch_add(1, std::memory_order_relaxed); }
  void record_overrun()  noexcept { if (on_.load(std::memory_order_relaxed)) over_.fetch_add(1, std::memory_order_relaxed); }
  uint64_t underruns() const noexcept { return under_.load(std::memory_order_relaxed); }
  uint64_t overruns()  const noexcept { return over_.load(std::memory_order_relaxed); }
  void stop_accounting() noexcept { on_.store(false, std::memory_order_relaxed); }
  void reset() noexcept {   // zero counts + re-enable accounting (for a clean A/B measurement window)
    under_.store(0, std::memory_order_relaxed);
    over_.store(0, std::memory_order_relaxed);
    on_.store(true, std::memory_order_relaxed);
  }
private:
  std::atomic<bool> on_{true};
  std::atomic<uint64_t> under_{0}, over_{0};
};
}
