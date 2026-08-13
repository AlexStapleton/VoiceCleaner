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
    const size_t h = head_.idx.load(std::memory_order_acquire);
    const size_t t = tail_.idx.load(std::memory_order_acquire);
    return (h + cap_ - t) % cap_;
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
