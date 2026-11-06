// LibertyRecomp Menu: the Beyond suit's sounds, synthesized (v1.7.46).
//
// Nothing sampled: every sound is made here, built to how the suit works in
// the show (the user's reference stills) and to what the game needs to hear.
//
//  - thrusters: the boot jets. A hiss (band-passed noise) whose centre and
//    level follow the thrust, a low whistle under it that climbs with
//    thrust (two sines a few cents apart, so they beat), a crackle of
//    combustion grains when the thrust changes fast. Spooled: the jets chase
//    the thrust target with a short lag (a tenth of a second up, slower
//    down), so lifts and drops are heard as such.
//  - rush: the air past the suit, with its speed (band-passed noise moving
//    up in pitch and level as he goes faster); in a glide it is the main
//    sound, a broad soft wash with the wings' flutter in it (a slow
//    tremolo).
//  - wings: a snap when they unfold (a short bright transient and a flap of
//    low noise, then the membrane tightening: a sine chirp down), a softer
//    fold.
//  - grapnel: the launcher's pop, the line paying out (a ratchet: a buzz
//    whose rate climbs as the line runs), the latch (a short metal ring),
//    the reel pulling him in (a whine rising with speed).
//  - cloak: the shimmer the Batmobile's uses (rising on, falling off).
//  - landing: a thud (a low damped sine) with a dust puff of noise, scaled
//    by the fall.
//  - suit up / off: a servo whir and a seal hiss (the suit closing round him).
//  - the energy's low warning: two soft beeps.
//
// Plain C++ with no platform code: the game thread sets targets (atomics),
// the mixer's thread renders.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gta4::suit {

class SuitSynth {
 public:
  static constexpr float kRate = 48000.0f;

  // thrust 0..1 (the boot jets), speed m/s through the air, glide 0..1
  // (the wings out: the rush broadens, the flutter comes in), volume 0..1.
  void SetTarget(float thrust, float speed, float glide, float volume) {
    target_thrust_.store(std::clamp(thrust, 0.0f, 1.0f), std::memory_order_relaxed);
    target_speed_.store(std::max(speed, 0.0f), std::memory_order_relaxed);
    target_glide_.store(std::clamp(glide, 0.0f, 1.0f), std::memory_order_relaxed);
    target_volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
  }
  void Wings(bool open) { wings_open_.store(open, std::memory_order_relaxed), wings_.fetch_add(1); }
  void GrapnelFire() { grapnel_fires_.fetch_add(1, std::memory_order_relaxed); }
  void GrapnelLatch() { grapnel_latches_.fetch_add(1, std::memory_order_relaxed); }
  // the reel: 0 off, else how fast he is pulled (m/s)
  void Reel(float speed) { reel_.store(std::max(speed, 0.0f), std::memory_order_relaxed); }
  void Cloak(bool on) { cloak_on_.store(on, std::memory_order_relaxed), cloaks_.fetch_add(1); }
  // how hard he came down, m/s (0.5 a step, 12 a drop from a roof)
  void Land(float impact) { land_impact_.store(std::clamp(impact, 0.0f, 20.0f), std::memory_order_relaxed), lands_.fetch_add(1); }
  void Suit(bool on) { suit_on_.store(on, std::memory_order_relaxed), suits_.fetch_add(1); }
  void Warn() { warns_.fetch_add(1, std::memory_order_relaxed); }
  bool Quiet() const { return quiet_.load(std::memory_order_relaxed); }

