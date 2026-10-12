#pragma once
#include <array>
#include "params.h"     // vc::ipc::Params
#include "settings.h"   // vc::MicProfile
#include "slider.h"
#include "ui_state.h"

namespace vc::ui {

// The tuning sliders. The window draws, hit-tests, applies, persists and seeds them all from
// kSliderSpecs, so adding a slider means adding one row here (plus its Params/MicProfile field).
enum class SliderId : int {
  TargetLoudness, Hpf, Limiter, Declick, SpeechDeclick, NoiseGate, Strength, MaxBoost,
};
inline constexpr int kSliderCount = 8;

struct SliderSpec {
  SliderId id;
  const wchar_t* caption;                          // drawn above the track
  const wchar_t* help;                             // help-strip text on hover
  float lo, hi, def;                               // range + default (== MicProfile / Params defaults)
  SliderFmt fmt;
  float vc::MicProfile::* profileField;            // persisted per mic in settings.toml
  float vc::ipc::Params::* paramField;             // live value sent to the worker
  vc::ipc::Params (UiState::*apply)(float);        // UiState setter for paramField
};

// Indexed by SliderId.
extern const std::array<SliderSpec, kSliderCount> kSliderSpecs;
inline const SliderSpec& sliderSpec(SliderId id) { return kSliderSpecs[(size_t)id]; }

// Collapsible slider groups, top to bottom; each slider belongs to exactly one.
struct SliderGroup {
  const wchar_t* title;
  float titleWidth;      // px reserved for the title before the header's hairline rule
  int count;
  std::array<SliderId, 4> ids;
};
inline constexpr int kSliderGroupCount = 3;
extern const std::array<SliderGroup, kSliderGroupCount> kSliderGroups;

}  // namespace vc::ui
