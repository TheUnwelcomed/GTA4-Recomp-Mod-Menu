// Renders the F-16's synthesized engine (src/gta4_jet_engine.h) to a WAV
// file, through a scripted flight: start-up, idle, throttle up, afterburner
// with a cannon burst and a missile, throttle back, shutdown.
//   c++ -std=c++20 -O2 -I../../src render_preview.cpp -o render_preview
//   ./render_preview jet_preview.wav
#include <cstdio>
#include <cstdint>
#include <vector>

#include "gta4_jet_engine.h"

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "jet_preview.wav";
  static gta4::jet::EngineSynth synth;
  constexpr int kRate = 48000, kBlock = 256;
  const float idle = gta4::jet::EngineSynth::kIdle;
  struct Step { float until, spool, ab, volume; };
  // seconds: what the trainer would set by then
  const Step script[] = {
      {0.5f, 0.0f, 0, 1},       // silence
      {5.0f, idle, 0, 1},        // getting in: spool up to idle
      {7.0f, idle, 0, 1},        // idle
      {10.0f, 0.75f, 0, 1},      // throttle up
      {12.0f, 1.0f, 0, 1},       // full throttle
      {16.0f, 1.0f, 1, 1},       // afterburner (cannon at 13 s, missile at 14.5 s)
      {19.0f, 0.6f, 0, 1},       // throttle back
      {21.0f, idle, 0, 1},       // idle
      {27.0f, 0.0f, 0, 0.8f},    // getting out: wind down
  };
  std::vector<int16_t> pcm;
  float lr[kBlock * 2];
  double t = 0;
  size_t step = 0;
  bool missile = false;
  double next_shot = 13.0;
  while (t < 27.0) {
    while (step + 1 < sizeof(script) / sizeof(script[0]) && t >= script[step].until) ++step;
    const auto& s = script[step];
    synth.SetTarget(s.spool, s.ab, s.volume);
    if (t >= next_shot && t < 14.0) { synth.Shot(); next_shot += 0.06; }
    if (!missile && t >= 14.5) { synth.Launch(); missile = true; }
    synth.Render(lr, kBlock);
    for (float v : lr) {
      const float c = v > 1 ? 1 : (v < -1 ? -1 : v);
      pcm.push_back(int16_t(c * 32767));
    }
    t += double(kBlock) / kRate;
  }
  FILE* f = std::fopen(out, "wb");
  if (!f) return 1;
  const uint32_t data = uint32_t(pcm.size() * 2), riff = 36 + data, fmt = 16, rate = kRate, bps = kRate * 4;
  const uint16_t pcmfmt = 1, ch = 2, align = 4, bits = 16;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcmfmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&bps, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
  std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
  std::printf("wrote %s (%.1f s)\n", out, t);
  return 0;
}
