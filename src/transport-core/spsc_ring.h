#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <vector>

namespace vc {
template <typename T>
class SpscRing {
public:
  explicit SpscRing(size_t capacity)
      : slots_(capacity + 1), cap_(capacity + 1) {}

  bool try_push(const T& item) noexcept {
    const size_t h = head_.idx.load(std::memory_order_relaxed);
    const size_t n = (h + 1) % cap_;
    if (n == tail_.idx.load(std::memory_order_acquire)) return false; // full
    slots_[h] = item;
    head_.idx.store(n, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const size_t t = tail_.idx.load(std::memory_order_relaxed);
    if (t == head_.idx.load(std::memory_order_acquire)) return false; // empty
    out = slots_[t];
    tail_.idx.store((t + 1) % cap_, std::memory_order_release);
    return true;
  }

  size_t size() const noexcept {
    // Consistent occupancy for a third-party observer (e.g. a telemetry thread). Pin head across
    // the tail read: if head advances between the two loads, the modulo can otherwise wrap to a
    // bogus near-capacity value. Retry until head is stable; converges in one iteration in practice.
    for (int attempt = 0; attempt < 4; ++attempt) {
      const size_t h1 = head_.idx.load(std::memory_order_acquire);
      const size_t t  = tail_.idx.load(std::memory_order_acquire);
      const size_t h2 = head_.idx.load(std::memory_order_acquire);
      if (h1 == h2) return (h1 + cap_ - t) % cap_;
    }
    // Fallback (head churning improbably fast): bounded, never a wrapped-huge value.
    const size_t h = head_.idx.load(std::memory_order_acquire);
    const size_t t = tail_.idx.load(std::memory_order_acquire);
    const size_t s = (h + cap_ - t) % cap_;
    return s <= (cap_ - 1) ? s : 0;
  }
  // Discard all queued items. NOT thread-safe: call only while neither the producer nor the
  // consumer is running (e.g. between Bridge::stop() and start(), with all threads joined).
  void clear() noexcept {
    head_.idx.store(0, std::memory_order_relaxed);
    tail_.idx.store(0, std::memory_order_relaxed);
  }
  size_t capacity() const noexcept { return cap_ - 1; }
  T* data() noexcept { return slots_.data(); }
  size_t byte_size() const noexcept { return slots_.size() * sizeof(T); }

private:
  struct alignas(128) Index { std::atomic<size_t> idx{0}; };
  std::vector<T> slots_;
  size_t cap_;
  Index head_;
  Index tail_;
};
}  // namespace vc
