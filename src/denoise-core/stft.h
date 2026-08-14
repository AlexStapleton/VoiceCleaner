#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// Parameterized real STFT/ISTFT with DFN3 Vorbis analysis+synthesis windows.
// At 50% overlap (hop == fftSize/2) the Vorbis window satisfies the
// Princen-Bradley condition -> perfect reconstruction.
// The reconstructed signal is delayed by (fftSize - hop) samples vs. input.
//
// kiss_fft types are kept out of this header (pimpl); allocation happens in
// the ctor (init-time). analyze()/synthesize() are alloc-free on the hot path.
class Stft {
public:
  Stft(int fftSize = 960, int hop = 480);
  ~Stft();
  int bins() const noexcept;            // fftSize/2 + 1
  int hop()  const noexcept;
  // Push `hop` new time-domain samples; writes `bins()` complex bins as separate real/imag arrays.
  void analyze(const float* hopSamples, float* re, float* im) noexcept;
  // Overlap-add one spectral frame; writes `hop` time-domain output samples.
  void synthesize(const float* re, const float* im, float* hopOut) noexcept;
  void reset() noexcept;
private:
  struct Impl; Impl* p_;
};
}
