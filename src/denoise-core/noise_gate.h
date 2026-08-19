#pragma once
#include <cstddef>
namespace vc {
// Voice-safe downward noise gate. Keyed on the chain's speech decision (`open`): when speech is
// active the gate passes at unity; during the gaps it attenuates toward a floor so the AGC's
// amplified noise floor / typing residue drops to (near) silence. `strength` in [0,1] scales the
// attenuation depth (0 = bypass). Fast attack (open) so speech onsets aren't clipped; slower
// release (close) so gaps fade smoothly. RT-safe: no allocation in process(). out may alias in.
class NoiseGate {
 public:
  struct Config {
    float fs = 48000.0f;
    float attackMs = 5.0f;     // gain rise (opening) — fast so speech onset isn't cut
    float releaseMs = 80.0f;   // gain fall (closing) — smooth fade into the gap
    float floorDb = -45.0f;    // attenuation at strength = 1 when fully closed
  };
  explicit NoiseGate(const Config& c);
  void  process(const float* in, float* out, size_t n, bool open, float strength) noexcept;
  void  reset() noexcept;
  float currentGain() const noexcept { return gain_; }
 private:
  float atkCoef_, relCoef_, floorDb_;
  float gain_ = 1.0f;
};
}  // namespace vc
