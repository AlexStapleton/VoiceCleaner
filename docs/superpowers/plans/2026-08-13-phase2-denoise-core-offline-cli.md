# Phase 2 (groundwork) — denoise-core boundary + offline CLI

> subagent-driven-development. Dependency-free foundation for the engines and the §11 regression harness.

**Goal:** Stand up the `denoise-core` library boundary (`INoiseFilter` + a `PassthroughFilter`) and an offline WAV-in/WAV-out CLI over it, so DSP can be validated file-based without a mic (blueprint §2.1, §11). No external deps, no engines yet.

**Architecture:** New `denoise-core` static lib (platform-agnostic, `INoiseFilter` interface + `PassthroughFilter`). WAV I/O added to `transport-core`. New `offline-cli` executable: read WAV → 480-sample frames → `INoiseFilter::ProcessFrame` → write WAV. Later phases add RNNoise / DFN3 filters behind the same interface and a regression corpus.

**Constraint while building:** a Release drift-validation run may be active and holds `phase0-app.exe` locked — **build Debug only, do not build Release, do not run phase0-app.**

---

## Task 1: WAV I/O in transport-core

**Files:** Create `src/transport-core/wav.h`, `wav.cpp`; Test `tests/transport-core/test_wav.cpp` (add to test CMake). Add `wav.cpp` to `src/transport-core/CMakeLists.txt`.

```cpp
// wav.h
#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace vc {
struct WavData {
  uint32_t sampleRate = 48000;
  uint16_t channels   = 1;
  std::vector<float> samples;   // interleaved, float [-1,1]
};
// Reads PCM16 or IEEE-float32 WAV. Returns false on error (missing file, bad header, unsupported).
bool wav_read(const std::string& path, WavData& out);
// Writes a canonical WAV. asFloat=false -> PCM16, true -> float32.
bool wav_write(const std::string& path, const WavData& in, bool asFloat = false);
}
```
- Parse the RIFF/`fmt `/`data` chunks; support `audioFormat` 1 (PCM, 16-bit) and 3 (IEEE float, 32-bit); convert PCM16→float via `/32768.0f`, float32 passthrough. Skip unknown chunks by their size. Write a minimal canonical 44-byte header + data.
- Round-trip test: build a WavData (mono, 48k, a few hundred samples of a ramp/sine), `wav_write` to a temp path (`std::filesystem::temp_directory_path()`), `wav_read` back, assert sampleRate/channels match and samples match within 1/32768 (PCM16) or exactly (float32). Test BOTH asFloat=false and true.

TDD → commit `feat(core): WAV read/write (PCM16 + float32)`.

## Task 2: denoise-core library (INoiseFilter + PassthroughFilter)

**Files:** Create `src/denoise-core/CMakeLists.txt`, `inoise_filter.h`, `passthrough_filter.h`, `passthrough_filter.cpp`; Test `tests/denoise-core/CMakeLists.txt`, `test_passthrough.cpp`. Add `add_subdirectory(src/denoise-core)` and `add_subdirectory(tests/denoise-core)` to the top-level `CMakeLists.txt`.

```cpp
// inoise_filter.h  (blueprint §6.5)
#pragma once
#include <cstdint>
namespace vc {
class INoiseFilter {
public:
  virtual ~INoiseFilter() = default;
  virtual void     ProcessFrame(const float* in, float* out) noexcept = 0;  // exactly 480 in/out
  virtual float    LastVadProbability() const noexcept = 0;
  virtual void     SetAggression(float zeroToOne) noexcept = 0;
  virtual uint32_t AlgorithmicDelaySamples() const noexcept = 0;
};
}
```
```cpp
// passthrough_filter.h
#pragma once
#include "inoise_filter.h"
namespace vc {
// Identity filter: copies input to output. Delay 0. Used to validate the offline harness end-to-end
// and as the null baseline in the regression corpus.
class PassthroughFilter : public INoiseFilter {
public:
  void     ProcessFrame(const float* in, float* out) noexcept override;
  float    LastVadProbability() const noexcept override { return vad_; }
  void     SetAggression(float) noexcept override {}
  uint32_t AlgorithmicDelaySamples() const noexcept override { return 0; }
private:
  float vad_ = 0.0f;
};
}
```
`passthrough_filter.cpp`: `ProcessFrame` copies `FRAME_SAMPLES` floats in→out (include `frame.h` from transport-core; link transport-core for it). Optionally set `vad_` from frame RMS (simple, optional).

`src/denoise-core/CMakeLists.txt`:
```cmake
add_library(denoise-core passthrough_filter.cpp)
target_include_directories(denoise-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denoise-core PUBLIC transport-core)   # for frame.h; denoise-core stays windows.h-free
```
Test `test_passthrough.cpp`: fill a 480 buffer, ProcessFrame, assert output == input; assert `AlgorithmicDelaySamples()==0`.
`tests/denoise-core/CMakeLists.txt`:
```cmake
add_executable(denoise-core-tests test_passthrough.cpp)
target_link_libraries(denoise-core-tests PRIVATE denoise-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(denoise-core-tests)
```
TDD → commit `feat(denoise-core): INoiseFilter interface + passthrough filter`.

## Task 3: offline-cli

**Files:** Create `src/offline-cli/CMakeLists.txt`, `main.cpp`. Add `add_subdirectory(src/offline-cli)` to top-level CMake.

`main.cpp`: `usage: offline-cli <in.wav> <out.wav>`. Read input WAV; extract channel 0 to mono; process in 480-sample frames through a `PassthroughFilter` (zero-pad the final partial frame); write mono output WAV at the same sample rate (PCM16). Print frames processed. Non-zero exit on read/write failure.
```cmake
add_executable(offline-cli main.cpp)
target_link_libraries(offline-cli PRIVATE denoise-core transport-core)
```
- **Manual verify (no external file needed):** the implementer generates a test WAV in code or via a tiny step (e.g. write one with `wav_write`), runs `offline-cli in.wav out.wav`, reads `out.wav` back, and confirms it's sample-identical to the input mono (PassthroughFilter is identity). Paste the frames-processed line and the equality result. Build Debug only.
- Commit `feat(offline-cli): WAV-in/out passthrough over denoise-core`.

## Self-review
- Establishes the §2.1 boundary and §11 offline harness with zero external deps.
- Deferred: real engines (RNNoise/DFN3), STFT/ERB, regression corpus, `--compensate-delay` (stub/omit now).
- `denoise-core` must not include `<windows.h>`.
