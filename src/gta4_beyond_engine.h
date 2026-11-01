// LibertyRecomp Menu: the Batman Beyond Batmobile's sound, synthesized.
//
// Built to the show's car as heard in Return of the Joker (the user's clip,
// measured, nothing taken from it): a smooth turbine whine whose third
// harmonic rings out high (about 2.9 kHz cruising; 2.2 -> 3.1 kHz as it
// spools up leaving the Batcave), a deep pulsing thrum under it (about
// 42-74 Hz, with an octave below), and air rushing past that grows with
// speed. A fly-by drops the whole sound about two semitones (Doppler).
//
//  - spool: the turbines chase their target with a lag (a second up,
//    slower down); hovering it idles at about a third.
//  - whine: twin turbines a few cents apart (the two booms), partials 1-5,
//    the third strongest; a slow vibrato.
//  - thrum: the hover field's hum, its pitch rising with thrust, pulsing.
//  - rush: band-passed noise, centre and level with speed.
//  - boost: the whine pushed up and brighter, a burst of air.
//  - Doppler and distance for when it is heard from outside (summoned, or
//    hovering after Terry dropped out of it): every pitch scaled, far away
//    duller and quieter.
//  - events: the floor hatch and the canopy (a servo's whir and the
//    hydraulic air release), the beam that drops the player through the
//    hatch or lifts him in, the cloak's shimmer, a missile's whoosh.
//
// Plain C++ with no platform code: the game thread sets targets (atomics),
// the mixer's thread renders. tools/beyond/render_preview.cpp renders it to
// a file on a PC.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gta4::beyond {

class EngineSynth {
 public:
  static constexpr float kRate = 48000.0f;
  static constexpr float kIdle = 0.30f;

  // spool: the turbines' target (0 off, kIdle hovering, 1 full thrust);
  // speed: its speed through the air, m/s; boost 0..1; volume 0..1.
  void SetTarget(float spool, float speed, float boost, float volume) {
    target_spool_.store(std::clamp(spool, 0.0f, 1.0f), std::memory_order_relaxed);
    target_speed_.store(std::max(speed, 0.0f), std::memory_order_relaxed);
    target_boost_.store(std::clamp(boost, 0.0f, 1.0f), std::memory_order_relaxed);
    target_volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
  }
  // Heard from outside: how far (m) and how fast it comes closer (m/s, +
  // closing); inside: SetOutside(false).
  void SetListener(bool outside, float distance, float closing) {
    outside_.store(outside, std::memory_order_relaxed);
    distance_.store(std::max(distance, 1.0f), std::memory_order_relaxed);
    closing_.store(std::clamp(closing, -150.0f, 150.0f), std::memory_order_relaxed);
  }
  void Hatch(bool opening) { hatch_open_.store(opening, std::memory_order_relaxed), hatches_.fetch_add(1); }
  void Canopy(bool opening) { canopy_open_.store(opening, std::memory_order_relaxed), canopies_.fetch_add(1); }
  void Beam(bool down) { beam_down_.store(down, std::memory_order_relaxed), beams_.fetch_add(1); }
  void Cloak(bool on) { cloak_on_.store(on, std::memory_order_relaxed), cloaks_.fetch_add(1); }
  void Launch() { launches_.fetch_add(1, std::memory_order_relaxed); }
  bool Quiet() const { return quiet_.load(std::memory_order_relaxed); }
  float Rpm() const { return rpm_out_.load(std::memory_order_relaxed); }

