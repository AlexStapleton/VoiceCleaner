#include "switchable_engine.h"
#include <algorithm>
#include <cmath>
namespace vc {
SwitchableEngine::SwitchableEngine(INoiseFilter* a, INoiseFilter* b, int fadeFrames)
    : eng_{a,b}, fadeFrames_(fadeFrames) {
  const uint32_t da = a->AlgorithmicDelaySamples(), db = b->AlgorithmicDelaySamples();
  const uint32_t mx = std::max(da, db);
  delaySamples_[0] = static_cast<int>(mx - da);
  delaySamples_[1] = static_cast<int>(mx - db);
  for (int e=0;e<2;++e) dline_[e].assign(static_cast<size_t>(delaySamples_[e]) + 1, 0.0f);
}
float SwitchableEngine::pushDelay(int e, float x) noexcept {
  if (delaySamples_[e] == 0) return x;
  float y = dline_[e][static_cast<size_t>(dpos_[e])];
  dline_[e][static_cast<size_t>(dpos_[e])] = x;
  dpos_[e] = (dpos_[e] + 1) % static_cast<int>(dline_[e].size());
  return y;
}
uint32_t SwitchableEngine::AlgorithmicDelaySamples() const noexcept {
  return std::max(eng_[0]->AlgorithmicDelaySamples(), eng_[1]->AlgorithmicDelaySamples());
}
float SwitchableEngine::LastVadProbability() const noexcept { return eng_[cur_]->LastVadProbability(); }
void  SwitchableEngine::SetAggression(float z) noexcept { eng_[0]->SetAggression(z); eng_[1]->SetAggression(z); }

void SwitchableEngine::ProcessFrame(const float* in, float* out) noexcept {
  float o0[480], o1[480];
  eng_[0]->ProcessFrame(in, o0);       // BOTH run every frame -> both stay warm
  eng_[1]->ProcessFrame(in, o1);
  const int want = want_.load(std::memory_order_relaxed);
  if (want != cur_ && fadeLeft_ == 0) { from_ = cur_; cur_ = want; fadeLeft_ = fadeFrames_; }

  for (int i=0;i<480;++i) {
    const float d0 = pushDelay(0, o0[i]);
    const float d1 = pushDelay(1, o1[i]);
    const float target = (cur_==0) ? d0 : d1;
    if (fadeLeft_ > 0) {
      const float src = (from_==0) ? d0 : d1;
      // progress p spans 0..1 INCLUSIVE across the fade: frame 0 -> p=0 (all source, so the
      // first fade frame equals the pre-switch value -> continuous); last fade frame -> p=1
      // (all target). fadeFrames_<=1 is an instant switch (p=1) and avoids a divide-by-zero.
      const int frameIndex = fadeFrames_ - fadeLeft_;
      const float p = (fadeFrames_ <= 1)
                          ? 1.0f
                          : static_cast<float>(frameIndex) / static_cast<float>(fadeFrames_ - 1);
      const float th = p * 1.5707963f;   // 0..pi/2
      out[i] = std::cos(th)*src + std::sin(th)*target;
    } else {
      out[i] = target;
    }
  }
  if (fadeLeft_ > 0) --fadeLeft_;
}
}
