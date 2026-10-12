#pragma once
#include <string>
namespace vc::ui {

enum class SliderFmt { Dbfs, Ratio2, Hz, Db };

// Pure slider model: value in [min,max], fraction<->value, clamp, formatted label.
// No pixels — the window maps a click x to a fraction against the track rect and calls setFraction.
class Slider {
 public:
  Slider() = default;
  // Params named lo/hi (not min/max) to avoid the Windows min/max macros when windows.h is in scope.
  Slider(float lo, float hi, float value, SliderFmt fmt) : min_(lo), max_(hi), fmt_(fmt) { setValue(value); }
  float value() const { return value_; }
  float fraction() const;              // (value-min)/(max-min), clamped to 0..1
  void  setFraction(float t);          // value = min + clamp(t,0,1)*(max-min)
  void  setValue(float v);             // clamped to [min,max]
  std::string format() const;          // per fmt (e.g. "-20 dBFS", "0.50", "80 Hz", "-1 dB")

 private:
  float min_ = 0.0f, max_ = 1.0f, value_ = 0.0f;
  SliderFmt fmt_ = SliderFmt::Ratio2;
};

}  // namespace vc::ui
