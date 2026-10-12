#include "slider_table.h"

namespace vc::ui {

using P = vc::ipc::Params;
using M = vc::MicProfile;

const std::array<SliderSpec, kSliderCount> kSliderSpecs = {{
  { SliderId::TargetLoudness, L"TARGET LOUDNESS",
    L"TARGET LOUDNESS \x2014 auto-levels your voice to this level while you speak (an AGC, not a raw volume). It only acts when the VAD dot in the status line is lit. Watch the AGC readout move as you drag.",
    -30.0f, -6.0f, -16.0f, SliderFmt::Dbfs, false, &M::outputGainDbfs, &P::agcTargetDbfs, &UiState::onSetOutputGain },
  { SliderId::Aggression, L"AGGRESSION",
    L"AGGRESSION \x2014 how hard DeepFilterNet3 scrubs noise (DFN3 only). Higher is cleaner but can sound processed.",
    0.0f, 1.0f, 0.0f, SliderFmt::Ratio2, true, &M::aggression, &P::aggression, &UiState::onSetAggression },
  { SliderId::Hpf, L"HPF",
    L"HPF \x2014 high-pass filter. Cuts low rumble, handling thumps and AC hum below the set frequency. Raise to remove more low end.",
    40.0f, 200.0f, 80.0f, SliderFmt::Hz, false, &M::hpfHz, &P::hpfCutoffHz, &UiState::onSetHpf },
  { SliderId::Limiter, L"LIMITER",
    L"LIMITER \x2014 a ceiling that stops loud peaks from clipping. Lower the ceiling for safer, quieter peaks.",
    -6.0f, 0.0f, -1.0f, SliderFmt::Db, false, &M::limiterDbfs, &P::limiterCeilingDbfs, &UiState::onSetLimiter },
  { SliderId::Declick, L"DE-CLICK",
    L"DE-CLICK \x2014 ducks short clicks (keyboard/mouse) in the gaps between words. Raise until clicks fade; too high can soften consonants.",
    0.0f, 1.0f, 0.0f, SliderFmt::Ratio2, false, &M::declickStrength, &P::declickStrength, &UiState::onSetDeclick },
  { SliderId::SpeechDeclick, L"SPEECH DECLICK",
    L"SPEECH DECLICK \x2014 how much de-click applies while you talk. 0 = never touch your voice (safest). Raise only if clicks ride on your speech.",
    0.0f, 1.0f, 0.0f, SliderFmt::Ratio2, false, &M::declickVoiceFloor, &P::declickVoiceFloor, &UiState::onSetDeclickVoiceFloor },
  { SliderId::NoiseGate, L"NOISE GATE",
    L"NOISE GATE \x2014 silences the gaps between words so the amplified noise floor / typing static drops out when you're not speaking. Raise until the gaps go quiet; too high can clip soft word endings.",
    0.0f, 1.0f, 0.0f, SliderFmt::Ratio2, false, &M::gateStrength, &P::gateStrength, &UiState::onSetGate },
  { SliderId::Strength, L"STRENGTH",
    L"STRENGTH \x2014 how hard the denoiser works. Lower blends your natural (un-denoised) voice back in to soften musical-noise / 'laser' artifacts (RNNoise); 100% = full denoise.",
    0.0f, 1.0f, 1.0f, SliderFmt::Ratio2, false, &M::mixStrength, &P::mixStrength, &UiState::onSetMix },
  { SliderId::MaxBoost, L"MAX BOOST",
    L"MAX BOOST \x2014 the most the auto-leveler may raise your voice. If the AGC readout sits at this value while you talk, your mic is quieter than it can make up for: raise MAX BOOST (or your mic level in Windows). Lower it if background noise gets pumped up between words.",
    0.0f, 30.0f, 18.0f, SliderFmt::Db, false, &M::maxBoostDb, &P::agcMaxGainDb, &UiState::onSetMaxBoost },
}};

const std::array<SliderGroup, kSliderGroupCount> kSliderGroups = {{
  { L"DENOISE",     100.0f, 2, { SliderId::Strength, SliderId::Aggression } },
  { L"VOICE LEVEL", 120.0f, 3, { SliderId::TargetLoudness, SliderId::MaxBoost, SliderId::Limiter } },
  { L"CLEANUP",     100.0f, 4, { SliderId::Hpf, SliderId::NoiseGate, SliderId::Declick, SliderId::SpeechDeclick } },
}};

}  // namespace vc::ui
