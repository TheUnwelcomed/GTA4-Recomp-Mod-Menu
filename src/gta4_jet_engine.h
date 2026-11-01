// Liberty Trainer: the F-16's engine, synthesized.
//
// GTA IV has no jets of its own (its airport planes are ambient sound
// effects), so the jet's engine is generated here and played through the
// port's own audio mixer (ps5/game/sound.h) beside the game's sound:
//
//  - spool: the engine's speed chases its target with a turbine's lag,
//    spooling up in a second or two and winding down more slowly. Getting in
//    starts it (the whine rises to idle), getting out shuts it down.
//  - whine: the turbine's tones (fan and compressor blade rates and their
//    harmonics), their pitch following the engine's speed.
//  - roar: jet noise, low-passed noise that opens up and grows with power.
//  - rumble: the low end, mostly the afterburner's.
//  - crackle: the afterburner's popping at full throttle.
//  - the cannon's rounds and a missile launch's whoosh, at their own level
//    (the volume set with the engine scales the engine only);
//  - the lock-on tone: two short beeps.
//
// Plain C++ with no platform code: the game thread sets targets (atomics),
// the mixer's thread renders. A PC test renders it to a file
// (tools/jet/render_preview.cpp in the trainer repository).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gta4::jet {

class EngineSynth {
 public:
  static constexpr float kRate = 48000.0f;
  static constexpr float kIdle = 0.32f;

  // spool: the engine's target speed (0 off, kIdle idle, 1 full power);
  // afterburner 0..1; volume 0..1 (distance, in or out of the jet).
  void SetTarget(float spool, float afterburner, float volume) {
    target_spool_.store(std::clamp(spool, 0.0f, 1.0f), std::memory_order_relaxed);
    target_ab_.store(std::clamp(afterburner, 0.0f, 1.0f), std::memory_order_relaxed);
    target_volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
  }
  void Shot() { shots_.fetch_add(1, std::memory_order_relaxed); }
  void Launch() { launches_.fetch_add(1, std::memory_order_relaxed); }
  void Beep() { beeps_.fetch_add(1, std::memory_order_relaxed); }
  // Spooled down, silent and nothing left ringing: the voice can stop.
  bool Quiet() const { return quiet_.load(std::memory_order_relaxed); }
  float Rpm() const { return rpm_out_.load(std::memory_order_relaxed); }