  // Interleaved stereo, frames at kRate, overwritten (not mixed into).
  void Render(float* lr, size_t frames) {
    const float target = target_spool_.load(std::memory_order_relaxed);
    const float speed_target = target_speed_.load(std::memory_order_relaxed);
    const float boost_target = target_boost_.load(std::memory_order_relaxed);
    const float vol_target = target_volume_.load(std::memory_order_relaxed);
    const bool outside = outside_.load(std::memory_order_relaxed);
    const float dist_target = outside ? distance_.load(std::memory_order_relaxed) : 1.0f;
    const float closing_target = outside ? closing_.load(std::memory_order_relaxed) : 0.0f;
    for (uint32_t n = launches_.exchange(0, std::memory_order_relaxed); n; --n) launch_ = 1.0f;
    if (hatches_.exchange(0, std::memory_order_relaxed))
      hatch_t_ = 0.0f, hatch_opening_ = hatch_open_.load(std::memory_order_relaxed);
    if (canopies_.exchange(0, std::memory_order_relaxed))
      canopy_t_ = 0.0f, canopy_opening_ = canopy_open_.load(std::memory_order_relaxed);
    if (beams_.exchange(0, std::memory_order_relaxed))
      beam_t_ = 0.0f, beam_down_now_ = beam_down_.load(std::memory_order_relaxed);
    if (cloaks_.exchange(0, std::memory_order_relaxed))
      cloak_t_ = 0.0f, cloak_on_now_ = cloak_on_.load(std::memory_order_relaxed);

    constexpr float dt = 1.0f / kRate;
    constexpr float kPi = 3.14159265f;
    for (size_t i = 0; i < frames; ++i) {
      // ---- spool, speed, boost, the listener (smoothed)
      const float d = target - rpm_;
      rpm_ += (d > 0 ? std::min(d * 1.1f, 0.65f) : std::max(d * 0.55f, -0.35f)) * dt;
      speed_ += (speed_target - speed_) * 2.5f * dt;
      boost_ += (boost_target - boost_) * (boost_target > boost_ ? 5.0f : 1.8f) * dt;
      vol_ += (vol_target - vol_) * 8.0f * dt;
      dist_ += (dist_target - dist_) * 6.0f * dt;
      closing_now_ += (closing_target - closing_now_) * 6.0f * dt;
      const float rpm = std::max(rpm_, 0.0f);
      // Doppler: c / (c - v) for a source closing at v; 1/r beyond 6 m.
      const float doppler = outside ? 343.0f / (343.0f - std::clamp(closing_now_, -120.0f, 120.0f)) : 1.0f;
      const float near = outside ? std::min(1.0f, 6.0f / dist_) : 1.0f;
      const float air_cut = outside ? std::clamp(16000.0f * 25.0f / (25.0f + dist_), 1500.0f, 16000.0f) : 16000.0f;
      const float sp = std::clamp(speed_ / 70.0f, 0.0f, 1.5f);

      // ---- whine: twin turbines, the third partial ringing high
      if ((i & 63) == 0) wander_ += (Noise() * 0.5f - wander_ * 0.02f) * 0.01f;
      vib_ph_ = Wrap(vib_ph_ + 4.6f * dt);
      const float f1 = (430.0f + 640.0f * std::pow(rpm, 1.15f)) * (1.0f + 0.16f * boost_) *
                       (1.0f + 0.0035f * Sin(vib_ph_) + 0.004f * wander_) * doppler;
      const float fa = f1, fb = f1 * 1.0042f;  // (the other boom's turbine: they beat)
      pa1_ = Wrap(pa1_ + fa * dt), pa2_ = Wrap(pa2_ + 2.0f * fa * dt), pa3_ = Wrap(pa3_ + 3.0f * fa * dt);
      pa4_ = Wrap(pa4_ + 4.0f * fa * dt), pa5_ = Wrap(pa5_ + 5.01f * fa * dt);
      pb1_ = Wrap(pb1_ + fb * dt), pb3_ = Wrap(pb3_ + 3.0f * fb * dt);
      const float bright = 0.85f + 0.35f * boost_;
      const float turbine_a = 0.50f * Sin(pa1_) + 0.22f * Sin(pa2_) + 0.80f * bright * Sin(pa3_) +
                              0.14f * Sin(pa4_) + 0.10f * bright * Sin(pa5_);
      const float turbine_b = 0.45f * Sin(pb1_) + 0.70f * bright * Sin(pb3_);
      const float whine_amp = 0.055f * Smooth(0.02f, 0.4f, rpm) * (0.55f + 0.45f * rpm) * (1.0f + 0.3f * boost_);
      const float whine_l = (0.62f * turbine_a + 0.38f * turbine_b) * whine_amp;
      const float whine_r = (0.38f * turbine_a + 0.62f * turbine_b) * whine_amp;

      // ---- thrum: the hover field's deep pulsing hum, rising with thrust
      const float fh = (44.0f + 30.0f * rpm) * (1.0f + 0.06f * boost_) * doppler;
      ph_h_ = Wrap(ph_h_ + fh * dt), ph_hs_ = Wrap(ph_hs_ + 0.5f * fh * dt);
      ph_am_ = Wrap(ph_am_ + (5.5f + 2.5f * rpm) * dt);
      const float am = 0.80f + 0.20f * Sin(ph_am_);
      const float thrum_tone = Sin(ph_h_) + 0.50f * Sin(2.0f * ph_h_) + 0.25f * Sin(3.0f * ph_h_) + 0.28f * Sin(ph_hs_);
      const float thrum_amp = 0.06f * Smooth(0.0f, 0.25f, rpm) * am * (1.0f - 0.35f * std::min(sp, 1.0f));
      const float thrum = thrum_tone * thrum_amp;

      // ---- rush: air past it, its band and level with speed (and boost)
      const float nc = Noise(), nl = Noise(), nr = Noise();
      const float centre = std::min((450.0f + 3200.0f * sp + 1500.0f * boost_) * doppler, 9000.0f);
      const float g = std::tan(kPi * centre * dt);
      const float rush_l = rush_l_.Bandpass(0.75f * nc + 0.25f * nl, g, 1.1f);
      const float rush_r = rush_r_.Bandpass(0.75f * nc + 0.25f * nr, g, 1.1f);
      const float rush_amp = (0.010f + 0.12f * sp * sp + 0.10f * boost_) * Smooth(0.0f, 0.2f, rpm + sp);
      // the turbines' own breath under the whine
      const float breath = breath_lp_.Lowpass(nc, std::tan(kPi * std::min(900.0f + 1800.0f * rpm, 8000.0f) * dt), 0.9f) *
                           0.025f * rpm;

      // ---- one-shots: the missile's whoosh
      float fx = 0.0f;
      if (launch_ > 1e-4f) {
        fx += launch_bp_.Bandpass(Noise(), 0.03f + 0.20f * launch_, 1.1f) * launch_ * 0.8f;
        launch_ *= 0.99993f;
      }
      // the floor hatch / the canopy: a servo's whir, then the air release
      auto door = [&](float& t, bool opening, float len, Svf& motor_lp, Svf& hiss_hp, float& ph) {
        if (t > len + 1.0f)
          return 0.0f;
        float out = 0.0f;
        const float env = Smooth(0.0f, 0.06f, t) * (1.0f - Smooth(len - 0.1f, len, t));
        if (env > 0.0f) {
          const float k = std::min(t / len, 1.0f);
          const float fm = opening ? 170.0f + 110.0f * k : 280.0f - 110.0f * k;
          ph = Wrap(ph + fm * dt);
          out += motor_lp.Lowpass(2.0f * ph - 1.0f, 0.06f, 0.7f) * 0.10f * env;
        }
        // the hiss: opening at the start, closing as it seals
        const float h0 = opening ? 0.02f : len - 0.05f;
        if (t > h0) {
          const float h = (t - h0);
          const float hiss_env = std::exp(-h * 5.5f) * Smooth(0.0f, 0.015f, h);
          out += hiss_hp.Bandpass(Noise(), 0.55f, 0.35f) * 0.16f * hiss_env;
        }
        t += dt;
        return out;
      };
      fx += door(hatch_t_, hatch_opening_, 0.45f, hatch_motor_, hatch_hiss_, hatch_ph_);
      fx += door(canopy_t_, canopy_opening_, 0.9f, canopy_motor_, canopy_hiss_, canopy_ph_);
      // the beam: a shimmer sweeping down (dropping out) or up (lifted in)
      if (beam_t_ < 0.9f) {
        const float k = beam_t_ / 0.9f;
        const float fsw = beam_down_now_ ? 2600.0f * std::pow(0.25f, k) : 650.0f * std::pow(4.0f, k);
        beam_ph_ = Wrap(beam_ph_ + fsw * dt), beam_ph2_ = Wrap(beam_ph2_ + fsw * 1.51f * dt);
        const float env = Smooth(0.0f, 0.05f, beam_t_) * (1.0f - Smooth(0.55f, 0.9f, beam_t_));
        const float ring = Sin(beam_ph_) * Sin(beam_ph2_ + 0.25f * Sin(beam_ph_ * 7.0f));
        fx += (ring * 0.06f + beam_bp_.Bandpass(Noise(), std::tan(kPi * fsw * dt), 0.25f) * 0.08f) * env;
        beam_t_ += dt;
      }
      // the cloak: a shimmer rising (on) or falling (off), a fizzing tail
      if (cloak_t_ < 1.4f) {
        const float k = cloak_t_ / 1.4f;
        const float fs = cloak_on_now_ ? 300.0f * std::pow(12.0f, k) : 3600.0f * std::pow(1.0f / 12.0f, k);
        cloak_ph_ = Wrap(cloak_ph_ + fs * dt), cloak_ph2_ = Wrap(cloak_ph2_ + fs * 1.33f * dt);
        const float env = Smooth(0.0f, 0.08f, cloak_t_) * (1.0f - Smooth(0.9f, 1.4f, cloak_t_));
        const float shimmer = Sin(cloak_ph_) * Sin(cloak_ph2_) * (0.6f + 0.4f * Sin(cloak_ph_ * 0.071f));
        // the fizz: noise held in steps (a crushed, digital grain)
        if ((i & 7) == 0) fizz_ = Noise();
        fx += (shimmer * 0.05f + cloak_bp_.Bandpass(fizz_, 0.45f, 0.6f) * 0.03f * k) * env;
        cloak_t_ += dt;
      }

      // ---- out: the engine through the listener (distance, air), the rest near
      const float eng_l = (whine_l + thrum + rush_l * rush_amp + breath) * vol_ * near;
      const float eng_r = (whine_r + thrum + rush_r * rush_amp + breath) * vol_ * near;
      const float ga = std::tan(kPi * std::min(air_cut, 18000.0f) * dt);
      const float out_l = air_l_.Lowpass(eng_l, ga, 1.2f);
      const float out_r = air_r_.Lowpass(eng_r, ga, 1.2f);
      float l = (out_l + fx) * 2.2f, r = (out_r + fx) * 2.2f;
      lr[2 * i] = std::tanh(l * 1.1f) * 0.85f;
      lr[2 * i + 1] = std::tanh(r * 1.1f) * 0.85f;
    }
    rpm_out_.store(rpm_, std::memory_order_relaxed);
    quiet_.store(target <= 0.0f && rpm_ < 0.01f && launch_ < 1e-3f && hatch_t_ > 1.5f && canopy_t_ > 2.0f &&
                     beam_t_ >= 0.9f && cloak_t_ >= 1.4f && hatches_.load(std::memory_order_relaxed) == 0 &&
                     canopies_.load(std::memory_order_relaxed) == 0 && beams_.load(std::memory_order_relaxed) == 0 &&
                     cloaks_.load(std::memory_order_relaxed) == 0,
                 std::memory_order_relaxed);
  }

