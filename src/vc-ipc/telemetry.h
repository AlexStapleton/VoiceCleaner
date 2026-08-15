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
  float    frameP50Ms;
  float    frameP99Ms;
  float    frameMaxMs;
  uint32_t ringAOcc;
  uint32_t ringBOcc;
  uint32_t ringBHigh;
  uint32_t dropouts;
  uint32_t activeEngine;   // 0 = RNNoise, 1 = DFN3
  double   driftRatioPpm;
  uint64_t uptimeMs;
  uint32_t workerState;    // WorkerState
  uint32_t seq;            // increments each aggregate publish
};
}  // namespace vc::ipc
