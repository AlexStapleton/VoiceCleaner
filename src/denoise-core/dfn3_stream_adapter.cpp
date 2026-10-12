#include "dfn3_stream_adapter.h"
#include <algorithm>
#include <cmath>
#include "dfn3_streaming.h"
namespace vc {

Dfn3StreamAdapter::Dfn3StreamAdapter(const std::string& modelDir, const Options& opt)
    : eng_(std::make_unique<Dfn3Streaming>(modelDir)),
      autoLevel_(opt.autoInputLevel),
      pre_(eng_->latencySamples()) {  // throws std::runtime_error on load failure
  // Self-warm-up (RT-critical): the first W-1 hops fill the internal analysis/GRU
  // windows and cause ORT to allocate persistent input tensors. We run those hops
  // HERE, in the ctor (executed on bridge.start(), off the RT relay thread), so the
  // relay thread never pays that allocation cost.
  //
  // Order: reset() FIRST, then prime with silent hops and DON'T reset again. This
  // leaves the windows full and the OrtValues persistent when real audio arrives.
  // Resetting after priming would clear state and re-trigger the first-hop
  // allocation on real audio, defeating the warm-up. Priming with silence is
  // harmless (silence -> silence), so the small residual state is fine.
  float in[480] = {0.0f};
  float out[480];
  eng_->reset();
  // Warm-up hops go straight to the model (unity gain); the normalizer/pre-gain start fresh afterwards.
  for (int i = 0; i < 16; ++i) eng_->process(in, out);  // >= standard model prime (2 no-encoder warm-up hops + window fill W~6); 16 is a safe margin
}

Dfn3StreamAdapter::~Dfn3StreamAdapter() = default;

void Dfn3StreamAdapter::ProcessFrame(const float* in, float* out) noexcept {
  if (!autoLevel_) { eng_->process(in, out); return; }
  lastGainDb_ = norm_.process(in);
  const float g = std::pow(10.0f, lastGainDb_ * (1.0f / 20.0f));   // once per frame
  pre_.process(in, out, g, [this](const float* a, float* b) noexcept { eng_->process(a, b); });
}

float Dfn3StreamAdapter::LastVadProbability() const noexcept {
  // Map the model's local-SNR-in-dB estimate (lsnr) to a speech probability in [0,1] via a
  // logistic. PROVISIONAL heuristic: 0 dB -> 0.5 is a defensible speech/noise boundary and
  // the slope gives a roughly +/-7 dB transition, but these constants are not calibrated.
  // Proper calibration against a labeled speech/noise corpus is future work (audit finding).
  constexpr float kVadBoundaryDb = 0.0f;  // lsnr at the speech/noise 50% point
  constexpr float kVadSlope = 0.3f;       // logistic slope (~+/-7 dB transition)
  const float lsnr = eng_->lastLsnr();
  return std::clamp(1.0f / (1.0f + std::exp(-kVadSlope * (lsnr - kVadBoundaryDb))), 0.0f, 1.0f);
}

void Dfn3StreamAdapter::SetAggression(float z) noexcept { eng_->setAggression(z); }

uint32_t Dfn3StreamAdapter::AlgorithmicDelaySamples() const noexcept {
  return eng_->latencySamples();
}

uint64_t Dfn3StreamAdapter::failedHops() const noexcept {
  return eng_->failedHops();
}

}  // namespace vc
