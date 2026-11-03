// Liberty Trainer: the sound of a place the DeLorean travels to (v1.7.37),
// synthesized.
//
// An original ambience made from scratch here: nothing in it comes from
// films, games or any recording. It plays through the port's own mixer
// (ps5/game/sound.h) while the player is on a map set's island, which has
// none of the city's sounds: a map mod brings no audio, and the game's
// ambient zones end at the shore.
//
// Two characters:
//  - keys (the Gator Keys, Florida 1984): surf breaking on a reef, a light
//    warm breeze, gulls now and then, and the insects' trill in the heat;
//  - atoll (Wake Island, 1941): a stronger steady wind over the lagoon,
//    heavier surf, a gull less often, and every minute or two a propeller
//    plane droning past (its pitch falling as it goes by: a lost place with
//    a war somewhere beyond the horizon).
//
// How it is made: the surf is noise through a low-pass whose cutoff and
// level swell with each wave (two slow sines, their sum shaped, every wave a
// little different), two independent noise sources left and right for
// width; the wind is band-passed noise whose centre wanders, under a low
// rumble; a gull's cry is two to four syllables of a sine with harmonics
// gliding down with a vibrato, each panned somewhere; the insects a trill
// of a high tone pulsed at 20 Hz; the plane two saw tones (the engine and
// its exhaust) with a Doppler glide and a pan across.
//
// Plain C++ with no platform code: the game thread sets targets (atomics),
// the mixer's thread renders. tools/island/render_preview.cpp (in the
// trainer repository) renders each character to a file.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gta4::island {

class Ambience {
 public:
  static constexpr float kRate = 48000.0f;
  enum Character : int { kKeys = 0, kAtoll = 1 };

  // volume 0..1 (0 fades out: Quiet() once silent); the character (keys, atoll).
  void SetTarget(float volume, int character) {
    target_volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    character_.store(character, std::memory_order_relaxed);
  }
  bool Quiet() const { return quiet_.load(std::memory_order_relaxed); }

