// Liberty Trainer: the Batmobile's engine, synthesized.
//
// An original voice in the spirit of the 1989/1992 film car (a jet turbine
// over a big, lumpy V8), made from scratch here: nothing in it comes from the
// films or any recording. It plays through the port's own audio mixer
// (ps5/game/sound.h), the car's game name having no engine sound in IV:
//
//  - start: the starter cranks, the V8 catches with a flare, the turbine
//    whines up behind it; shut down, the V8 stops and the turbine spins down.
//  - rumble: the V8's firing pulses (8 cylinders: the firing rate is
//    rpm / 15, ~47 Hz at idle, ~430 Hz at the limit) with a cross-plane
//    crank's uneven beat and a lopey idle, through a low exhaust resonance
//    and a gentle saturation that growls under load.
//  - turbine: two soft tones and a partial, ~0.75 kHz at idle up to ~3.3 kHz,
//    their pitch lagging the engine, a little wander.
//  - intake: band-passed noise opening with throttle and revs, gated by the
//    firing pulses (a rasp).
//  - booster: a deep afterburner roar with sparse crackles.
//  - lift-off: pops and burble from the exhaust when the throttle shuts at
//    high revs.
//  - canopy: a pneumatic hiss opening (a latch click first) and shutting
//    (ending in a clunk); these play with the engine off too.
//
// Plain C++ with no platform code: the game thread sets targets and events
// (atomics), the mixer's thread renders. tools/bat/render_preview.cpp (in
// the trainer repository) renders it to a file.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gta4::bat {

class EngineSynth {
 public:
  static constexpr float kRate = 48000.0f;
  static constexpr float kIdle = 0.16f;  // normalized revs at idle (~700 rpm)

  // rpm: 0 off, kIdle idle, 1 the rev limit (the caller works it out from road
  // speed and gears); throttle and boost 0..1; volume 0..1.
  void SetTarget(float rpm, float throttle, float boost, float volume) {
    target_rpm_.store(std::clamp(rpm, 0.0f, 1.0f), std::memory_order_relaxed);
    target_throttle_.store(std::clamp(throttle, 0.0f, 1.0f), std::memory_order_relaxed);
    target_boost_.store(std::clamp(boost, 0.0f, 1.0f), std::memory_order_relaxed);
    target_volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
  }
  void CanopyOpen() { opens_.fetch_add(1, std::memory_order_relaxed); }
  void CanopyShut() { shuts_.fetch_add(1, std::memory_order_relaxed); }

  // Stopped, spun down and nothing left ringing: the voice can stop.
  bool Quiet() const { return quiet_.load(std::memory_order_relaxed); }
  float Rpm() const { return rpm_out_.load(std::memory_order_relaxed); }

