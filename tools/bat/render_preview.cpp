// Renders the Batmobile's synthesized engine (src/gta4_bat_engine.h) to a WAV
// file through a scripted drive: start-up, idle, two blips of the throttle,
// four gears, a cruise, the booster, a lift-off, idle, the canopy opening and
// shutting, shut down. With a second argument it also writes the script's
// section boundaries (seconds) for a level check.
//   c++ -std=c++20 -O2 -Wall -Wextra -I../../src render_preview.cpp -o render_preview
//   ./render_preview bat_preview.wav [sections.txt]
#include <cstdio>
#include <cstdint>
#include <vector>

#include "gta4_bat_engine.h"

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "bat_preview.wav";
  static gta4::bat::EngineSynth synth;
  constexpr int kRate = 48000, kBlock = 256;
  constexpr float idle = gta4::bat::EngineSynth::kIdle;
  struct Step { float until, rpm0, rpm1, throttle, boost; const char* name; };
  // seconds: what the trainer would set by then (rpm ramps from rpm0 to rpm1)
  const Step script[] = {
      {0.5f, 0, 0, 0, 0, "silence"},
      {3.0f, idle, idle, 0, 0, "start-up"},
      {5.5f, idle, idle, 0, 0, "idle"},
      {6.0f, idle, 0.62f, 1, 0, "blip 1"},
      {6.8f, idle, idle, 0, 0, "blip 1 off"},
      {7.3f, idle, 0.7f, 1, 0, "blip 2"},
      {8.2f, idle, idle, 0, 0, "blip 2 off"},
      {10.2f, 0.25f, 0.95f, 1, 0, "gear 1"},
      {12.4f, 0.55f, 0.95f, 1, 0, "gear 2"},
      {14.8f, 0.58f, 0.95f, 1, 0, "gear 3"},
      {17.4f, 0.62f, 0.93f, 1, 0, "gear 4"},
      {20.0f, 0.7f, 0.7f, 0.45f, 0, "cruise"},
      {23.0f, 0.75f, 0.95f, 1, 1, "boost"},
      {25.0f, 0.95f, 0.4f, 0, 0, "lift-off"},
      {27.5f, idle, idle, 0, 0, "idle 2 (canopy at 25.5 / 27.0)"},
      {31.5f, 0, 0, 0, 0, "shut down"},
  };
  constexpr int kSteps = sizeof(script) / sizeof(script[0]);
  if (argc > 2) {
    if (FILE* s = std::fopen(argv[2], "w")) {
      float from = 0;
      for (const auto& st : script) {
        std::fprintf(s, "%.2f %.2f %s\n", from, st.until, st.name);
        from = st.until;
      }
      std::fclose(s);
    }
  }
  std::vector<int16_t> pcm;
  float lr[kBlock * 2];
  double t = 0;
  int step = 0;
  bool opened = false, shut = false;
  while (t < 31.5) {
    while (step + 1 < kSteps && t >= script[step].until) ++step;
    const auto& s = script[step];
    const float from = step ? script[step - 1].until : 0.0f;
    const float u = float((t - from) / (s.until - from));
    synth.SetTarget(s.rpm0 + (s.rpm1 - s.rpm0) * u, s.throttle, s.boost, 1.0f);
    if (!opened && t >= 25.5) { synth.CanopyOpen(); opened = true; }
    if (!shut && t >= 27.0) { synth.CanopyShut(); shut = true; }
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
  std::printf("wrote %s (%.1f s), quiet at the end: %s\n", out, t, synth.Quiet() ? "yes" : "no");
  return 0;
}
