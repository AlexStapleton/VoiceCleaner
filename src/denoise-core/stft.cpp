#include "stft.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "kiss_fftr.h"

namespace vc {

struct Stft::Impl {
  int fftSize;
  int hop;
  int bins;

  kiss_fftr_cfg fwd;   // forward real FFT
  kiss_fftr_cfg inv;   // inverse real FFT

  float wnorm;                  // forward-analysis normalization (libDF lib.rs:133)
  std::vector<float> window;    // Vorbis (DFN3), length fftSize
  std::vector<float> anaBuf;    // analysis buffer, holds latest fftSize samples
  std::vector<float> olaBuf;    // synthesis overlap-add accumulator, length fftSize

  std::vector<float>        td;     // scratch time-domain, length fftSize
  std::vector<kiss_fft_cpx> freq;   // scratch spectrum, length bins
};

Stft::Stft(int fftSize, int hop) : p_(new Impl{}) {
  p_->fftSize = fftSize;
  p_->hop     = hop;
  p_->bins    = fftSize / 2 + 1;

  p_->fwd = kiss_fftr_alloc(fftSize, /*inverse=*/0, nullptr, nullptr);
  p_->inv = kiss_fftr_alloc(fftSize, /*inverse=*/1, nullptr, nullptr);

  // DeepFilterNet3 Vorbis window: w[n] = sin( (pi/2) * sin^2( pi*(n+0.5)/N ) ).
  // Applied on both analysis and synthesis; satisfies the Princen-Bradley
  // condition -> perfect reconstruction at 50% overlap. (libDF lib.rs:126-132)
  p_->window.resize(fftSize);
  const double pi = 3.14159265358979323846;
  for (int n = 0; n < fftSize; ++n) {
    double s = std::sin(pi * (static_cast<double>(n) + 0.5) / fftSize);
    p_->window[n] = static_cast<float>(std::sin(0.5 * pi * s * s));
  }

  // Forward-analysis spectrum normalization (libDF lib.rs:133): applied to the
  // forward FFT output ONLY, so the complex spectrum fed to the DFN3 feature
  // extractor is at libDF's scale. wnorm = 2*hop / fftSize^2. Combined with the
  // (unnormalized) inverse FFT below, the analysis->synthesis round-trip gain is
  // wnorm*fftSize = 2*hop/fftSize = 1 at 50% overlap, preserving reconstruction.
  p_->wnorm = 2.0f * static_cast<float>(hop) /
              (static_cast<float>(fftSize) * static_cast<float>(fftSize));

  p_->anaBuf.assign(fftSize, 0.0f);
  p_->olaBuf.assign(fftSize, 0.0f);
  p_->td.assign(fftSize, 0.0f);
  p_->freq.assign(p_->bins, kiss_fft_cpx{});
}

Stft::~Stft() {
  if (p_) {
    kiss_fftr_free(p_->fwd);
    kiss_fftr_free(p_->inv);
    delete p_;
  }
}

int Stft::bins() const noexcept { return p_->bins; }
int Stft::hop()  const noexcept { return p_->hop; }

void Stft::analyze(const float* hopSamples, float* re, float* im) noexcept {
  const int N = p_->fftSize;
  const int H = p_->hop;

  // Shift analysis buffer left by hop, append the newest hop samples at the end
  // so anaBuf holds the latest N samples.
  float* buf = p_->anaBuf.data();
  for (int i = 0; i < N - H; ++i) buf[i] = buf[i + H];
  for (int i = 0; i < H; ++i)     buf[N - H + i] = hopSamples[i];

  // Apply analysis window into scratch.
  const float* w = p_->window.data();
  float* td = p_->td.data();
  for (int i = 0; i < N; ++i) td[i] = buf[i] * w[i];

  kiss_fftr(p_->fwd, td, p_->freq.data());

  const float wn = p_->wnorm;
  for (int i = 0; i < p_->bins; ++i) {
    re[i] = p_->freq[i].r * wn;
    im[i] = p_->freq[i].i * wn;
  }
}

void Stft::synthesize(const float* re, const float* im, float* hopOut) noexcept {
  const int N = p_->fftSize;
  const int H = p_->hop;

  for (int i = 0; i < p_->bins; ++i) {
    p_->freq[i].r = re[i];
    p_->freq[i].i = im[i];
  }

  float* td = p_->td.data();
  kiss_fftri(p_->inv, p_->freq.data(), td);   // NOT normalized: scaled by N

  // Apply synthesis window + overlap-add. No inverse-FFT 1/N here: the forward
  // analysis already carries wnorm, and wnorm * (unnormalized inverse FFT gain N)
  // = 1 (matches libDF frame_synthesis, which normalizes in analysis only).
  const float* w = p_->window.data();
  float* ola = p_->olaBuf.data();
  for (int i = 0; i < N; ++i) ola[i] += td[i] * w[i];

  // Output first hop samples.
  for (int i = 0; i < H; ++i) hopOut[i] = ola[i];

  // Shift accumulator left by hop, zero-fill the exposed tail.
  for (int i = 0; i < N - H; ++i) ola[i] = ola[i + H];
  for (int i = N - H; i < N; ++i) ola[i] = 0.0f;
}

void Stft::reset() noexcept {
  std::fill(p_->anaBuf.begin(), p_->anaBuf.end(), 0.0f);
  std::fill(p_->olaBuf.begin(), p_->olaBuf.end(), 0.0f);
}

}  // namespace vc
