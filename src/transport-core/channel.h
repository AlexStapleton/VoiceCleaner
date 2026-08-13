#pragma once
#include <cstddef>
namespace vc {
void extract_channel(const float* interleaved, size_t frames, int numChannels, int ch, float* out);
void mono_to_interleaved(const float* mono, size_t frames, int numChannels, float* out);
}
