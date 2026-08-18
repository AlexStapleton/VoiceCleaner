#include "slider.h"
#include <cstdio>
namespace vc::ui {

static float clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

float Slider::fraction() const {
  if (max_ <= min_) return 0.0f;
  return clamp01((value_ - min_) / (max_ - min_));
}
void Slider::setFraction(float t) { value_ = min_ + clamp01(t) * (max_ - min_); }
void Slider::setValue(float v) {
  value_ = v < min_ ? min_ : (v > max_ ? max_ : v);
}
std::string Slider::format() const {
  char buf[32];
  switch (fmt_) {
    case SliderFmt::Dbfs:   std::snprintf(buf, sizeof(buf), "%.0f dBFS", value_); break;
    case SliderFmt::Ratio2: std::snprintf(buf, sizeof(buf), "%.2f", value_); break;
    case SliderFmt::Hz:     std::snprintf(buf, sizeof(buf), "%.0f Hz", value_); break;
    case SliderFmt::Db:     std::snprintf(buf, sizeof(buf), "%.0f dB", value_); break;
  }
  return buf;
}

}  // namespace vc::ui
