// Renders a place's synthesized ambience (src/gta4_island_sound.h) to a WAV
// file: the keys (surf, breeze, gulls, insects) or the atoll (wind, surf, a
// plane passing), for a given number of seconds, and prints the levels of
// each 10-second stretch (RMS and peak) so a change can be checked without
// a console.
//   c++ -std=c++20 -O2 -Wall -Wextra -I../../src render_preview.cpp -o render_preview
//   ./render_preview keys|atoll out.wav [seconds]
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gta4_island_sound.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: render_preview keys|atoll out.wav [seconds]\n");
    return 2;
  }
  const std::string which = argv[1];
  const int character = which == "atoll" ? gta4::island::Ambience::kAtoll : gta4::island::Ambience::kKeys;
  const double seconds = argc > 3 ? std::atof(argv[3]) : 90.0;
  gta4::island::Ambience amb;
  amb.SetTarget(1.0f, character);
  const size_t rate = size_t(gta4::island::Ambience::kRate);
  const size_t frames = size_t(seconds * double(rate));
  std::vector<float> lr(frames * 2);
  constexpr size_t block = 512;
  for (size_t at = 0; at < frames; at += block)
    amb.Render(lr.data() + 2 * at, std::min(block, frames - at));
  // levels per 10 s
  for (size_t s = 0; s < frames; s += 10 * rate) {
    double sum = 0; float peak = 0;
    const size_t end = std::min(frames, s + 10 * rate);
    for (size_t i = s; i < end; ++i) {
      sum += double(lr[2 * i]) * lr[2 * i] + double(lr[2 * i + 1]) * lr[2 * i + 1];
      peak = std::max(peak, std::max(std::fabs(lr[2 * i]), std::fabs(lr[2 * i + 1])));
    }
    const double rms = std::sqrt(sum / double(2 * (end - s)));
    std::printf("%4zu-%4zu s: rms %.3f (%.1f dB) peak %.3f\n", s / rate, end / rate, rms,
                20 * std::log10(std::max(rms, 1e-6)), peak);
  }
  std::vector<int16_t> pcm(lr.size());
  for (size_t i = 0; i < lr.size(); ++i)
    pcm[i] = int16_t(std::lround(std::clamp(lr[i], -1.0f, 1.0f) * 32767.0f));
  FILE* f = std::fopen(argv[2], "wb");
  if (!f) return 1;
  const uint32_t data = uint32_t(pcm.size() * 2), riff = 36 + data, fmt = 16, rate32 = uint32_t(rate),
                 bps = rate32 * 4;
  const uint16_t pcmfmt = 1, ch = 2, align = 4, bits = 16;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcmfmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate32, 4, 1, f); std::fwrite(&bps, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
  std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
  std::printf("wrote %s (%.0f s, %s)\n", argv[2], seconds, which.c_str());
  return 0;
}
