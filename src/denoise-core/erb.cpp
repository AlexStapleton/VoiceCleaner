#include "erb.h"

#include <cmath>

namespace vc {

namespace {
// libDF freq<->ERB scale conversions (lib.rs:42-47).
inline float freq2erb(float freqHz) {
  return 9.265f * std::log1p(freqHz / (24.7f * 9.265f));
}
inline float erb2freq(float nErb) {
  return 24.7f * 9.265f * (std::exp(nErb / 9.265f) - 1.0f);
}
}  // namespace

ErbFilterBank::ErbFilterBank(int nFreq, int nBands, float sampleRate, int minBandFreqs)
    : nFreq_(nFreq), nBands_(nBands), widths_(nBands, 0) {
  // Replicates libDF erb_fb (lib.rs:68-99). nFreq is expected to be
  // fftSize/2 + 1; sampleRate/fftSize gives the per-bin frequency width.
  const int fftSize = (nFreq - 1) * 2;
  const float nyqFreq = sampleRate / 2.0f;
  const float freqWidth = sampleRate / static_cast<float>(fftSize);
  const float erbLow = freq2erb(0.0f);
  const float erbHigh = freq2erb(nyqFreq);
  const float step = (erbHigh - erbLow) / static_cast<float>(nBands);

  int prevFreq = 0;   // last freq bin of the previous ERB band
  int freqOver = 0;   // number of freq bins already borrowed by previous bands
  for (int i = 1; i <= nBands; ++i) {
    const float f = erb2freq(erbLow + static_cast<float>(i) * step);
    const int fb = static_cast<int>(std::round(f / freqWidth));
    int nbFreqs = fb - prevFreq - freqOver;
    if (nbFreqs < minBandFreqs) {
      freqOver = minBandFreqs - nbFreqs;  // track enforced bins
      nbFreqs = minBandFreqs;             // enforce the floor
    } else {
      freqOver = 0;
    }
    widths_[i - 1] = nbFreqs;
    prevFreq = fb;
  }
  widths_[nBands - 1] += 1;  // account for the +1 (Nyquist) bin in fftSize/2+1

  int total = 0;
  for (int w : widths_) total += w;
  const int tooLarge = total - nFreq_;
  if (tooLarge > 0) widths_[nBands - 1] -= tooLarge;
}

int ErbFilterBank::nBands() const noexcept { return nBands_; }
int ErbFilterBank::nFreq()  const noexcept { return nFreq_; }
const std::vector<int>& ErbFilterBank::bandWidths() const noexcept { return widths_; }

void ErbFilterBank::compress(const float* perBin, float* bands) const noexcept {
  int base = 0;
  for (int b = 0; b < nBands_; ++b) {
    const int w = widths_[b];
    float sum = 0.0f;
    for (int j = 0; j < w; ++j) sum += perBin[base + j];
    bands[b] = (w > 0) ? sum / static_cast<float>(w) : 0.0f;
    base += w;
  }
}

void ErbFilterBank::expand(const float* bands, float* perBin) const noexcept {
  int base = 0;
  for (int b = 0; b < nBands_; ++b) {
    const int w = widths_[b];
    for (int j = 0; j < w; ++j) perBin[base + j] = bands[b];
    base += w;
  }
}

}  // namespace vc
