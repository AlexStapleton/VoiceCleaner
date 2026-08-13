// offline-cli: WAV in -> 480-sample frames -> INoiseFilter -> WAV out.
#include <cstdio>
#include <vector>

#include "channel.h"
#include "frame.h"
#include "passthrough_filter.h"
#include "wav.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: offline-cli <in.wav> <out.wav>\n");
    return 2;
  }

  vc::WavData in;
  if (!vc::wav_read(argv[1], in)) {
    std::fprintf(stderr, "error: failed to read %s\n", argv[1]);
    return 1;
  }

  const size_t frames = in.channels > 0 ? in.samples.size() / in.channels : 0;
  std::vector<float> mono(frames);
  vc::extract_channel(in.samples.data(), frames, in.channels, 0, mono.data());

  std::vector<float> processed(frames);
  vc::PassthroughFilter filter;

  const size_t fullBlocks = frames / vc::FRAME_SAMPLES;
  size_t blocksProcessed = 0;

  for (size_t b = 0; b < fullBlocks; ++b) {
    const size_t offset = b * vc::FRAME_SAMPLES;
    filter.ProcessFrame(mono.data() + offset, processed.data() + offset);
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
    filter.ProcessFrame(padIn.data(), padOut.data());
    for (size_t i = 0; i < remainder; ++i) {
      processed[offset + i] = padOut[i];
    }
    ++blocksProcessed;
  }

  vc::WavData out;
  out.sampleRate = in.sampleRate;
  out.channels = 1;
  out.samples = processed;

  if (!vc::wav_write(argv[2], out, /*asFloat=*/false)) {
    std::fprintf(stderr, "error: failed to write %s\n", argv[2]);
    return 1;
  }

  std::printf("processed %zu frames (%zu samples) at %u Hz -> %s\n",
              blocksProcessed, frames, out.sampleRate, argv[2]);
  return 0;
}