  // Interleaved stereo, frames at kRate, overwritten.
  void Render(float* lr, size_t frames) {
    const float thrust_target = target_thrust_.load(std::memory_order_relaxed);
    const float speed_target = target_speed_.load(std::memory_order_relaxed);
    const float glide_target = target_glide_.load(std::memory_order_relaxed);
    const float vol_target = target_volume_.load(std::memory_order_relaxed);
    const float reel_target = reel_.load(std::memory_order_relaxed);
    if (wings_.exchange(0, std::memory_order_relaxed))
      wing_t_ = 0.0f, wing_opening_ = wings_open_.load(std::memory_order_relaxed);
    for (uint32_t n = grapnel_fires_.exchange(0, std::memory_order_relaxed); n; --n) fire_t_ = 0.0f, line_t_ = 0.0f;
    for (uint32_t n = grapnel_latches_.exchange(0, std::memory_order_relaxed); n; --n) latch_t_ = 0.0f, line_t_ = 1e3f;
    if (cloaks_.exchange(0, std::memory_order_relaxed))
      cloak_t_ = 0.0f, cloak_on_now_ = cloak_on_.load(std::memory_order_relaxed);
    if (lands_.exchange(0, std::memory_order_relaxed))
      land_t_ = 0.0f, land_now_ = land_impact_.load(std::memory_order_relaxed);
    if (suits_.exchange(0, std::memory_order_relaxed))
      suit_t_ = 0.0f, suit_on_now_ = suit_on_.load(std::memory_order_relaxed);
    for (uint32_t n = warns_.exchange(0, std::memory_order_relaxed); n; --n) warn_t_ = 0.0f;

    constexpr float dt = 1.0f / kRate;
    constexpr float kPi = 3.14159265f;
    for (size_t i = 0; i < frames; ++i) {
      // ---- the jets, the speed, the glide (smoothed)
      const float d = thrust_target - thrust_;
      thrust_ += (d > 0 ? std::min(d * 10.0f, 4.0f) : std::max(d * 4.0f, -2.5f)) * dt;
      const float change = std::fabs(d) * 10.0f;
      speed_ += (speed_target - speed_) * 3.0f * dt;
      glide_ += (glide_target - glide_) * 4.0f * dt;
      vol_ += (vol_target - vol_) * 8.0f * dt;
      reel_now_ += (reel_target - reel_now_) * 6.0f * dt;
      const float th = std::clamp(thrust_, 0.0f, 1.0f);
      const float sp = std::clamp(speed_ / 45.0f, 0.0f, 1.5f);

      // ---- thrusters: hiss, whistle, crackle
      const float na = Noise(), nb = Noise(), nc = Noise();
      const float hiss_c = 1800.0f + 3800.0f * th;
      const float hiss = jet_l_.Bandpass(0.7f * na + 0.3f * nb, std::tan(kPi * std::min(hiss_c, 9000.0f) * dt), 0.9f);
      const float hiss_r = jet_r_.Bandpass(0.7f * na + 0.3f * nc, std::tan(kPi * std::min(hiss_c * 1.03f, 9000.0f) * dt), 0.9f);
      const float fw = 160.0f + 420.0f * std::pow(th, 1.3f);
      pw1_ = Wrap(pw1_ + fw * dt), pw2_ = Wrap(pw2_ + fw * 1.006f * dt), pw3_ = Wrap(pw3_ + fw * 2.0f * dt);
      const float whistle = (0.55f * Sin(pw1_) + 0.45f * Sin(pw2_) + 0.25f * Sin(pw3_)) * 0.05f * Smooth(0.02f, 0.3f, th);
      // crackle: combustion grains while the thrust moves, and a little at full
      if ((i & 15) == 0) grain_ = Uniform() < (0.02f + 0.25f * std::min(change, 1.0f) + 0.04f * th) ? Noise() : 0.0f;
      const float crackle = crackle_lp_.Lowpass(grain_, 0.12f, 0.8f) * 0.18f * Smooth(0.0f, 0.1f, th);
      const float jet_amp = (0.015f + 0.20f * th * th) * Smooth(0.0f, 0.05f, th);
      const float jets_l = (hiss * jet_amp + whistle + crackle);
      const float jets_r = (hiss_r * jet_amp + whistle + crackle);

      // ---- rush: the air past him; in a glide broad and soft, fluttering
      flut_ph_ = Wrap(flut_ph_ + (4.0f + 6.0f * sp) * dt);
      const float flutter = 1.0f - glide_ * 0.35f * (0.5f + 0.5f * Sin(flut_ph_));
      const float centre = std::min(350.0f + 2400.0f * sp - 800.0f * glide_ * sp, 7000.0f);
      const float q = 1.0f + 1.2f * glide_;  // (wider band in the glide)
      const float rl = rush_l_.Bandpass(0.75f * na + 0.25f * nb, std::tan(kPi * std::max(centre, 120.0f) * dt), q);
      const float rr = rush_r_.Bandpass(0.75f * na + 0.25f * nc, std::tan(kPi * std::max(centre, 120.0f) * dt), q);
      const float rush_amp = (0.004f + 0.16f * sp * sp) * (1.0f + 0.6f * glide_) * flutter;

      // ---- one-shots
      float fx = 0.0f;
      // the wings: a snap and a flap, then the membrane tightening (open); a softer fold
      if (wing_t_ < 0.8f) {
        const float t = wing_t_;
        const float snap = std::exp(-t * 60.0f) * (wing_opening_ ? 0.9f : 0.35f);
        const float flap = flap_lp_.Lowpass(Noise(), 0.05f, 0.6f) * std::exp(-t * 9.0f) * (wing_opening_ ? 0.5f : 0.3f);
        float chirp = 0.0f;
        if (wing_opening_ && t > 0.05f) {
          const float k = (t - 0.05f) / 0.5f;
          const float fc = 1400.0f * std::pow(0.3f, std::min(k, 1.0f));
          wing_ph_ = Wrap(wing_ph_ + fc * dt);
          chirp = Sin(wing_ph_) * 0.08f * std::exp(-k * 4.0f);
        }
        fx += snap_hp_.Bandpass(Noise(), 0.5f, 0.5f) * snap * 0.25f + flap + chirp;
        wing_t_ += dt;
      }
      // the grapnel: the pop out of the launcher
      if (fire_t_ < 0.3f) {
        fx += pop_bp_.Bandpass(Noise(), 0.25f, 0.5f) * std::exp(-fire_t_ * 35.0f) * 0.7f;
        fire_t_ += dt;
      }
      // the line paying out: a ratchet buzz whose rate climbs
      if (line_t_ < 2.5f) {
        const float rate = 28.0f + 90.0f * std::min(line_t_ / 0.8f, 1.0f);
        line_ph_ = Wrap(line_ph_ + rate * dt);
        const float click = line_ph_ < 0.08f ? 1.0f - line_ph_ / 0.08f : 0.0f;
        fx += line_bp_.Bandpass(click * Noise(), 0.35f, 0.6f) * 0.12f * Smooth(0.0f, 0.03f, line_t_);
        line_t_ += dt;
      }
      // the latch: a short metal ring
      if (latch_t_ < 0.4f) {
        latch_ph_ = Wrap(latch_ph_ + 2900.0f * dt), latch_ph2_ = Wrap(latch_ph2_ + 4340.0f * dt);
        fx += (Sin(latch_ph_) * 0.6f + Sin(latch_ph2_) * 0.4f) * std::exp(-latch_t_ * 14.0f) * 0.12f +
              latch_bp_.Bandpass(Noise(), 0.4f, 0.4f) * std::exp(-latch_t_ * 50.0f) * 0.3f;
        latch_t_ += dt;
      }
      // the reel: a whine with the pull
      if (reel_now_ > 0.2f) {
        const float fr = 500.0f + 60.0f * reel_now_;
        reel_ph_ = Wrap(reel_ph_ + fr * dt), reel_ph2_ = Wrap(reel_ph2_ + fr * 2.01f * dt);
        fx += (Sin(reel_ph_) * 0.6f + Sin(reel_ph2_) * 0.3f) * 0.05f * std::min(reel_now_ / 8.0f, 1.0f) +
              reel_bp_.Bandpass(Noise(), std::tan(kPi * fr * 2.0f * dt), 0.5f) * 0.04f;
      }
      // the cloak: a shimmer rising (on) or falling (off), a fizzing tail
      if (cloak_t_ < 1.4f) {
        const float k = cloak_t_ / 1.4f;
        const float fs = cloak_on_now_ ? 300.0f * std::pow(12.0f, k) : 3600.0f * std::pow(1.0f / 12.0f, k);
        cloak_ph_ = Wrap(cloak_ph_ + fs * dt), cloak_ph2_ = Wrap(cloak_ph2_ + fs * 1.33f * dt);
        const float env = Smooth(0.0f, 0.08f, cloak_t_) * (1.0f - Smooth(0.9f, 1.4f, cloak_t_));
        const float shimmer = Sin(cloak_ph_) * Sin(cloak_ph2_) * (0.6f + 0.4f * Sin(cloak_ph_ * 0.071f));
        if ((i & 7) == 0) fizz_ = Noise();
        fx += (shimmer * 0.05f + cloak_bp_.Bandpass(fizz_, 0.45f, 0.6f) * 0.03f * k) * env;
        cloak_t_ += dt;
      }
      // the landing: a thud and a puff, with the fall
      if (land_t_ < 0.6f) {
        const float w = std::min(land_now_ / 12.0f, 1.2f);
        land_ph_ = Wrap(land_ph_ + (55.0f + 20.0f * w) * dt);
        fx += Sin(land_ph_) * std::exp(-land_t_ * 18.0f) * 0.5f * w +
              puff_lp_.Lowpass(Noise(), 0.08f, 0.7f) * std::exp(-land_t_ * 10.0f) * 0.35f * w;
        land_t_ += dt;
      }
      // suiting up: a servo whir, then the seal's hiss; off: the hiss, then the whir down
      if (suit_t_ < 1.3f) {
        const float t = suit_t_;
        const float whir_env = suit_on_now_ ? Smooth(0.0f, 0.05f, t) * (1.0f - Smooth(0.7f, 0.9f, t))
                                            : Smooth(0.3f, 0.35f, t) * (1.0f - Smooth(1.1f, 1.3f, t));
        const float fm = suit_on_now_ ? 220.0f + 160.0f * std::min(t / 0.8f, 1.0f) : 380.0f - 160.0f * std::min(t / 1.3f, 1.0f);
        suit_ph_ = Wrap(suit_ph_ + fm * dt);
        fx += suit_lp_.Lowpass(2.0f * suit_ph_ - 1.0f, 0.06f, 0.7f) * 0.10f * whir_env;
        const float h0 = suit_on_now_ ? 0.85f : 0.0f;
        if (t > h0) {
          const float h = t - h0;
          fx += suit_hiss_.Bandpass(Noise(), 0.5f, 0.35f) * 0.14f * std::exp(-h * 6.0f) * Smooth(0.0f, 0.01f, h);
        }
        suit_t_ += dt;
      }
      // the warning: two soft beeps
      if (warn_t_ < 0.5f) {
        const float on = (warn_t_ < 0.12f) || (warn_t_ > 0.24f && warn_t_ < 0.36f) ? 1.0f : 0.0f;
        warn_ph_ = Wrap(warn_ph_ + 1760.0f * dt);
        fx += Sin(warn_ph_) * 0.05f * on;
        warn_t_ += dt;
      }

      const float l = (jets_l + rl * rush_amp) * vol_ + fx;
      const float r = (jets_r + rr * rush_amp) * vol_ + fx;
      lr[2 * i] = std::tanh(l * 2.4f) * 0.85f;
      lr[2 * i + 1] = std::tanh(r * 2.4f) * 0.85f;
    }
    quiet_.store(thrust_target <= 0.0f && thrust_ < 0.01f && speed_target < 0.5f && speed_ < 0.5f &&
                     reel_target <= 0.0f && wing_t_ >= 0.8f && fire_t_ >= 0.3f && line_t_ >= 2.5f && latch_t_ >= 0.4f &&
                     cloak_t_ >= 1.4f && land_t_ >= 0.6f && suit_t_ >= 1.3f && warn_t_ >= 0.5f &&
                     wings_.load(std::memory_order_relaxed) == 0 && cloaks_.load(std::memory_order_relaxed) == 0 &&
                     lands_.load(std::memory_order_relaxed) == 0 && suits_.load(std::memory_order_relaxed) == 0,
                 std::memory_order_relaxed);
  }

