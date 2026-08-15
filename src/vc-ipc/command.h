#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "params.h"
namespace vc::ipc {
enum class CmdType : uint32_t {
  SetParams           = 1,   // uses `params`
  SelectCaptureDevice = 2,   // uses `deviceId` + `driftSeedPpm`
  SelectRenderDevice  = 3,   // uses `deviceId`
  SetWarmStandby      = 4,   // uses `flag` (0/1)
  ResetCounters       = 5,
  Shutdown            = 6,
};
// Host-side command (UI -> worker). Richer than the shared POD: carries a device-id string.
struct Command {
  CmdType     type{};
  Params      params      = Params::defaults();
  std::string deviceId;
  double      driftSeedPpm = 0.0;
  uint32_t    flag         = 0;
};
// Length-checked binary framing: magic "VCC1" + uint32 type + per-type payload.
std::vector<uint8_t> encode(const Command& c);
bool decode(const uint8_t* data, size_t len, Command& out);  // false on malformed/short/unknown
}  // namespace vc::ipc