  // Interleaved stereo, frames at kRate, overwritten (not mixed into).
  void Render(float* lr, size_t frames) {
    const float target = target_volume_.load(std::memory_order_relaxed);
    const int ch = character_.load(std::memory_order_relaxed);
    const bool atoll = ch == kAtoll;
    constexpr float dt = 1.0f / kRate;
    for (size_t i = 0; i < frames; ++i) {
      // the volume eased over ~2 s either way
      volume_ += (target - volume_) * (dt / 2.0f);
      if (target <= 0.0f && volume_ < 0.001f)
        volume_ = 0.0f;
      t_ += dt;
      float l = 0.0f, r = 0.0f;
      // ---- surf: waves every 7-12 s, each a swell of brighter, louder noise
      wave_a_ = Wrap(wave_a_ + dt / 8.3f);
      wave_b_ = Wrap(wave_b_ + dt / 11.7f);
      const float swell = std::max(0.0f, Sin(wave_a_) * 0.6f + Sin(wave_b_) * 0.4f + 0.15f);
      const float wave = swell * swell * (atoll ? 1.25f : 1.0f);
      const float cutoff = 180.0f + 1400.0f * wave;
      const float g_surf = 3.14159265f * cutoff / kRate;
      const float surf_l = surf_l_.Lowpass(Noise(), g_surf, 0.9f) * (0.08f + 0.9f * wave);
      const float surf_r = surf_r_.Lowpass(Noise(), g_surf * 1.07f, 0.9f) * (0.08f + 0.9f * wave);
      // (the wash between waves: a steady hiss)
      const float wash = wash_.Lowpass(Noise(), 3.14159265f * 900.0f / kRate, 1.1f) * 0.05f;
      l += (surf_l * 0.6f + wash) * 0.35f;
      r += (surf_r * 0.6f + wash) * 0.35f;
      // ---- wind: band-passed noise, its centre and strength wandering
      wind_ph_ = Wrap(wind_ph_ + dt / 6.1f);
      wind_ph2_ = Wrap(wind_ph2_ + dt / 2.7f);
      const float gust = 0.5f + 0.5f * Sin(wind_ph_) * 0.6f + 0.2f * Sin(wind_ph2_);
      const float wind_level = (atoll ? 0.22f : 0.08f) * (0.6f + 0.4f * gust);
      const float centre = (atoll ? 420.0f : 330.0f) * (0.8f + 0.4f * gust);
      const float wn = Noise();
      const float wind = wind_bp_.Bandpass(wn, 3.14159265f * centre / kRate, 1.3f) * wind_level +
                         rumble_.Lowpass(wn, 3.14159265f * 70.0f / kRate, 0.9f) * wind_level * 0.8f;
      l += wind * 0.9f;
      r += wind_r_.Bandpass(Noise(), 3.14159265f * centre * 1.1f / kRate, 1.3f) * wind_level * 0.9f +
           wind * 0.3f;
      // ---- gulls: a cry now and then
      gull_wait_ -= dt;
      if (gull_wait_ <= 0.0f) {
        gull_wait_ = (atoll ? 25.0f : 9.0f) + Uniform() * (atoll ? 40.0f : 18.0f);
        gull_syllables_ = 2 + int(Uniform() * 3.0f);
        gull_t_ = 0.0f;
        gull_pan_ = Uniform();
        gull_base_ = 1100.0f + Uniform() * 1100.0f;
        gull_far_ = 0.35f + Uniform() * 0.65f;  // (nearer: louder, brighter)
      }
      if (gull_syllables_ > 0) {
        gull_t_ += dt;
        const float syl = 0.32f;  // seconds a syllable
        const float within = std::fmod(gull_t_, syl);
        if (gull_t_ >= syl * float(gull_syllables_)) {
          gull_syllables_ = 0;
        } else {
          const float env = Smooth(0.0f, 0.02f, within) * (1.0f - Smooth(0.14f, 0.30f, within));
          const float glide = 1.0f - 0.32f * within / syl;  // down through the syllable
          const float vib = 1.0f + 0.03f * Sin(gull_vib_);
          gull_vib_ = Wrap(gull_vib_ + 24.0f * dt);
          gull_ph_ = Wrap(gull_ph_ + gull_base_ * glide * vib * dt);
          const float cry = (Sin(gull_ph_) + 0.45f * Sin(Wrap(2.0f * gull_ph_)) + 0.2f * Sin(Wrap(3.0f * gull_ph_))) * env;
          const float far_lp = gull_lp_.Lowpass(cry, 3.14159265f * (1500.0f + 4000.0f * gull_far_) / kRate, 1.0f);
          const float level = 0.09f * gull_far_;
          l += far_lp * level * (1.0f - gull_pan_ * 0.8f);
          r += far_lp * level * (0.2f + gull_pan_ * 0.8f);
        }
      }
      // ---- insects (the keys): a high trill pulsed at 20 Hz, coming and going
      if (!atoll) {
        insect_wait_ -= dt;
        if (insect_wait_ <= 0.0f) {
          insect_wait_ = 6.0f + Uniform() * 12.0f;
          insect_on_ = !insect_on_ || Uniform() < 0.6f;
        }
        insect_level_ += ((insect_on_ ? 1.0f : 0.0f) - insect_level_) * dt / 1.5f;
        insect_pulse_ = Wrap(insect_pulse_ + 21.0f * dt);
        insect_ph_ = Wrap(insect_ph_ + 4600.0f * dt);
        const float pulse = std::max(0.0f, Sin(insect_pulse_));
        const float trill = Sin(insect_ph_) * pulse * pulse * insect_level_ * 0.012f;
        l += trill * 0.7f;
        r += trill;
      }
      // ---- the plane (the atoll): a drone passing every minute or two
      if (atoll) {
        plane_wait_ -= dt;
        if (plane_wait_ <= 0.0f && plane_t_ < 0.0f) {
          plane_wait_ = 50.0f + Uniform() * 70.0f;
          plane_t_ = 0.0f;
          plane_dir_ = Uniform() < 0.5f ? -1.0f : 1.0f;
        }
        if (plane_t_ >= 0.0f) {
          plane_t_ += dt;
          constexpr float pass = 44.0f;  // seconds across
          if (plane_t_ > pass) {
            plane_t_ = -1.0f;
          } else {
            const float x = plane_t_ / pass;  // 0..1, overhead at 0.5
            const float near = 1.0f - std::fabs(x - 0.5f) * 2.0f;
            const float level = near * near * near * 0.16f;
            const float doppler = 1.06f - 0.12f * Smooth(0.35f, 0.65f, x);
            const float rpm = 96.0f * doppler;
            plane_ph_ = Wrap(plane_ph_ + rpm * dt);
            plane_ph2_ = Wrap(plane_ph2_ + rpm * 1.5f * dt);
            float drone = (Saw(plane_ph_) * 0.6f + Saw(plane_ph2_) * 0.3f + Sin(Wrap(plane_ph_ * 4.0f)) * 0.15f);
            drone = plane_lp_.Lowpass(drone, 3.14159265f * (300.0f + 1200.0f * near) / kRate, 0.8f) * level;
            const float pan = 0.5f + plane_dir_ * (x - 0.5f) * 0.9f;
            l += drone * (1.0f - pan);
            r += drone * pan;
          }
        }
      }
      lr[2 * i] = l * volume_;
      lr[2 * i + 1] = r * volume_;
    }
    quiet_.store(volume_ <= 0.0f, std::memory_order_relaxed);
  }

