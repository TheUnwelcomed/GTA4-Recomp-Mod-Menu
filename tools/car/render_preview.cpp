// Renders an add-on car's synthesized engine (src/gta4_car_engine.h) to a WAV
// file through a scripted drive: start-up, idle, two blips of the throttle,
// four gears, a cruise, the booster (Batmobile) or full throttle, a lift-off,
// idle, the car's own sounds (the Batmobile's canopy opening and shutting,
// the Deimos's and the Nexus's pop-up lamps going up and down, the Nexus's
// gullwing doors; the Lazar 3's turret slewing at idle, a burst from its
// cannon cruising, its air brakes at the stop, its ramps down and up), shut
// down. With a third argument it also writes the script's section
// boundaries (seconds) for a level check.
//   c++ -std=c++20 -O2 -Wall -Wextra -I../../src render_preview.cpp -o render_preview
//   ./render_preview batmobile bat_preview.wav [sections.txt]
//   ./render_preview deimos|nexus|arbiterx|dom645t|gauntletc|lazar3|delorean|hellcat <car>_preview.wav [sections.txt]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "gta4_car_engine.h"

namespace {
// The car's own sounds in idle 2: the canopy (hiss up at 25.5, down at
// 27.0); pop-up lamps (up 25.5, down 26.7); doors (opening at 26.0, shut at
// 27.2), a gullwing's struts with them.
// The Lazar's: turret slewing 4.0-5.0, cannon at 18.0-19.0 (four rounds),
// air brakes at 25.0, ramps down at 25.6 and up at 27.6.
enum class Extras { kCanopy, kPopupsAndDoors, kDoors, kGullwings, kArmour };
struct Car {
  const char* name;
  const gta4::car::Character* character;
  bool booster;
  Extras extras;
};
constexpr Car kCars[] = {
    {"batmobile", &gta4::car::kBatmobile, true, Extras::kCanopy},
    {"deimos", &gta4::car::kDeimos, false, Extras::kPopupsAndDoors},
    {"nexus", &gta4::car::kNexus, false, Extras::kGullwings},
    {"arbiterx", &gta4::car::kArbiter, false, Extras::kDoors},
    {"dom645t", &gta4::car::kDominator, false, Extras::kDoors},
    {"gauntletc", &gta4::car::kGauntlet, false, Extras::kDoors},
    {"lazar3", &gta4::car::kLazar, false, Extras::kArmour},
    {"delorean", &gta4::car::kDelorean, false, Extras::kGullwings},
    {"hellcat", &gta4::car::kHellcat, false, Extras::kDoors},
    {"charger", &gta4::car::kCharger, false, Extras::kDoors},
};
}  // namespace

int main(int argc, char** argv) {
  const Car* car = &kCars[0];
  if (argc > 1)
    for (const auto& c : kCars)
      if (std::strcmp(argv[1], c.name) == 0)
        car = &c;
  char fallback[64];
  std::snprintf(fallback, sizeof(fallback), "%s_preview.wav", car->name);
  const char* out = argc > 2 ? argv[2] : fallback;
  auto synth = std::make_unique<gta4::car::EngineSynth>(*car->character);
  constexpr int kRate = 48000, kBlock = 256;
  constexpr float idle = gta4::car::EngineSynth::kIdle;
  struct Step { float until, rpm0, rpm1, throttle, boost; const char* name; };
  // seconds: what the trainer would set by then (rpm ramps from rpm0 to rpm1)
  const float boost = car->booster ? 1.0f : 0.0f;
  const bool canopy = car->extras == Extras::kCanopy;
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
      {23.0f, 0.75f, 0.95f, 1, boost, car->booster ? "boost" : "full throttle"},
      {25.0f, 0.95f, 0.4f, 0, 0, "lift-off"},
      {27.5f, idle, idle, 0, 0, canopy ? "idle 2 (canopy at 25.5 / 27.0)" : "idle 2 (lamps 25.5 / 26.7, doors 26.0 / 27.2)"},
      {31.5f, 0, 0, 0, 0, "shut down"},
  };
  constexpr int kSteps = sizeof(script) / sizeof(script[0]);
  if (argc > 3) {
    if (FILE* s = std::fopen(argv[3], "w")) {
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
  bool first = false, second = false, doors_up = false, doors_down = false;
  const bool armour = car->extras == Extras::kArmour;
  int rounds = 0;
  bool braked = false, ramps_down = false, ramps_up = false;
  while (t < 31.5) {
    while (step + 1 < kSteps && t >= script[step].until) ++step;
    const auto& s = script[step];
    const float from = step ? script[step - 1].until : 0.0f;
    const float u = float((t - from) / (s.until - from));
    synth->SetTarget(s.rpm0 + (s.rpm1 - s.rpm0) * u, s.throttle, s.boost, 1.0f);
    const bool popups = car->extras == Extras::kPopupsAndDoors || car->extras == Extras::kGullwings;
    if (!first && !armour && t >= 25.5) {
      if (canopy)
        synth->CanopyOpen();
      else if (popups)
        synth->Motor(0.4f);
      first = true;
    }
    if (!second && !armour && t >= (canopy ? 27.0 : 26.7)) {
      if (canopy)
        synth->CanopyShut();
      else if (popups)
        synth->Motor(0.4f);
      second = true;
    }
    if (armour) {
      synth->Servo(t >= 4.0 && t < 5.0 ? (t < 4.5 ? 1.0f : 0.5f) : 0.0f);
      if (rounds < 4 && t >= 18.0 + 0.32 * rounds) {
        synth->Cannon();
        ++rounds;
      }
      if (!braked && t >= 25.0) {
        synth->AirBrake();
        braked = true;
      }
      if (!ramps_down && t >= 25.6) {
        synth->Hydraulic(1.5f, true);
        ramps_down = true;
      }
      if (!ramps_up && t >= 27.6) {
        synth->Hydraulic(1.7f, false);
        ramps_up = true;
      }
    } else if (!canopy) {
      if (!doors_up && t >= 26.0) {
        synth->DoorOpen();
        if (car->extras == Extras::kGullwings)
          synth->CanopyOpen(false);  // the struts
        doors_up = true;
      }
      if (!doors_down && t >= 27.2) {
        if (car->extras == Extras::kGullwings)
          synth->CanopyShut(false);
        synth->DoorShut();
        doors_down = true;
      }
    }
    synth->Render(lr, kBlock);
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
  std::printf("wrote %s (%.1f s), quiet at the end: %s\n", out, t, synth->Quiet() ? "yes" : "no");
  return 0;
}
