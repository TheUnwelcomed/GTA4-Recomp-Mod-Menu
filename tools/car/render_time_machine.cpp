// Renders the DeLorean time machine's synthesized sounds (src/gta4_time_machine.h)
// to a WAV file through a scripted run: the time circuits on, a date keyed in,
// the flux capacitor, the build-up to 88 mph, the departure, the arrival's
// triple boom, the cold vents, the plutonium reload, Mr. Fusion, the wheels
// folding for flight and the hover thrusters, the wheels down, the empty
// warning.
//   c++ -std=c++20 -O2 -Wall -Wextra -I../../src render_time_machine.cpp -o render_time_machine
//   ./render_time_machine time_machine.wav
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "gta4_time_machine.h"

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "time_machine.wav";
  auto tm = std::make_unique<gta4::car::TimeMachineSynth>();
  constexpr int kRate = 48000, kBlock = 256;
  std::vector<int16_t> pcm;
  float lr[kBlock * 2];
  double t = 0;
  int stage = 0;
  // (seconds, what happens)
  while (t < 40.0) {
    auto at = [&](double when) { return t >= when && stage < 100 && t - double(kBlock) / kRate < when; };
    if (at(0.5)) tm->CircuitsOn();
    if (t >= 1.0 && t < 1.05) tm->SetFlux(0.3f);
    for (int k = 0; k < 8; ++k)
      if (at(1.4 + 0.22 * k)) tm->Key((k * 7) % 10);
    if (at(3.3)) tm->Enter();
    // the build-up: 6-10 s
    if (t >= 6.0 && t < 10.0) {
      const float u = float((t - 6.0) / 4.0);
      tm->SetCharge(u);
      tm->SetFlux(0.3f + 0.7f * u);
    }
    if (at(10.0)) {
      tm->SetCharge(0.0f);
      tm->SetFlux(0.0f);
      tm->Depart(false);
    }
    if (at(13.0)) tm->Arrive();
    if (at(13.6)) tm->Vents();
    if (at(21.0)) tm->Empty();
    if (at(22.0)) tm->Plutonium();
    if (at(25.5)) tm->Fusion();
    if (at(29.5)) tm->Convert(true);
    if (t >= 30.5 && t < 35.0) tm->SetHover(t < 32.5 ? 0.3f : 0.9f);
    if (at(35.0)) {
      tm->SetHover(0.0f);
      tm->Convert(false);
    }
    if (at(37.0)) tm->CircuitsOff();
    tm->Render(lr, kBlock);
    for (float v : lr) {
      const float c = v > 1 ? 1 : (v < -1 ? -1 : v);
      pcm.push_back(int16_t(c * 32767));
    }
    t += double(kBlock) / kRate;
  }
  (void)stage;
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
  std::printf("wrote %s (%.1f s), quiet at the end: %s\n", out, t, tm->Quiet() ? "yes" : "no");
  return 0;
}
