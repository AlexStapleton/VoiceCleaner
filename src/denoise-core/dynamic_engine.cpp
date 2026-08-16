#include "dynamic_engine.h"
#include <algorithm>
#include <cmath>
namespace vc {

DynamicEngine::DynamicEngine(Loader loader, const Config& cfg)
    : loader_(std::move(loader)), mode_(cfg.mode), fadeFrames_(cfg.fadeFrames) {
  maxDelay_ = std::max(cfg.delaySamples[0], cfg.delaySamples[1]);
  for (int i = 0; i < 2; ++i) {
    alignDelay_[i] = static_cast<int>(maxDelay_ - cfg.delaySamples[i]);
    dline_[i].assign(static_cast<size_t>(alignDelay_[i]) + 1, 0.0f);
  }
  const int init = clamp01(cfg.initialActive);
  cur_.store(init, std::memory_order_relaxed); from_ = init;
  want_.store(init, std::memory_order_relaxed);
  // Build the initial resident leaf(s). Warm: both; Lean: only the active. If any install throws
  // (e.g. model load failure), free any already-published slot before propagating — the destructor
  // does NOT run for a ctor that throws.
  try {
    if (mode_ == Mode::Warm) { installLeaf(0); installLeaf(1); }
    else                     { installLeaf(init); }
  } catch (...) {
    for (int i = 0; i < 2; ++i) delete eng_[i].exchange(nullptr, std::memory_order_acq_rel);
    throw;
  }
}

DynamicEngine::~DynamicEngine() {
  for (int i = 0; i < 2; ++i) delete eng_[i].exchange(nullptr, std::memory_order_acq_rel);
  Retired r;
  while (retiredQ_.try_pop(r)) delete r.ptr;
}

float DynamicEngine::pushDelay(int slot, float x) noexcept {
  if (alignDelay_[slot] == 0) return x;
  float y = dline_[slot][static_cast<size_t>(dpos_[slot])];
  dline_[slot][static_cast<size_t>(dpos_[slot])] = x;
  dpos_[slot] = (dpos_[slot] + 1) % static_cast<int>(dline_[slot].size());
  return y;
}

float DynamicEngine::LastVadProbability() const noexcept {
  INoiseFilter* e = eng_[cur_.load(std::memory_order_relaxed)].load(std::memory_order_acquire);
  return e ? e->LastVadProbability() : 0.0f;
}

void DynamicEngine::SetAggression(float z) noexcept {
  lastAggression_.store(z, std::memory_order_relaxed);
  for (int i = 0; i < 2; ++i) {
    INoiseFilter* e = eng_[i].load(std::memory_order_acquire);
    if (e) e->SetAggression(z);
  }
}

void DynamicEngine::installLeaf(int idx) {
  std::unique_ptr<INoiseFilter> leaf = loader_(idx);                 // may throw -> caller aborts, no state mutated
  leaf->SetAggression(lastAggression_.load(std::memory_order_relaxed));
  std::fill(dline_[idx].begin(), dline_[idx].end(), 0.0f);           // fresh alignment buffer
  dpos_[idx] = 0;
  eng_[idx].store(leaf.release(), std::memory_order_release);        // publish (all writes above are ordered-before)
}

void DynamicEngine::ProcessFrame(const float* in, float* out) noexcept {
  const int want = want_.load(std::memory_order_acquire);
  int cur = cur_.load(std::memory_order_relaxed);
  if (want != cur && fadeLeft_ == 0) { from_ = cur; cur = want; cur_.store(cur, std::memory_order_relaxed); fadeLeft_ = fadeFrames_; }

  INoiseFilter* e0 = eng_[0].load(std::memory_order_acquire);
  INoiseFilter* e1 = eng_[1].load(std::memory_order_acquire);
  const bool fading  = fadeLeft_ > 0;
  const bool runBoth = fading || mode_ == Mode::Warm;

  if (runBoth) {
    float o0[FRAME_SAMPLES], o1[FRAME_SAMPLES];
    if (e0) e0->ProcessFrame(in, o0); else std::fill(o0, o0 + FRAME_SAMPLES, 0.0f);
    if (e1) e1->ProcessFrame(in, o1); else std::fill(o1, o1 + FRAME_SAMPLES, 0.0f);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
      const float d0 = pushDelay(0, o0[i]);
      const float d1 = pushDelay(1, o1[i]);
      const float target = (cur == 0) ? d0 : d1;
      if (fadeLeft_ > 0) {
        const int frameIndex = fadeFrames_ - fadeLeft_;
        const float p = (fadeFrames_ <= 1) ? 1.0f
                        : static_cast<float>(frameIndex) / static_cast<float>(fadeFrames_ - 1);
        const float th = p * 1.5707963f;                 // 0..pi/2
        const float src = (from_ == 0) ? d0 : d1;
        out[i] = std::cos(th) * src + std::sin(th) * target;
      } else {
        out[i] = target;
      }
    }
  } else {
    // Lean, not fading: run ONLY the active leaf; keep its delay line advancing.
    INoiseFilter* ec = (cur == 0) ? e0 : e1;
    float tmp[FRAME_SAMPLES];
    if (ec) ec->ProcessFrame(in, tmp); else std::fill(tmp, tmp + FRAME_SAMPLES, 0.0f);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] = pushDelay(cur, tmp[i]);
  }

  if (fadeLeft_ > 0 && --fadeLeft_ == 0 && mode_ == Mode::Lean) {
    // Retire the outgoing leaf UNLESS a switch back to it arrived mid-fade (RT-local decision -> no race).
    if (want_.load(std::memory_order_acquire) != from_) {
      INoiseFilter* p = eng_[from_].exchange(nullptr, std::memory_order_acq_rel);
      if (p && !retiredQ_.try_push(Retired{p, from_}))
        eng_[from_].store(p, std::memory_order_release);   // queue full (should never happen): keep resident, no leak
    }
  }
}

void DynamicEngine::requestActive(int idx) {
  idx = clamp01(idx);
  if (eng_[idx].load(std::memory_order_acquire) == nullptr) installLeaf(idx);  // may throw -> want_ untouched
  want_.store(idx, std::memory_order_release);
}

void DynamicEngine::reclaim(const std::function<void(INoiseFilter*, int)>& onRetire) noexcept {
  Retired r;
  while (retiredQ_.try_pop(r)) {
    if (onRetire) onRetire(r.ptr, r.idx);
    delete r.ptr;
  }
}

}  // namespace vc
