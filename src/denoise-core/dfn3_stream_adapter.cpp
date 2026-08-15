#include "dfn3_stream_adapter.h"
#include <algorithm>
#include <cmath>
#include "dfn3_streaming.h"
namespace vc {

Dfn3StreamAdapter::Dfn3StreamAdapter(const std::string& modelDir)
    : eng_(std::make_unique<Dfn3Streaming>(modelDir)) {  // throws std::runtime_error on load failure
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
  for (int i = 0; i < 8; ++i) eng_->process(in, out);  // >= max window W (=6) so all windows fill
}

Dfn3StreamAdapter::~Dfn3StreamAdapter() = default;

void Dfn3StreamAdapter::ProcessFrame(const float* in, float* out) noexcept {
  eng_->process(in, out);
}

float Dfn3StreamAdapter::LastVadProbability() const noexcept {
  // Map local SNR (lsnr, dB-ish) to [0,1] via a logistic. The slope/offset are a
  // rough default; tuning against real VAD is future work.
  const float lsnr = eng_->lastLsnr();
  return std::clamp(1.0f / (1.0f + std::exp(-0.3f * lsnr)), 0.0f, 1.0f);
}

uint32_t Dfn3StreamAdapter::AlgorithmicDelaySamples() const noexcept {
  return eng_->latencySamples();
}

}  // namespace vc