 private:
  struct Svf {
    float ic1 = 0, ic2 = 0;
    float Lowpass(float x, float g, float k) {
      const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
      const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = Flush(2 * v1 - ic1), ic2 = Flush(2 * v2 - ic2);
      return v2;
    }
    float Bandpass(float x, float g, float k) {
      const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
      const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = Flush(2 * v1 - ic1), ic2 = Flush(2 * v2 - ic2);
      return v1;
    }
    static float Flush(float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; }
  };
  static float Wrap(float p) { return p - std::floor(p); }
  static float Sin(float p) { return std::sin(6.28318531f * p); }
  static float Saw(float p) { return 2.0f * p - 1.0f; }
  static float Smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
  }
  float Uniform() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return float(seed_ >> 8) * (1.0f / 16777216.0f);
  }
  float Noise() { return Uniform() * 2.0f - 1.0f; }

  std::atomic<float> target_volume_{0};
  std::atomic<int> character_{kKeys};
  std::atomic<bool> quiet_{true};
  float volume_ = 0.0f, t_ = 0.0f;
  float wave_a_ = 0.0f, wave_b_ = 0.37f;
  Svf surf_l_, surf_r_, wash_, wind_bp_, wind_r_, rumble_, gull_lp_, plane_lp_;
  float wind_ph_ = 0.0f, wind_ph2_ = 0.5f;
  float gull_wait_ = 4.0f, gull_t_ = 0.0f, gull_pan_ = 0.5f, gull_base_ = 1500.0f, gull_far_ = 0.5f;
  float gull_ph_ = 0.0f, gull_vib_ = 0.0f;
  int gull_syllables_ = 0;
  float insect_wait_ = 3.0f, insect_level_ = 0.0f, insect_pulse_ = 0.0f, insect_ph_ = 0.0f;
  bool insect_on_ = true;
  float plane_wait_ = 20.0f, plane_t_ = -1.0f, plane_dir_ = 1.0f, plane_ph_ = 0.0f, plane_ph2_ = 0.0f;
  uint32_t seed_ = 0x2545F491u;
};

}  // namespace gta4::island
