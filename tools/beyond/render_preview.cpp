// Renders the Batman Beyond Batmobile's synthesized sound
// (src/gta4_beyond_engine.h) to a WAV file, through a scripted scene:
// hovering, spooling up and away, cruising, a boost, a fly-past heard from
// the street (Doppler), settling into a hover, the floor hatch and the beam
// dropping the pilot out, the cloak on and off.
//   c++ -std=c++20 -O2 -I../../src render_preview.cpp -o render_preview
//   ./render_preview beyond_preview.wav
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "gta4_beyond_engine.h"

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "beyond_preview.wav";
  static gta4::beyond::EngineSynth synth;
  constexpr int kRate = 48000, kBlock = 256;
  const float idle = gta4::beyond::EngineSynth::kIdle;
  struct Key { float t, spool, speed, boost; };
  // the inside scene (0-11 s) and the hover after the fly-past (17-30 s)
  const Key keys[] = {
      {0.0f, 0.0f, 0, 0},    {0.4f, idle, 0, 0},   {3.5f, idle, 0, 0},     // starting, hovering
      {4.5f, 1.0f, 8, 0},    {6.0f, 0.85f, 45, 0}, {8.0f, 0.8f, 60, 0},    // spool up, away, cruising
      {8.6f, 1.0f, 70, 1},   {10.2f, 1.0f, 85, 1}, {11.0f, 0.8f, 70, 0},   // boost
      {11.0f, 0.85f, 50, 0}, {17.0f, 0.85f, 50, 0},                         // (outside: the fly-past)
      {17.0f, 0.7f, 20, 0},  {19.5f, idle, 0, 0},  {30.0f, idle, 0, 0},    // settling into a hover
  };
  std::vector<int16_t> pcm;
  float lr[kBlock * 2];
  double t = 0;
  bool hatch = false, beam = false, cloak_on = false, cloak_off = false, missile = false, closed = false;
  while (t < 30.0) {
    // the keys: linear between neighbours
    size_t k = 0;
    while (k + 1 < sizeof(keys) / sizeof(keys[0]) && keys[k + 1].t <= t) ++k;
    const Key& a = keys[k];
    const Key& b = keys[k + 1 < sizeof(keys) / sizeof(keys[0]) ? k + 1 : k];
    const float u = b.t > a.t ? float((t - a.t) / (b.t - a.t)) : 0.0f;
    const float spool = a.spool + (b.spool - a.spool) * std::fmin(std::fmax(u, 0.0f), 1.0f);
    const float speed = a.speed + (b.speed - a.speed) * std::fmin(std::fmax(u, 0.0f), 1.0f);
    const float boost = a.boost + (b.boost - a.boost) * std::fmin(std::fmax(u, 0.0f), 1.0f);
    synth.SetTarget(spool, speed, boost, 1.0f);
    // 11-17 s: heard from the street as it flies past at 50 m/s, 12 m off at 14 s
    if (t >= 11.0 && t < 17.0) {
      const double along = 50.0 * (t - 14.0), off = 12.0;
      const double dist = std::sqrt(along * along + off * off);
      synth.SetListener(true, float(dist), float(-50.0 * along / dist));
    } else if (t >= 17.0 && t < 19.5) {  // it comes back and settles 8 m away
      synth.SetListener(true, float(8.0 + 30.0 * (19.5 - t) / 2.5), float(12.0 * (19.5 - t) / 2.5));
    } else if (t >= 19.5) {
      synth.SetListener(true, 8.0f, 0.0f);
    } else {
      synth.SetListener(false, 1.0f, 0.0f);
    }
    if (!missile && t >= 7.0) missile = true, synth.Launch();
    if (!hatch && t >= 21.0) hatch = true, synth.Hatch(true);
    if (!beam && t >= 21.4) beam = true, synth.Beam(true);
    if (!closed && t >= 23.0) closed = true, synth.Hatch(false);
    if (!cloak_on && t >= 24.5) cloak_on = true, synth.Cloak(true);
    if (!cloak_off && t >= 27.5) cloak_off = true, synth.Cloak(false);
    synth.Render(lr, kBlock);
    for (int i = 0; i < kBlock * 2; ++i)
      pcm.push_back(int16_t(std::lround(std::fmax(-1.0f, std::fmin(1.0f, lr[i])) * 32000.0f)));
    t += double(kBlock) / kRate;
  }
  FILE* f = std::fopen(out, "wb");
  if (!f)
    return 1;
  const uint32_t data = uint32_t(pcm.size() * 2), riff = 36 + data, fmt = 16, rate = kRate, bytes = kRate * 4;
  const uint16_t pcm_tag = 1, ch = 2, align = 4, bits = 16;
  std::fwrite("RIFF", 1, 4, f), std::fwrite(&riff, 4, 1, f), std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f), std::fwrite(&pcm_tag, 2, 1, f), std::fwrite(&ch, 2, 1, f), std::fwrite(&rate, 4, 1, f);
  std::fwrite(&bytes, 4, 1, f), std::fwrite(&align, 2, 1, f), std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f), std::fwrite(&data, 4, 1, f), std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
  std::printf("wrote %s: %.1f s\n", out, pcm.size() / 2.0 / kRate);
  return 0;
}
