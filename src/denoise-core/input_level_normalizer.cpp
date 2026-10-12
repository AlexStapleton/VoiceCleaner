#include "input_level_normalizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace vc {
namespace { constexpr size_t kFrame = 480; constexpr float kFrameSec = 0.01f; }

InputLevelNormalizer::InputLevelNormalizer(const Config& c)
    : cfg_(c), ring_(static_cast<size_t>(std::max(1, c.windowFrames)), 0.0f),
      sorted_(static_cast<size_t>(std::max(1, c.windowFrames)), 0.0f) {}

float InputLevelNormalizer::process(const float* frame) noexcept {
  double e = 0.0;
  for (size_t i = 0; i < kFrame; ++i) e += static_cast<double>(frame[i]) * frame[i];
  const float ms = static_cast<float>(e / kFrame);
  const float db = 10.0f * std::log10(ms + 1e-30f);
  const size_t cap = ring_.size();
  if (db >= cfg_.silenceDbfs) {
    if (count_ == cap) {                      // drop the oldest from the sorted copy
      const float old = ring_[head_];
      float* p = std::lower_bound(sorted_.data(), sorted_.data() + count_, old);
      std::memmove(p, p + 1, static_cast<size_t>(sorted_.data() + count_ - p - 1) * sizeof(float));
      --count_;
    }
    float* q = std::upper_bound(sorted_.data(), sorted_.data() + count_, db);
    std::memmove(q + 1, q, static_cast<size_t>(sorted_.data() + count_ - q) * sizeof(float));
    *q = db;
    ++count_;
    ring_[head_] = db;
    head_ = (head_ + 1) % cap;
    // NB: ring_ only advances on non-silent frames, so it always holds the newest `count_` values.
  }
  if (count_ > 0) {
    const size_t idx = static_cast<size_t>(cfg_.percentile * static_cast<float>(count_ - 1) + 0.5f);
    const float level = sorted_[std::min(idx, count_ - 1)];
    const float target = std::clamp(cfg_.targetDbfs - level, 0.0f, cfg_.maxGainDb);
    const bool fast = static_cast<float>(frames_) * kFrameSec < cfg_.fastPhaseSec;
    const float step = (fast ? cfg_.fastSlewDbPerSec : cfg_.slewDbPerSec) * kFrameSec;
    gain_ += std::clamp(target - gain_, -step, step);
  }
  ++frames_;
  return gain_;
}
}
