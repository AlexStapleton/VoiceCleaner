#pragma once
#include <cstddef>
#include <vector>
namespace vc {
// DeepFilterNet3 ERB filterbank: partitions the nFreq (=fftSize/2+1) frequency
// bins into nBands contiguous ERB bands. Band widths are computed on the ERB
// scale (libDF erb_fb, lib.rs:68-99) with a min_nb_erb_freqs floor. compress()
// averages each band's bins (libDF band_compr, k = 1/band_size); expand()
// broadcasts each band value back across its bins.
class ErbFilterBank {
public:
  ErbFilterBank(int nFreq = 481, int nBands = 32, float sampleRate = 48000.0f, int minBandFreqs = 2);
  int nBands() const noexcept;
  int nFreq()  const noexcept;
  const std::vector<int>& bandWidths() const noexcept;  // number of freq bins per band (sums to nFreq)
  // Compress a per-bin nonnegative spectrum (e.g. power/magnitude) into nBands band values
  // (mean over the band's bins, matching libDF's normalized filterbank).
  void compress(const float* perBin, float* bands) const noexcept;
  // Expand nBands band values back to per-bin values (each bin gets its band's value).
  void expand(const float* bands, float* perBin) const noexcept;
private:
  int nFreq_, nBands_;
  std::vector<int> widths_;   // per-band bin counts, sum == nFreq_
};
}
