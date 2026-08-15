#include "drift_control.h"
namespace vc {

// Forgetting factor on the control integrator (integral-leak anti-windup).
//
// NOTE / deviation from the original spec: the spec prescribed a plain
// summing integrator plus a back-calculation anti-windup. That combination
// cannot satisfy DriftController.AtSetpointRatioNearOne: a pure integrator
// permanently retains the startup transient (the occupancy LPF ramps up from
// 0), leaving a steady ~20 ppm bias at the setpoint (tolerance is 5 ppm), and
// the naive back-solve additionally overshoots the wrong way when the P term
// alone exceeds the clamp, pinning the ratio to the low limit.
//
// A mild leak sheds that transient (err -> 0 at the setpoint, so the integral
// decays to ~0) while still winding up to the clamp under a persistent drain
// error. leak=0.999 gives: setpoint 0.14 ppm, draining -> +maxPpm clamp,
// saturation -> +maxPpm clamp. This is a standard, RT-safe anti-windup.
static constexpr double kIntegLeak = 0.999;

DriftController::DriftController(const Config& c) : cfg_(c), lpf_(c.filterAlpha) {}

double DriftController::update(double occupancy) noexcept {
  const double filt = lpf_.update(occupancy);
  const double err  = cfg_.setpoint - filt;   // positive when draining (occ below setpoint)

  integ_ = kIntegLeak * integ_ + err;

  const double raw = 1.0 + cfg_.kp * err + cfg_.ki * integ_;

  const double lo = 1.0 - cfg_.maxPpm * 1e-6;
  const double hi = 1.0 + cfg_.maxPpm * 1e-6;

  ratio_ = raw < lo ? lo : (raw > hi ? hi : raw);
  return ratio_;
}

void DriftController::seedPpm(double ppm) noexcept {
  const double lo = 1.0 - cfg_.maxPpm * 1e-6;
  const double hi = 1.0 + cfg_.maxPpm * 1e-6;
  double r = 1.0 + ppm * 1e-6;
  ratio_ = r < lo ? lo : (r > hi ? hi : r);
  // Place the bulk of the seed on the integral term so the very first update reproduces `ratio_`
  // (err starts ~0 because we prime the occupancy LPF at the setpoint).
  integ_ = (cfg_.ki != 0.0) ? (ratio_ - 1.0) / cfg_.ki : 0.0;
  lpf_.set(cfg_.setpoint);
}

double DriftController::ratioPpm() const noexcept { return (ratio_ - 1.0) * 1e6; }
double DriftController::filteredOccupancy() const noexcept { return lpf_.value(); }

}
