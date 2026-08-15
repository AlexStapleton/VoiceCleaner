#include "command.h"
#include <cstring>
namespace vc::ipc {
namespace {
constexpr uint8_t kMagic[4] = {'V','C','C','1'};
template <class T> void put(std::vector<uint8_t>& b, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}
template <class T> bool get(const uint8_t*& p, const uint8_t* end, T& v) {
  if (static_cast<size_t>(end - p) < sizeof(T)) return false;
  std::memcpy(&v, p, sizeof(T)); p += sizeof(T); return true;
}
}  // namespace

std::vector<uint8_t> encode(const Command& c) {
  std::vector<uint8_t> b;
  b.insert(b.end(), kMagic, kMagic + 4);
  put(b, static_cast<uint32_t>(c.type));
  switch (c.type) {
    case CmdType::SetParams: put(b, c.params); break;
    case CmdType::SelectCaptureDevice: {
      put(b, c.driftSeedPpm);
      put(b, static_cast<uint32_t>(c.deviceId.size()));
      b.insert(b.end(), c.deviceId.begin(), c.deviceId.end());
      break;
    }
    case CmdType::SelectRenderDevice: {
      put(b, static_cast<uint32_t>(c.deviceId.size()));
      b.insert(b.end(), c.deviceId.begin(), c.deviceId.end());
      break;
    }
    case CmdType::SetWarmStandby: put(b, c.flag); break;
    case CmdType::ResetCounters:
    case CmdType::Shutdown: break;
  }
  return b;
}

bool decode(const uint8_t* data, size_t len, Command& out) {
  if (len < 8) return false;
  if (std::memcmp(data, kMagic, 4) != 0) return false;
  const uint8_t* p = data + 4; const uint8_t* end = data + len;
  uint32_t type = 0; if (!get(p, end, type)) return false;
  out = Command{};
  out.type = static_cast<CmdType>(type);
  switch (out.type) {
    case CmdType::SetParams: return get(p, end, out.params);
    case CmdType::SelectCaptureDevice: {
      uint32_t n = 0;
      if (!get(p, end, out.driftSeedPpm)) return false;
      if (!get(p, end, n)) return false;
      if (static_cast<size_t>(end - p) < n) return false;
      out.deviceId.assign(reinterpret_cast<const char*>(p), n); p += n; return true;
    }
    case CmdType::SelectRenderDevice: {
      uint32_t n = 0;
      if (!get(p, end, n)) return false;
      if (static_cast<size_t>(end - p) < n) return false;
      out.deviceId.assign(reinterpret_cast<const char*>(p), n); p += n; return true;
    }
    case CmdType::SetWarmStandby: return get(p, end, out.flag);
    case CmdType::ResetCounters:
    case CmdType::Shutdown: return true;
    default: return false;
  }
}
}  // namespace vc::ipc
