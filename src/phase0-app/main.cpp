#include <cstring>
#include <cstdio>
#include <string>
#include "com_guard.h"
#include "device_enumerator.h"

static const char* kindStr(vc::SampleKind k) {
  switch (k) { case vc::SampleKind::Float32: return "float32";
               case vc::SampleKind::Pcm16: return "pcm16"; default: return "unsupported"; }
}
static void printList(const char* label, EDataFlow flow, vc::DeviceEnumerator& e) {
  std::printf("%s:\n", label);
  for (const auto& d : e.list(flow)) {
    std::printf("  %ls\t%ls\t%uHz\t%uch\t%s\n",
                d.name.c_str(), d.id.c_str(), d.sampleRate, d.channels, kindStr(d.kind));
  }
}
int main(int argc, char** argv) {
  bool listDevices = false;
  for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--list-devices")) listDevices = true;
  if (listDevices) {
    try {
      vc::ComGuard com;
      vc::DeviceEnumerator e;
      printList("RENDER", eRender, e);
      printList("CAPTURE", eCapture, e);
    } catch (const std::exception& ex) { std::fprintf(stderr, "error: %s\n", ex.what()); return 1; }
    return 0;
  }
  std::printf("phase0-app: pass --list-devices to enumerate endpoints. (Full bridge wiring lands in a later task.)\n");
  return 0;
}
