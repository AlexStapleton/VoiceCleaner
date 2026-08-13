#pragma once
namespace vc {
class LeakyIntegrator {
public:
  explicit LeakyIntegrator(double alpha) : a_(alpha) {}
  double update(double x) noexcept { y_ += a_ * (x - y_); return y_; }
  double value() const noexcept { return y_; }
  void   set(double v) noexcept { y_ = v; }
private:
  double a_; double y_ = 0.0;
};

class DriftController {
public:
  struct Config { double setpoint; double kp; double ki; double maxPpm; double filterAlpha; };
  explicit DriftController(const Config& c);
  double update(double occupancy) noexcept;   // returns ratio (output/input) to apply
  double ratioPpm() const noexcept;            // (ratio-1)*1e6
  double filteredOccupancy() const noexcept;
private:
  Config cfg_; LeakyIntegrator lpf_; double integ_ = 0.0; double ratio_ = 1.0;
};
}
