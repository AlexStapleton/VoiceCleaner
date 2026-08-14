#pragma once
#include <cstddef>
namespace vc {
class Agc {
public:
  struct Config { float targetRmsDb=-15.0f; float attackMs=50.0f; float releaseMs=500.0f;
                  float maxGainDb=30.0f; float minGainDb=-10.0f; float fs=48000.0f; };
  explicit Agc(const Config& c);
  // Process n samples. When vadActive is false, the gain is FROZEN (no adaptation) — prevents
  // hunting up during pauses (blueprint §7.1). out may alias in.
  void  process(const float* in, float* out, size_t n, bool vadActive) noexcept;
  float currentGainDb() const noexcept;
  void  reset() noexcept;
private:
  Config cfg_; float gainDb_ = 0.0f; float envRms_ = 0.0f;
  float atkCoef_, relCoef_, targetLin_, maxG_, minG_;
};
}
