#include "channel.h"
namespace vc {
void extract_channel(const float* in, size_t frames, int nc, int ch, float* out) {
  for (size_t i = 0; i < frames; ++i) out[i] = in[i * nc + ch];
}
void mono_to_interleaved(const float* mono, size_t frames, int nc, float* out) {
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < nc; ++c) out[i * nc + c] = mono[i];
}
}
