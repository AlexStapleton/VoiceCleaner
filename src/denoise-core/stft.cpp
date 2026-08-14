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

  for (int i = 0; i < p_->bins; ++i) {
    re[i] = p_->freq[i].r;
    im[i] = p_->freq[i].i;
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

  // Apply synthesis window + inverse-FFT normalization (1/N), overlap-add.
  const float* w = p_->window.data();
  const float invN = 1.0f / static_cast<float>(N);
  float* ola = p_->olaBuf.data();
  for (int i = 0; i < N; ++i) ola[i] += (td[i] * invN) * w[i];

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