  // Interleaved stereo, frames at kRate, overwritten (not mixed into).
  void Render(float* lr, size_t frames) {
    const float target = target_spool_.load(std::memory_order_relaxed);
    const float ab_target = target_ab_.load(std::memory_order_relaxed);
    const float vol_target = target_volume_.load(std::memory_order_relaxed);
    for (uint32_t n = shots_.exchange(0, std::memory_order_relaxed); n; --n) shot_ = 1.0f, thump_ = 1.0f;
    for (uint32_t n = launches_.exchange(0, std::memory_order_relaxed); n; --n) launch_ = 1.0f;
    for (uint32_t n = beeps_.exchange(0, std::memory_order_relaxed); n; --n) beep_t_ = 0.0f;

    constexpr float dt = 1.0f / kRate;
    for (size_t i = 0; i < frames; ++i) {
      // ---- spool: exponential approach with a slew limit (turbine inertia)
      const float d = target - rpm_;
      const float rate = d > 0 ? std::min(d * (1.0f / 1.3f), 0.55f) : std::max(d * (1.0f / 2.2f), -0.30f);
      rpm_ += rate * dt;
      ab_ += (ab_target - ab_) * (ab_target > ab_ ? 4.0f : 2.5f) * dt;
      vol_ += (vol_target - vol_) * 8.0f * dt;
      const float rpm = std::max(rpm_, 0.0f);
      const float p2 = rpm * rpm;

      // ---- whine: blade-rate tones with a slow, slight wander
      if ((i & 63) == 0) wander_ += (Noise() * 0.5f - wander_ * 0.02f) * 0.01f;
      const float f1 = (650.0f + 2450.0f * std::pow(rpm, 1.25f)) * (1.0f + 0.004f * wander_);
      const float fan = 0.37f * f1;
      ph1_ = Wrap(ph1_ + f1 * dt);
      ph1b_ = Wrap(ph1b_ + f1 * 1.0045f * dt);
      ph2_ = Wrap(ph2_ + 2.0f * f1 * dt);
      ph3_ = Wrap(ph3_ + 3.02f * f1 * dt);
      phf_ = Wrap(phf_ + fan * dt);
      const float tones = Sin(ph1_) + 0.55f * Sin(ph1b_) + 0.32f * Sin(ph2_) + 0.10f * Sin(ph3_) +
                          0.45f * Sin(phf_);
      const float whine_amp = 0.060f * Smooth(0.02f, 0.5f, rpm) * (0.6f + 0.4f * rpm) * (1.0f - 0.45f * ab_);
      const float whine = tones * whine_amp;

      // ---- roar: low-passed noise opening up with power (stereo: mostly shared)
      const float common = Noise(), nl = Noise(), nr = Noise();
      const float cut = 160.0f + 2300.0f * p2 + 900.0f * ab_;
      const float g = std::tan(3.14159265f * std::min(cut, 12000.0f) * dt);
      const float roar_in_l = 0.78f * common + 0.22f * nl, roar_in_r = 0.78f * common + 0.22f * nr;
      const float roar_l = roar_l_.Lowpass(roar_in_l, g, 0.75f);
      const float roar_r = roar_r_.Lowpass(roar_in_r, g, 0.75f);
      const float roar_amp = (0.035f + 0.30f * p2 + 0.22f * ab_) * Smooth(0.0f, 0.25f, rpm + ab_);

      // ---- rumble: brown noise, low-passed, with the afterburner's pulsing
      brown_ = brown_ * 0.996f + common * 0.06f;
      rumble_ += (brown_ - rumble_) * (2.0f * 3.14159265f * 110.0f * dt);
      if ((i & 255) == 0) pulse_target_ = 0.75f + 0.25f * Noise();
      pulse_ += (pulse_target_ - pulse_) * 0.002f;
      const float rumble = rumble_ * (0.25f * p2 + 0.9f * ab_ * pulse_);

      // ---- crackle: sparse pops while the afterburner burns
      if (ab_ > 0.05f && Uniform() < ab_ * 55.0f * dt) crack_ = 0.6f + 0.4f * Uniform();
      crack_ *= 0.9935f;  // (about 3 ms)
      const float crack = crack_l_.Bandpass(Noise() * crack_, 0.11f, 1.4f) * 0.55f * ab_;

      // ---- hiss: air
      const float hiss_in = Noise();
      hiss_lp_ += (hiss_in - hiss_lp_) * 0.45f;
      const float hiss = (hiss_in - hiss_lp_) * (0.006f + 0.020f * rpm) * Smooth(0.0f, 0.1f, rpm);

      // ---- cannon: a crack of noise and a low thump per round
      float gun = 0.0f;
      if (shot_ > 1e-4f || thump_ > 1e-4f) {
        gun = gun_lp_.Lowpass(Noise() * shot_, 0.35f, 0.9f) * 0.55f;
        thump_ph_ = Wrap(thump_ph_ + 85.0f * dt);
        gun += Sin(thump_ph_) * thump_ * 0.35f;
        shot_ *= 0.9965f;   // (~6 ms)
        thump_ *= 0.9990f;  // (~20 ms)
      }
      // ---- missile launch: a falling whoosh
      float whoosh = 0.0f;
      if (launch_ > 1e-4f) {
        const float sweep = 0.02f + 0.18f * launch_;
        whoosh = launch_lp_.Bandpass(Noise(), sweep, 1.1f) * launch_ * 0.9f;
        launch_ *= 0.99993f;  // (~0.3 s)
      }

      // ---- lock-on tone: two 70 ms beeps at 1.25 kHz
      float beep = 0.0f;
      if (beep_t_ < 0.2f) {
        beep_ph_ = Wrap(beep_ph_ + 1250.0f * dt);
        const bool on = beep_t_ < 0.07f || (beep_t_ > 0.11f && beep_t_ < 0.18f);
        beep = on ? Sin(beep_ph_) * 0.10f : 0.0f;
        beep_t_ += dt;
      }
      const float engine = whine + rumble + crack;
      float l = ((engine + roar_l * roar_amp + hiss) * vol_ + gun + whoosh) * 1.6f;
      float r = ((engine + roar_r * roar_amp + hiss) * vol_ + gun + whoosh) * 1.6f;
      l = std::tanh(l * 1.2f) * 0.85f + beep;
      r = std::tanh(r * 1.2f) * 0.85f + beep;
      lr[2 * i] = l;
      lr[2 * i + 1] = r;
    }
    rpm_out_.store(rpm_, std::memory_order_relaxed);
    quiet_.store(target <= 0.0f && rpm_ < 0.01f && ab_ < 0.01f && launch_ < 1e-3f && shot_ < 1e-3f &&
                     beep_t_ >= 0.2f,
                 std::memory_order_relaxed);
  }

 private:
  // A state-variable filter (Chamberlin/Simper, one stage).
  struct Svf {
    float ic1 = 0, ic2 = 0;
    float Lowpass(float x, float g, float k) {  // k: 1/Q
      const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
      const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2 * v1 - ic1, ic2 = 2 * v2 - ic2;
      return v2;
    }
    float Bandpass(float x, float g, float k) {
      const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
      const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2 * v1 - ic1, ic2 = 2 * v2 - ic2;
      return v1;
    }
  };
  static float Wrap(float p) { return p - std::floor(p); }
  static float Sin(float p) { return std::sin(6.28318531f * p); }
  static float Smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
  }
  float Uniform() {  // [0, 1)
    seed_ = seed_ * 1664525u + 1013904223u;
    return float(seed_ >> 8) * (1.0f / 16777216.0f);
  }
  float Noise() { return Uniform() * 2.0f - 1.0f; }

  std::atomic<float> target_spool_{0}, target_ab_{0}, target_volume_{0};
  std::atomic<uint32_t> shots_{0}, launches_{0}, beeps_{0};
  std::atomic<bool> quiet_{true};
  std::atomic<float> rpm_out_{0};
  // (the mixer thread's own)
  float rpm_ = 0, ab_ = 0, vol_ = 0, wander_ = 0;
  float ph1_ = 0, ph1b_ = 0.37f, ph2_ = 0, ph3_ = 0, phf_ = 0, thump_ph_ = 0;
  float brown_ = 0, rumble_ = 0, pulse_ = 1, pulse_target_ = 1, crack_ = 0, hiss_lp_ = 0;
  float shot_ = 0, thump_ = 0, launch_ = 0, beep_t_ = 1.0f, beep_ph_ = 0;
  uint32_t seed_ = 0x9E3779B9u;
  Svf roar_l_, roar_r_, crack_l_, gun_lp_, launch_lp_;
};

}  // namespace gta4::jet