  // Interleaved stereo, frames at kRate, overwritten (not mixed into).
  void Render(float* lr, size_t frames) {
    const float target = target_rpm_.load(std::memory_order_relaxed);
    const float throttle_target = target_throttle_.load(std::memory_order_relaxed);
    const float boost_target = target_boost_.load(std::memory_order_relaxed);
    const float vol_target = target_volume_.load(std::memory_order_relaxed);
    for (uint32_t n = opens_.exchange(0, std::memory_order_relaxed); n; --n) {
      hiss_t_ = 0.0f, hiss_len_ = 0.55f, hiss_rising_ = true;
      click_ = 1.0f;
    }
    for (uint32_t n = shuts_.exchange(0, std::memory_order_relaxed); n; --n) {
      hiss_t_ = 0.0f, hiss_len_ = 0.7f, hiss_rising_ = false;
      clunk_at_ = 0.62f;
    }
    // Starting and stopping.
    if (target > 0.0f && !running_) {
      running_ = true;
      start_t_ = 0.0f;
    } else if (target <= 0.0f && running_) {
      running_ = false;
    }

    constexpr float dt = 1.0f / kRate;
    for (size_t i = 0; i < frames; ++i) {
      // ---- the engine's speed: cranking, catching, then following its target
      float want = target;
      bool cranking = false;
      if (running_ && start_t_ < 1.2f) {
        start_t_ += dt;
        if (start_t_ < 0.55f) {
          cranking = true;
          want = 0.035f;  // the starter turns it over
        } else if (start_t_ < 0.9f) {
          want = std::max(target, kIdle * 1.9f);  // it catches: a flare
        }
      }
      if (!running_)
        want = 0.0f;
      const float d = want - rpm_;
      const float rate = d > 0 ? std::min(d * (cranking ? 4.0f : 7.0f), 3.2f) : std::max(d * 3.2f, -1.6f);
      rpm_ = std::max(rpm_ + rate * dt, 0.0f);
      throttle_ += (throttle_target - throttle_) * 12.0f * dt;
      boost_ += (boost_target - boost_) * (boost_target > boost_ ? 5.0f : 2.0f) * dt;
      vol_ += (vol_target - vol_) * 8.0f * dt;
      // the turbine lags the engine, spins with the booster, spins down slowly
      const float turb_want = running_ ? std::max(rpm_ * 1.1f, 0.6f * boost_ + 0.25f) : 0.0f;
      turb_ += (turb_want - turb_) * (turb_want > turb_ ? 0.9f : 0.6f) * dt;
      const float r = std::clamp((rpm_ - kIdle) / (1.0f - kIdle), 0.0f, 1.0f);
      const float rpm_real = rpm_ >= kIdle ? 700.0f + 5800.0f * r : 700.0f * rpm_ / kIdle;

      // ---- V8 firing pulses (cross-plane: an uneven beat, lopey at idle)
      const float fire = rpm_real / 15.0f;
      fire_ph_ += fire * dt;
      if (fire_ph_ >= 1.0f && rpm_ > 0.004f) {
        fire_ph_ -= std::floor(fire_ph_);
        static constexpr float kBeat[8] = {1.00f, 0.80f, 1.12f, 0.93f, 0.86f, 1.06f, 0.78f, 1.16f};
        const float lope = 0.42f * (1.0f - r) * (1.0f - r);
        pulse_amp_ = kBeat[cyl_] * (1.0f + lope * Noise()) * (0.55f + 0.45f * std::min(1.0f, rpm_ / kIdle));
        cyl_ = (cyl_ + 1) & 7;
        pulse_t_ = 0.0f;
        if (cranking)
          pulse_amp_ *= 0.35f;  // compression thumps, no firing yet
      }
      pulse_t_ += dt;
      const float decay = 40.0f + fire * 5.0f;
      const float pulse = pulse_amp_ * std::exp(-pulse_t_ * decay);
      // the exhaust: a low resonance that rises with the revs, and the
      // fundamental thump, then a saturation that growls with load
      const float res = std::min(0.45f, 3.14159265f * (90.0f + 1.6f * fire) / kRate);
      const float low = thump_lp_.Lowpass(pulse, 3.14159265f * std::min(fire * 1.4f + 30.0f, 700.0f) / kRate,
                                          1.2f);
      const float body = exhaust_bp_.Bandpass(pulse, res, 0.9f);
      const float drive = 1.6f + 2.6f * throttle_ + 1.2f * r;
      float rumble = std::tanh((low * 1.3f + body * 0.9f) * drive) * (0.12f + 0.14f * throttle_ + 0.07f * r);
      // the pulses are one-sided: the DC goes (a 20 Hz high-pass), and the
      // buzz above the exhaust's own range (a low-pass rising with the revs)
      dc_y_ = rumble - dc_x_ + 0.9974f * dc_y_;
      dc_x_ = rumble;
      rumble = rumble_lp_.Lowpass(dc_y_, 3.14159265f * (900.0f + 1700.0f * r) / kRate, 0.9f);
      if (rpm_ <= 0.002f && std::fabs(rumble) < 1e-4f)
        rumble = 0.0f;

      // ---- turbine whine
      const float tf = (750.0f + 2550.0f * std::pow(turb_, 1.15f)) * (1.0f + 0.004f * wander_);
      if ((i & 255) == 0)
        wander_ += (Noise() - wander_) * 0.15f;
      t1_ = Wrap(t1_ + tf * dt);
      t2_ = Wrap(t2_ + tf * 1.618f * dt);
      t3_ = Wrap(t3_ + tf * 2.35f * dt);
      t1r_ = Wrap(t1r_ + tf * 1.003f * dt);
      const float whine_amp = 0.022f * Smooth(0.02f, 0.4f, turb_) * (0.7f + 0.5f * turb_ + 0.4f * boost_);
      const float whine_l = (Sin(t1_) + 0.45f * Sin(t2_) + 0.18f * Sin(t3_)) * whine_amp;
      const float whine_r = (Sin(t1r_) + 0.45f * Sin(t2_) + 0.18f * Sin(t3_)) * whine_amp;

      // ---- intake rasp: noise gated by the firing pulses
      const float rasp_g = 3.14159265f * std::min(400.0f + 1400.0f * r, 3500.0f) / kRate;
      const float rasp_amp = (0.006f + 0.05f * throttle_ * (0.3f + r)) * std::min(1.0f, rpm_ / kIdle);
      const float gate = 0.35f + 0.65f * std::min(1.0f, pulse * 1.5f);
      const float rasp_l = rasp_l_.Bandpass(Noise(), rasp_g, 1.3f) * rasp_amp * gate;
      const float rasp_r = rasp_r_.Bandpass(Noise(), rasp_g * 1.07f, 1.3f) * rasp_amp * gate;

      // ---- booster: a deep roar opening up, and sparse crackles
      const float roar_g = 3.14159265f * (120.0f + 1300.0f * boost_) / kRate;
      const float roar_amp = 0.33f * boost_;
      const float roar_l = roar_l_.Lowpass(Noise(), roar_g, 0.8f) * roar_amp;
      const float roar_r = roar_r_.Lowpass(Noise(), roar_g, 0.8f) * roar_amp;
      float crack = 0.0f;
      if (boost_ > 0.05f && Uniform() < boost_ * 30.0f * dt)
        crack_ = 0.5f + 0.5f * Uniform();
      // lift-off: the throttle shut at high revs pops the exhaust
      if (running_ && throttle_ < 0.15f && r > 0.35f && Uniform() < (r - 0.3f) * 22.0f * dt)
        crack_ = std::max(crack_, 0.35f + 0.45f * Uniform());
      if (crack_ > 1e-4f) {
        crack = crack_bp_.Bandpass(Noise() * crack_, 0.09f, 1.2f) * 0.6f;
        crack_ *= 0.9985f;  // (~15 ms)
      }

      // ---- the canopy: hiss (a latch click first opening, a clunk shutting)
      float canopy = 0.0f;
      if (hiss_t_ < hiss_len_) {
        const float u = hiss_t_ / hiss_len_;
        const float env = std::min(1.0f, hiss_t_ * 40.0f) * (1.0f - u) * (1.0f - u * 0.4f);
        const float centre = hiss_rising_ ? 2600.0f + 1800.0f * u : 4200.0f - 1700.0f * u;
        canopy += hiss_bp_.Bandpass(Noise(), 3.14159265f * centre / kRate, 1.6f) * env * 0.11f;
        hiss_t_ += dt;
        if (clunk_at_ > 0.0f && hiss_t_ >= clunk_at_) {
          clunk_ = 1.0f;
          clunk_at_ = 0.0f;
        }
      }
      if (click_ > 1e-4f) {
        canopy += click_bp_.Bandpass(Noise() * click_, 0.35f, 2.0f) * 0.22f;
        click_ *= 0.996f;  // (~5 ms)
      }
      if (clunk_ > 1e-4f) {
        clunk_ph_ = Wrap(clunk_ph_ + 68.0f * dt);
        canopy += (Sin(clunk_ph_) * 0.30f + clunk_lp_.Lowpass(Noise(), 0.04f, 1.0f) * 0.5f) * clunk_;
        clunk_ *= 0.9993f;  // (~30 ms)
      } else {
        clunk_ph_ = 0.0f;
      }

      const float shared = rumble + crack;
      float l = (shared + whine_l + rasp_l + roar_l) * vol_ + canopy;
      float rr = (shared * 0.97f + whine_r + rasp_r + roar_r) * vol_ + canopy;
      // a soft ceiling at 0.6
      l = 0.6f * std::tanh(l * (1.0f / 0.6f));
      rr = 0.6f * std::tanh(rr * (1.0f / 0.6f));
      lr[2 * i] = l;
      lr[2 * i + 1] = rr;
    }
    rpm_out_.store(rpm_, std::memory_order_relaxed);
    quiet_.store(!running_ && rpm_ < 0.002f && turb_ < 0.01f && boost_ < 0.01f && crack_ < 1e-3f &&
                     hiss_t_ >= hiss_len_ && click_ < 1e-3f && clunk_ < 1e-3f,
                 std::memory_order_relaxed);
  }

