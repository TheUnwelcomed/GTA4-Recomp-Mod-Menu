// Renders the Lazar 3's synthesized engine (gta4_car_engine.h) to a WAV:
// four seconds of idle, two with the turret slewing, one after; prints each
// part's level, spectral centroid and share of energy per octave band, to
// compare voicings without a console (the v1.7.24 turret hum and idle).
// build: g++ -std=c++20 -O2 -DHEADER='"<menu src>/gta4_car_engine.h"' engine_render.cpp -o engine_render
// run:   ./engine_render lazar.wav
#include <cmath>
#include <cstdio>
#include <vector>
#include <complex>
#include HEADER
using namespace gta4::car;
static void wav(const char* path, const std::vector<float>& lr) {
  FILE* f = std::fopen(path, "wb");
  const int n = int(lr.size()), rate = 48000, bytes = n * 2;
  auto w32 = [&](int v) { std::fwrite(&v, 4, 1, f); };
  auto w16 = [&](short v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2);
  w32(rate); w32(rate * 4); w16(4); w16(16); std::fwrite("data", 1, 4, f); w32(bytes);
  for (float v : lr) w16(short(std::lround(std::fmax(-1.0, std::fmin(1.0, v)) * 32000)));
  std::fclose(f);
}
static void bands(const char* what, const std::vector<float>& lr, size_t from) {
  // energy per octave band via a crude DFT of a 1-s mono window
  const int N = 48000;
  std::vector<float> m(N);
  for (int i = 0; i < N; ++i) m[i] = 0.5f * (lr[2 * (from + i)] + lr[2 * (from + i) + 1]);
  double edges[] = {20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240};
  double e[9] = {};
  double total = 0, cent = 0;
  for (int k = 20; k < 10240; k += (k < 640 ? 2 : k < 2560 ? 8 : 32)) {
    std::complex<double> acc = 0;
    for (int i = 0; i < N; i += 1) acc += double(m[i]) * std::polar(1.0, -2 * M_PI * k * i / N);
    const double p = std::norm(acc);
    for (int b = 0; b < 9; ++b) if (k >= edges[b] && k < edges[b + 1]) e[b] += p * (k < 640 ? 2 : k < 2560 ? 8 : 32);
    total += p * (k < 640 ? 2 : k < 2560 ? 8 : 32);
    cent += k * p * (k < 640 ? 2 : k < 2560 ? 8 : 32);
  }
  double rms = 0;
  for (float v : m) rms += v * v;
  std::printf("%-22s rms %.3f centroid %5.0f Hz |", what, std::sqrt(rms / N), cent / total);
  for (int b = 0; b < 9; ++b) std::printf(" %4.0f:%4.1f%%", edges[b], 100 * e[b] / total);
  std::printf("\n");
}
int main(int, char** argv) {
  EngineSynth syn(kLazar);
  std::vector<float> lr;
  std::vector<float> buf(2 * 480);
  auto run = [&](float secs) { for (int i = 0; i < int(secs * 100); ++i) { syn.Render(buf.data(), 480); lr.insert(lr.end(), buf.begin(), buf.end()); } };
  syn.SetTarget(EngineSynth::kIdle, 0.0f, 0.0f, 0.9f);
  run(4.0f);  // idle settles
  const size_t idle_at = lr.size() / 2 - 48000;
  syn.Servo(1.0f);
  run(2.0f);
  const size_t slew_at = lr.size() / 2 - 48000;
  syn.Servo(0.0f);
  run(1.0f);
  bands("idle", lr, idle_at);
  bands("idle + turret slewing", lr, slew_at);
  wav(argv[1], lr);
}
