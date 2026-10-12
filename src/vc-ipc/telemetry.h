#pragma once
#include <cstdint>
namespace vc::ipc {
// POD telemetry block, worker -> UI, published ~1 Hz into shared memory (a plain memcpy). The UI
// tolerates a torn read; `seq` bumps on each publish. The UI reads seq/bypass/dspChain/
// vadActive/agcGainDb; the rest are diagnostics for external readers and future views.
enum class WorkerState : uint32_t { Starting = 0, Running = 1, DeviceLost = 2, Stopping = 3 };
struct Telemetry {
  // Input meter over the publish window.
  float    inputRms;
  float    inputPeak;
  uint32_t vadActive;      // 0 / 1
  // Window aggregates.
  float    frameP50Ms;     // engine+chain compute only
  float    frameP99Ms;
  float    frameMaxMs;
  float    hopP50Ms;       // whole relay hop: pop -> params -> engine/chain -> telemetry -> drift/resample -> push
  float    hopP99Ms;
  float    hopMaxMs;
  float    estLatencyMs;   // app-controlled latency (capture+framer+ring+engine+render), computed at start
  uint32_t ringAOcc;
  uint32_t ringBOcc;
  uint32_t ringBHigh;      // high-water since start / last ResetCounters
  uint32_t dropouts;
  double   driftRatioPpm;
  uint64_t uptimeMs;       // since the bridge (re)started
  uint32_t workerState;    // WorkerState
  uint32_t seq;            // increments each aggregate publish
  uint32_t bypass;         // current bypass state (0/1); lets the UI reflect true bypass on attach
  uint32_t dspChain;       // capability: worker has a DSP chain (0/1); UI disables bypass when 0
  float    agcGainDb;      // current AGC gain (dB) — live readout so the UI can show auto-leveling engaging
};
}  // namespace vc::ipc
