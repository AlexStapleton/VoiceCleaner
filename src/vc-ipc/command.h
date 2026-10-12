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
  // 4 was SetWarmStandby (never wired); retired, decodes as unknown.
  ResetCounters       = 5,
  Shutdown            = 6,
};
// Host-side command (UI -> worker). Richer than the shared POD: carries a device-id string.
struct Command {
  CmdType     type{};
  Params      params      = Params::defaults();
  std::string deviceId;
  double      driftSeedPpm = 0.0;   // drift-controller warm start for the new mic (the UI sends 0 = reset)
};
// Length-checked binary framing: magic "VCC2" + uint32 type + per-type payload. The UI and the
// worker always ship together; bump the magic whenever Params or a payload layout changes.
std::vector<uint8_t> encode(const Command& c);
bool decode(const uint8_t* data, size_t len, Command& out);  // false on malformed/short/unknown
}  // namespace vc::ipc
