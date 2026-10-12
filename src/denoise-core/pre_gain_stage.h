#pragma once
#include <array>
#include <cstddef>
#include "delay_line.h"
#include "frame.h"
namespace vc {
// Applies a per-sample-ramped input gain around a block "model" of known delay D and undoes it
// exactly on the output: out[i] = modelOut[i] / g(i - D), with g taken from a D-sample per-sample
// gain delay line. For a linear-in-amplitude model the result is the unity-gain output.
// No allocation after construction; no transcendental calls (division per sample only).
class PreGainStage {
public:
  explicit PreGainStage(size_t modelDelaySamples) : gainDelay_(modelDelaySamples, 1.0f) {}
  // `gain` is the linear gain for the END of this frame; the start is the previous frame's.
  // model(const float* in480, float* out480) is called once.
  template <class Model>
  void process(const float* in, float* out, float gain, Model&& model) noexcept {
    const float g0 = prev_, step = (gain - prev_) / static_cast<float>(FRAME_SAMPLES);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
      const float g = g0 + step * static_cast<float>(i + 1);
      gains_[i] = g;
      buf_[i] = in[i] * g;
    }
    prev_ = gain;
    model(buf_.data(), out);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) out[i] /= gainDelay_.push(gains_[i]);
  }
private:
  DelayLine gainDelay_;
  std::array<float, FRAME_SAMPLES> buf_{}, gains_{};
  float prev_ = 1.0f;
};
}
