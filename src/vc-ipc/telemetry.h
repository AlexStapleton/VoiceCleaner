#pragma once
#include <cstdint>
namespace vc::ipc {
// POD telemetry block, worker -> UI (blueprint §8.4). Written with relaxed atomic stores by the
// worker; the UI tolerates a torn read. `seq` bumps on each aggregate update for torn-read detection.
enum class WorkerState : uint32_t { Starting = 0, Running = 1, DeviceLost = 2, Stopping = 3 };
struct Telemetry {
  // Per-frame (cheap): meter fields.
  float    inputRms;
  float    inputPeak;
  uint32_t vadActive;      // 0 / 1
  // ~1 Hz aggregates.
  float    frameP50Ms;     // engine+chain compute only
  float    frameP99Ms;
  float    frameMaxMs;
  float    hopP50Ms;       // whole relay hop: pop -> params -> engine/chain -> telemetry -> drift/resample -> push
  float    hopP99Ms;
  float    hopMaxMs;
  float    estLatencyMs;   // app-controlled latency (capture+framer+ring+engine+render), computed at start
  uint32_t ringAOcc;       // not yet populated by the worker (deferred)
  uint32_t ringBOcc;
  uint32_t ringBHigh;      // not yet populated by the worker (deferred)
  uint32_t dropouts;
  uint32_t activeEngine;   // 0 = RNNoise, 1 = DFN3
  double   driftRatioPpm;
  uint64_t uptimeMs;       // not yet populated by the worker (deferred)
  uint32_t workerState;    // WorkerState
  uint32_t seq;            // increments each aggregate publish
  uint32_t bypass;         // current bypass state (0/1); lets the UI reflect true bypass on attach
  uint32_t dspChain;       // capability: worker has a DSP chain (0/1); UI disables bypass when 0
  float    agcGainDb;      // current AGC gain (dB) — live readout so the UI can show auto-leveling engaging
};
}  // namespace vc::ipc