 private:
  // A state-variable filter (Chamberlin/Simper, one stage); g = 2 pi f / rate.
  struct Svf {
    float ic1 = 0, ic2 = 0;
    float Lowpass(float x, float g, float k) {  // k: 1/Q
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
    static float Flush(float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; }  // no denormals
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

  std::atomic<float> target_rpm_{0}, target_throttle_{0}, target_boost_{0}, target_volume_{0};
  std::atomic<uint32_t> opens_{0}, shuts_{0};
  std::atomic<bool> quiet_{true};
  std::atomic<float> rpm_out_{0};
  // (the mixer thread's own)
  bool running_ = false;
  float start_t_ = 0, rpm_ = 0, throttle_ = 0, boost_ = 0, vol_ = 0, turb_ = 0, wander_ = 0;
  float fire_ph_ = 0, pulse_amp_ = 0, pulse_t_ = 1.0f, dc_x_ = 0, dc_y_ = 0;
  int cyl_ = 0;
  float t1_ = 0, t2_ = 0.31f, t3_ = 0.57f, t1r_ = 0.13f;
  float crack_ = 0, hiss_t_ = 1.0f, hiss_len_ = 0.0f, click_ = 0, clunk_ = 0, clunk_at_ = 0, clunk_ph_ = 0;
  bool hiss_rising_ = true;
  uint32_t seed_ = 0x7F4A7C15u;
  Svf thump_lp_, exhaust_bp_, rumble_lp_, rasp_l_, rasp_r_, roar_l_, roar_r_, crack_bp_, hiss_bp_, click_bp_,
      clunk_lp_;
};

}  // namespace gta4::bat
