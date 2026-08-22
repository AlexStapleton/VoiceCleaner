#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Blends an engine's WET (processed) output with the DRY (pre-engine) signal:
//   out[i] = s * wet[i] + (1 - s) * dryDelayed[i]
// The engine delays its output by `engineDelaySamples`, so the dry path is delayed by the same amount
// to keep the mix phase-aligned (no comb filtering). s in [0,1]; s=1 => pure wet (engine unchanged),
// s=0 => pure (delayed) dry. RT-safe: fixed buffer, no allocation in process(). out may alias wet.
class DryWetMix {
 public:
  explicit DryWetMix(size_t engineDelaySamples);
  // dry = pre-engine samples for THIS hop; wet = engine output for this hop (already delayed by the engine).
  void  process(const float* dry, const float* wet, float* out, size_t n, float strength) noexcept;
  void  reset() noexcept;
  size_t delaySamples() const noexcept { return L_; }
 private:
  size_t L_;
  std::vector<float> delay_;   // dry delay line (size L_, or 1 when L_==0 to keep indexing valid)
  size_t widx_ = 0;
};
}  // namespace vc
