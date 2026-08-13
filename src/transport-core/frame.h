#pragma once
#include <array>
#include <cstddef>
namespace vc {
inline constexpr size_t FRAME_SAMPLES = 480;   // 10 ms @ 48 kHz, mono
using Frame = std::array<float, FRAME_SAMPLES>;
}
