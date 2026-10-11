#pragma once
#include <cstddef>
#include <vector>
#include "frame.h"
namespace vc {

// Fixed-capacity single-threaded float FIFO (circular). Preallocated at construction so
// write/read never allocate — this is the RT-safety point: it replaces the std::deque that
// used to churn heap block-nodes on the capture/render/relay audio threads.
//
// Capacity is sized far above the real peak occupancy. These buffers only ever hold a
// sub-frame remainder (< 480 samples) plus at most one in-flight WASAPI packet / resampler
// output — a few hundred samples in steady state, since every hop drains them back down.
// The default (16384 ≈ 341 ms @ 48 kHz, ~34 frames) is >10x any realistic burst yet small
// enough (64 KiB) that the owner can VirtualLock all of them within the process working-set
// quota. Overflow therefore cannot happen under any real condition (drift compensation holds
// ring occupancy flat, so there is no sustained overproduction into the FIFO); the guard drops
// the newest samples rather than allocate, keeping the structure hard-bounded instead of
// growing latency without limit like the deque.
class FloatFifo {
public:
  explicit FloatFifo(size_t capacity = 16384)          // ≈341 ms @ 48 kHz; 64 KiB, lock-friendly
      : buf_(capacity + 1, 0.0f), cap_(capacity + 1) {}  // +1 slot so full is distinguishable from empty

  size_t buffered() const noexcept { return (head_ + cap_ - tail_) % cap_; }
  size_t space()    const noexcept { return cap_ - 1 - buffered(); }

  void write(const float* in, size_t n) noexcept {
    if (n > space()) n = space();                        // overflow guard (unreachable in practice): drop newest
    for (size_t i = 0; i < n; ++i) {
      buf_[head_] = in[i];
      head_ = (head_ + 1 == cap_) ? 0 : head_ + 1;
    }
  }

  // Copy up to n samples out, advancing the read cursor. Returns the count actually copied
  // (< n on underrun).
  size_t read(float* out, size_t n) noexcept {
    const size_t avail = buffered();
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; ++i) {
      out[i] = buf_[tail_];
      tail_ = (tail_ + 1 == cap_) ? 0 : tail_ + 1;
    }
    return n;
  }

  void clear() noexcept { head_ = tail_ = 0; }   // discard buffered samples (owner thread only)

  // Backing storage, for the owner to VirtualLock into the working set (the buffer is fixed at
  // construction and never reallocates, so this region is stable for the FIFO's lifetime).
  float*  data() noexcept      { return buf_.data(); }
  size_t  byte_size() const noexcept { return buf_.size() * sizeof(float); }

private:
  std::vector<float> buf_;
  size_t cap_;
  size_t head_ = 0;   // write index
  size_t tail_ = 0;   // read index
};

// Accumulates arbitrary-length writes and hands back fixed 480-sample frames.
class FrameAccumulator {
public:
  void write(const float* in, size_t n);
  bool pop_frame(float* out480);   // true if a full frame was written
  size_t buffered() const noexcept { return fifo_.buffered(); }
  void clear() noexcept { fifo_.clear(); }
  float* data() noexcept { return fifo_.data(); }              // lockable region (see FloatFifo::data)
  size_t byte_size() const noexcept { return fifo_.byte_size(); }
private:
  FloatFifo fifo_;
};

// Buffers whole 480-sample frames and serves arbitrary-length reads (short on underrun).
class FrameDistributor {
public:
  void push_frame(const float* in480);
  size_t read(float* out, size_t n);   // returns samples written (< n on underrun)
  size_t buffered() const noexcept { return fifo_.buffered(); }
  void clear() noexcept { fifo_.clear(); }
  float* data() noexcept { return fifo_.data(); }              // lockable region (see FloatFifo::data)
  size_t byte_size() const noexcept { return fifo_.byte_size(); }
private:
  FloatFifo fifo_;
};
}