 private:
  struct Svf {  // a state-variable filter (Simper), one stage
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

  std::atomic<float> target_spool_{0}, target_speed_{0}, target_boost_{0}, target_volume_{0};
  std::atomic<float> distance_{1}, closing_{0};
  std::atomic<bool> outside_{false}, hatch_open_{true}, canopy_open_{true}, beam_down_{true}, cloak_on_{true};
  std::atomic<uint32_t> launches_{0}, hatches_{0}, canopies_{0}, beams_{0}, cloaks_{0};
  std::atomic<bool> quiet_{true};
  std::atomic<float> rpm_out_{0};
  // (the mixer thread's own)
  float rpm_ = 0, speed_ = 0, boost_ = 0, vol_ = 0, dist_ = 1, closing_now_ = 0, wander_ = 0, vib_ph_ = 0;
  float pa1_ = 0, pa2_ = 0, pa3_ = 0, pa4_ = 0, pa5_ = 0, pb1_ = 0.31f, pb3_ = 0.17f;
  float ph_h_ = 0, ph_hs_ = 0, ph_am_ = 0;
  float launch_ = 0, hatch_t_ = 1e3f, canopy_t_ = 1e3f, beam_t_ = 1e3f, cloak_t_ = 1e3f;
  bool hatch_opening_ = true, canopy_opening_ = true, beam_down_now_ = true, cloak_on_now_ = true;
  float hatch_ph_ = 0, canopy_ph_ = 0, beam_ph_ = 0, beam_ph2_ = 0, cloak_ph_ = 0, cloak_ph2_ = 0, fizz_ = 0;
  uint32_t seed_ = 0x2545F491u;
  Svf rush_l_, rush_r_, breath_lp_, launch_bp_, hatch_motor_, hatch_hiss_, canopy_motor_, canopy_hiss_, beam_bp_,
      cloak_bp_, air_l_, air_r_;
};

}  // namespace gta4::beyond
