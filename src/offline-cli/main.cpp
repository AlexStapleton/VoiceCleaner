// offline-cli: WAV in -> 480-sample frames -> INoiseFilter (or DspChain) -> WAV out.
#include <cstdio>
#include <cstring>
#include <vector>

#include "channel.h"
#include "dsp_chain.h"
#include "frame.h"
#include "passthrough_filter.h"
#include "wav.h"

int main(int argc, char** argv) {
  bool useChain = false;
  int argi = 1;
  if (argc >= 2 && std::strcmp(argv[1], "--chain") == 0) {
    useChain = true;
    ++argi;
  }

  if (argc - argi != 2) {
    std::fprintf(stderr, "usage: offline-cli [--chain] <in.wav> <out.wav>\n");
    return 2;
  }

  const char* inPath = argv[argi];
  const char* outPath = argv[argi + 1];

  vc::WavData in;
  if (!vc::wav_read(inPath, in)) {
    std::fprintf(stderr, "error: failed to read %s\n", inPath);
    return 1;
  }

  const size_t frames = in.channels > 0 ? in.samples.size() / in.channels : 0;
  std::vector<float> mono(frames);
  vc::extract_channel(in.samples.data(), frames, in.channels, 0, mono.data());

  std::vector<float> processed(frames);
  vc::PassthroughFilter filter;
  vc::DspChain chain(&filter, vc::DspChain::Config{});

  const size_t fullBlocks = frames / vc::FRAME_SAMPLES;
  size_t blocksProcessed = 0;

  auto runBlock = [&](const float* src, float* dst) {
    if (useChain)
      chain.process_frame(src, dst);
    else
      filter.ProcessFrame(src, dst);
  };

  for (size_t b = 0; b < fullBlocks; ++b) {
    const size_t offset = b * vc::FRAME_SAMPLES;
    runBlock(mono.data() + offset, processed.data() + offset);
    ++blocksProcessed;
  }

  const size_t remainder = frames - fullBlocks * vc::FRAME_SAMPLES;
  if (remainder > 0) {
    vc::Frame padIn{};
    vc::Frame padOut{};
    const size_t offset = fullBlocks * vc::FRAME_SAMPLES;
    for (size_t i = 0; i < remainder; ++i) {
      padIn[i] = mono[offset + i];
    }
    runBlock(padIn.data(), padOut.data());
    for (size_t i = 0; i < remainder; ++i) {
      processed[offset + i] = padOut[i];
    }
    ++blocksProcessed;
  }

  vc::WavData out;
  out.sampleRate = in.sampleRate;
  out.channels = 1;
  out.samples = processed;

  if (!vc::wav_write(outPath, out, /*asFloat=*/false)) {
    std::fprintf(stderr, "error: failed to write %s\n", outPath);
    return 1;
  }

  std::printf("chain=%s: processed %zu frames (%zu samples) at %u Hz -> %s\n",
              useChain ? "on" : "off", blocksProcessed, frames, out.sampleRate,
              outPath);
  return 0;
}
