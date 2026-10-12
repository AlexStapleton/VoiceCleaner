#pragma once
#include <cstdint>
#include "frame.h"   // FRAME_SAMPLES
namespace vc {

// Inputs to the app-controlled latency budget. Every value is known at start() from the queried
// WASAPI buffer sizes and the runtime config — no hardware loopback needed, so the worker reports a
// real per-device figure for the current setup instead of a hand-estimate.
struct LatencyInputs {
  uint32_t captureBufferFrames = 0;   // WASAPI shared capture buffer (IAudioClient::GetBufferSize)
  uint32_t renderBufferFrames  = 0;   // WASAPI shared render buffer (pre-rolled full of silence at start)
  uint32_t ringPrimedFrames    = 0;   // Ring B frames primed at start (capacity/2), each FRAME_SAMPLES long
  uint32_t engineDelaySamples  = 0;   // engine algorithmic delay (960 = RNNoise native / low-latency, 1440 = default-mode pin shared by RNNoise and DFN3)
  double   sampleRate          = 48000.0;
};

// Per-stage breakdown, milliseconds. This is the latency VoiceCleaner itself adds (buffering +
// algorithmic). It EXCLUDES analog capture, driver, and downstream-app latency, which live outside
// the process and which the app does not control — but those parts are small and fixed, whereas the
// stages below are the dominant, tunable ones (ring priming especially).
struct LatencyBreakdown {
  double captureMs = 0, framerMs = 0, ringMs = 0, engineMs = 0, renderMs = 0, totalMs = 0;
};

inline LatencyBreakdown compute_latency(const LatencyInputs& in) noexcept {
  const double sr = in.sampleRate > 0.0 ? in.sampleRate : 48000.0;
  const double frameMs = static_cast<double>(FRAME_SAMPLES) / sr * 1000.0;
  LatencyBreakdown b;
  b.captureMs = static_cast<double>(in.captureBufferFrames) / sr * 1000.0;
  b.framerMs  = frameMs;                                       // up to one hop accumulates before a frame emits
  b.ringMs    = static_cast<double>(in.ringPrimedFrames) * frameMs;   // Ring B primed depth (the dominant chunk)
  b.engineMs  = static_cast<double>(in.engineDelaySamples) / sr * 1000.0;
  b.renderMs  = static_cast<double>(in.renderBufferFrames) / sr * 1000.0;
  b.totalMs   = b.captureMs + b.framerMs + b.ringMs + b.engineMs + b.renderMs;
  return b;
}
}  // namespace vc