 private:
  struct Svf {
    float ic1 = 0, ic2 = 0;
    float Lowpass(float x, float g, float k) {
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
  float Uniform() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return float(seed_ >> 8) * (1.0f / 16777216.0f);
  }
  float Noise() { return Uniform() * 2.0f - 1.0f; }

  std::atomic<float> target_thrust_{0}, target_speed_{0}, target_glide_{0}, target_volume_{0}, reel_{0};
  std::atomic<float> land_impact_{0};
  std::atomic<bool> wings_open_{true}, cloak_on_{true}, suit_on_{true};
  std::atomic<uint32_t> wings_{0}, grapnel_fires_{0}, grapnel_latches_{0}, cloaks_{0}, lands_{0}, suits_{0}, warns_{0};
  std::atomic<bool> quiet_{true};
  float thrust_ = 0, speed_ = 0, glide_ = 0, vol_ = 0, reel_now_ = 0;
  float pw1_ = 0, pw2_ = 0.3f, pw3_ = 0.6f, grain_ = 0, flut_ph_ = 0;
  float wing_t_ = 1e3f, wing_ph_ = 0, fire_t_ = 1e3f, line_t_ = 1e3f, line_ph_ = 0, latch_t_ = 1e3f, latch_ph_ = 0,
        latch_ph2_ = 0, reel_ph_ = 0, reel_ph2_ = 0, cloak_t_ = 1e3f, cloak_ph_ = 0, cloak_ph2_ = 0, fizz_ = 0,
        land_t_ = 1e3f, land_now_ = 0, land_ph_ = 0, suit_t_ = 1e3f, suit_ph_ = 0, warn_t_ = 1e3f, warn_ph_ = 0;
  bool wing_opening_ = true, cloak_on_now_ = true, suit_on_now_ = true;
  uint32_t seed_ = 0x9E3779B9u;
  Svf jet_l_, jet_r_, crackle_lp_, rush_l_, rush_r_, flap_lp_, snap_hp_, pop_bp_, line_bp_, latch_bp_, reel_bp_,
      cloak_bp_, puff_lp_, suit_lp_, suit_hiss_;
};

}  // namespace gta4::suit
