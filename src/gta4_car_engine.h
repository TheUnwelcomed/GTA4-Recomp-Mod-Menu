// Liberty Trainer: the add-on cars' engines, synthesized.
//
// Original voices made from scratch here: nothing in them comes from films,
// games or any recording. They play through the port's own audio mixer
// (ps5/game/sound.h), each car's game name having no engine sound in IV. One
// synth, a character per car:
//
//  - the Batmobile: in the spirit of the 1989/1992 film car, a jet turbine
//    over a big, lumpy cross-plane V8, a deep roar on the booster;
//  - the Grotti Deimos: a small mid-engined Italian V8 with a flat-plane
//    crank, as the 1970s-80s cars it echoes had (an even, rasping beat that
//    revs high and howls through its carburettors), no turbine.
//
//  - start: the starter cranks, the engine catches with a flare (and a
//    turbine whines up behind it); shut down, it stops (the turbine spins
//    down).
//  - rumble: the firing pulses (8 cylinders: the firing rate is rpm / 15)
//    with the crank's beat and the idle's lope, through an exhaust resonance
//    and a saturation that growls under load.
//  - howl: the carburettors' tone at twice the firing rate (the Deimos).
//  - turbine: two soft tones and a partial whose pitch lags the engine.
//  - intake: band-passed noise opening with throttle and revs, gated by the
//    firing pulses (a rasp).
//  - booster: a deep afterburner roar with sparse crackles.
//  - lift-off: pops from the exhaust when the throttle shuts at high revs.
//  - events, with the engine off too: a canopy's pneumatic hiss opening (a
//    latch click first) and shutting (ending in a clunk); a pop-up lamp's
//    electric motor.
//
// Plain C++ with no platform code: the game thread sets targets and events
// (atomics), the mixer's thread renders. tools/car/render_preview.cpp (in the
// trainer repository) renders each character to a file.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gta4::car {

// What makes one engine sound like itself.
struct Character {
  float idle_rpm, limit_rpm;      // real revs at idle and at the limit
  float beat[8];                  // each firing pulse's strength, in firing order
  float lope;                     // the idle's lumpiness
  float exhaust_hz, exhaust_per;  // the exhaust resonance: hz + per x the firing rate
  float thump;                    // the firing rate's own low thump
  float growl, growl_load;        // saturation drive, and more under load
  float level, level_load, level_revs;
  float bright, bright_revs;      // the rumble's top: hz + revs x (0..1)
  float howl;                     // the carburettors' tone at twice the firing rate
  float sub;                      // the firing rate's own tone: weight in the low end
  float turbine;                  // 0: none
  float turbine_hz, turbine_range;  // the whine's pitch: hz + range x spool
  float intake, intake_load;      // rasp level, and under load
  float intake_hz, intake_revs;   // its band: hz + revs x (0..1)
  float pops;                     // lift-off pops (a rate)
  float flare;                    // the catch's flare, times idle
  float roar_hz, roar_range;      // the booster's roar: its top, hz + range x boost
  float jet, jet_hz, jet_range;   // a turbine's roar under load: level, its top hz + range x revs^2
};
// The Batmobile, after the film car (Batman Returns): a turbine car. The
// turbine's hum at idle, its roar rising under load (a jet's: noise opening
// up with the revs, kept below ~1.6 kHz), its whine under it but never shrill
// (to ~1.1 kHz), a big slow V8's beat and weight for the body, a deep roar on
// the booster. (v1.7.11-v1.7.13: a bright V8 and a turbine whining to 3.3
// kHz; v1.7.14: the V8 alone, dark and low.)
constexpr Character kBatmobile{650.0f,  5200.0f, {1.00f, 0.80f, 1.12f, 0.93f, 0.86f, 1.06f, 0.78f, 1.16f},
                               0.45f,   55.0f,   1.0f,  2.1f, 2.1f, 3.0f, 0.10f, 0.09f, 0.04f, 480.0f, 820.0f,
                               0.0f,    0.07f,   0.45f, 420.0f, 700.0f, 0.004f, 0.022f, 220.0f, 520.0f, 16.0f,
                               1.8f,    90.0f,   650.0f, 0.20f, 260.0f, 1300.0f};
// The Grotti Deimos: a flat-plane Italian V8 (an even beat alternating
// between the banks, a busy idle, high revs, carburettor howl), no turbine.
constexpr Character kDeimos{950.0f,  7700.0f, {1.00f, 0.93f, 1.02f, 0.95f, 1.00f, 0.93f, 1.02f, 0.95f},
                            0.14f,   150.0f,  2.3f,  0.75f, 1.35f, 2.2f, 0.10f, 0.10f, 0.08f, 1500.0f, 3200.0f,
                            0.22f,   0.0f,    0.0f,  750.0f, 2550.0f, 0.009f, 0.08f, 650.0f, 2700.0f, 14.0f,
                            2.2f,    120.0f,  1300.0f, 0.0f, 0.0f, 0.0f};

class EngineSynth {
 public:
  static constexpr float kRate = 48000.0f;
  static constexpr float kIdle = 0.16f;  // normalized revs at idle

  explicit EngineSynth(const Character& character) : c_(character) {}

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
  // A pop-up lamp's motor: a whir for `seconds`, a soft clunk at the end.
  void Motor(float seconds) {
    motor_len_x1000_.store(uint32_t(std::clamp(seconds, 0.1f, 2.0f) * 1000.0f), std::memory_order_relaxed);
    motors_.fetch_add(1, std::memory_order_relaxed);
  }

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
    for (uint32_t n = motors_.exchange(0, std::memory_order_relaxed); n; --n) {
      motor_t_ = 0.0f;
      motor_len_ = float(motor_len_x1000_.load(std::memory_order_relaxed)) / 1000.0f;
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
          want = std::max(target, kIdle * c_.flare);  // it catches: a flare
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
      const float turb_want = running_ && c_.turbine > 0.0f ? std::max(rpm_ * 1.1f, 0.6f * boost_ + 0.25f) : 0.0f;
      turb_ += (turb_want - turb_) * (turb_want > turb_ ? 0.9f : 0.6f) * dt;
      const float r = std::clamp((rpm_ - kIdle) / (1.0f - kIdle), 0.0f, 1.0f);
      const float rpm_real =
          rpm_ >= kIdle ? c_.idle_rpm + (c_.limit_rpm - c_.idle_rpm) * r : c_.idle_rpm * rpm_ / kIdle;

      // ---- V8 firing pulses (the crank's beat, a lope at idle)
      const float fire = rpm_real / 15.0f;
      fire_ph_ += fire * dt;
      if (fire_ph_ >= 1.0f && rpm_ > 0.004f) {
        fire_ph_ -= std::floor(fire_ph_);
        const float lope = c_.lope * (1.0f - r) * (1.0f - r);
        pulse_amp_ = c_.beat[cyl_] * (1.0f + lope * Noise()) * (0.55f + 0.45f * std::min(1.0f, rpm_ / kIdle));
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
      const float res = std::min(0.45f, 3.14159265f * (c_.exhaust_hz + c_.exhaust_per * fire) / kRate);
      const float low = thump_lp_.Lowpass(pulse, 3.14159265f * std::min(fire * 1.4f + 30.0f, 700.0f) / kRate,
                                          1.2f);
      const float body = exhaust_bp_.Bandpass(pulse, res, 0.9f);
      const float drive = c_.growl + c_.growl_load * throttle_ + 1.2f * r;
      float rumble = std::tanh((low * c_.thump + body * 0.9f) * drive) *
                     (c_.level + c_.level_load * throttle_ + c_.level_revs * r);
      // the pulses are one-sided: the DC goes (a 20 Hz high-pass), and the
      // buzz above the exhaust's own range (a low-pass rising with the revs)
      dc_y_ = rumble - dc_x_ + 0.9974f * dc_y_;
      dc_x_ = rumble;
      rumble = rumble_lp_.Lowpass(dc_y_, 3.14159265f * (c_.bright + c_.bright_revs * r) / kRate, 0.9f);
      // weight: the firing rate's own tone, in step with the pulses, firmer
      // under load
      if (c_.sub > 0.0f && rpm_ > 0.002f)
        rumble += Sin(fire_ph_) * c_.sub * (0.55f + 0.45f * throttle_) * std::min(1.0f, rpm_ / kIdle);
      // the carburettors' howl: a tone at twice the firing rate, opening with
      // the revs and the throttle
      if (c_.howl > 0.0f && rpm_ > 0.002f) {
        const float hg = std::min(0.9f, 3.14159265f * (2.0f * fire) / kRate);
        const float h = howl_bp_.Bandpass(pulse, hg, 0.18f);
        rumble += std::tanh(h * 2.5f) * c_.howl * (0.15f + 0.55f * r) * (0.4f + 0.6f * throttle_);
      }
      if (rpm_ <= 0.002f && std::fabs(rumble) < 1e-4f)
        rumble = 0.0f;

      // ---- turbine whine
      const float tf = (c_.turbine_hz + c_.turbine_range * std::pow(turb_, 1.15f)) * (1.0f + 0.004f * wander_);
      if ((i & 255) == 0)
        wander_ += (Noise() - wander_) * 0.15f;
      t1_ = Wrap(t1_ + tf * dt);
      t2_ = Wrap(t2_ + tf * 1.618f * dt);
      t3_ = Wrap(t3_ + tf * 2.35f * dt);
      t1r_ = Wrap(t1r_ + tf * 1.003f * dt);
      const float whine_amp =
          0.022f * c_.turbine * Smooth(0.02f, 0.4f, turb_) * (0.7f + 0.5f * turb_ + 0.4f * boost_);
      const float whine_l = (Sin(t1_) + 0.3f * Sin(t2_) + 0.08f * Sin(t3_)) * whine_amp;
      const float whine_r = (Sin(t1r_) + 0.3f * Sin(t2_) + 0.08f * Sin(t3_)) * whine_amp;

      // ---- intake rasp: noise gated by the firing pulses
      const float rasp_g = 3.14159265f * std::min(c_.intake_hz + c_.intake_revs * r, 4500.0f) / kRate;
      const float rasp_amp = (c_.intake + c_.intake_load * throttle_ * (0.3f + r)) * std::min(1.0f, rpm_ / kIdle);
      const float gate = 0.35f + 0.65f * std::min(1.0f, pulse * 1.5f);
      const float rasp_l = rasp_l_.Bandpass(Noise(), rasp_g, 1.3f) * rasp_amp * gate;
      const float rasp_r = rasp_r_.Bandpass(Noise(), rasp_g * 1.07f, 1.3f) * rasp_amp * gate;

      // ---- a turbine's roar under load (the Batmobile): noise opening up
      // with the revs, louder with the throttle
      float jet_l = 0.0f, jet_r = 0.0f;
      if (c_.jet > 0.0f && rpm_ > 0.002f) {
        const float jg = 3.14159265f * (c_.jet_hz + c_.jet_range * r * r) / kRate;
        const float jamp =
            c_.jet * (0.12f + 0.88f * throttle_) * (0.15f + 0.85f * r) * std::min(1.0f, rpm_ / kIdle);
        jet_l = jet_l_.Lowpass(Noise(), jg, 0.75f) * jamp;
        jet_r = jet_r_.Lowpass(Noise(), jg, 0.75f) * jamp;
      }

      // ---- booster: a deep roar opening up, and sparse crackles
      const float roar_g = 3.14159265f * (c_.roar_hz + c_.roar_range * boost_) / kRate;
      const float roar_amp = 0.33f * boost_;
      const float roar_l = roar_l_.Lowpass(Noise(), roar_g, 0.8f) * roar_amp;
      const float roar_r = roar_r_.Lowpass(Noise(), roar_g, 0.8f) * roar_amp;
      float crack = 0.0f;
      if (boost_ > 0.05f && Uniform() < boost_ * 30.0f * dt)
        crack_ = 0.5f + 0.5f * Uniform();
      // lift-off: the throttle shut at high revs pops the exhaust
      if (running_ && throttle_ < 0.15f && r > 0.35f && Uniform() < (r - 0.3f) * c_.pops * dt)
        crack_ = std::max(crack_, 0.35f + 0.45f * Uniform());
      if (crack_ > 1e-4f) {
        crack = crack_bp_.Bandpass(Noise() * crack_, 0.06f, 1.2f) * 0.6f;
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
      // ---- a pop-up lamp's motor: a small electric motor's whir rising a
      // little as it runs, the gears' whine over it, a soft clunk at the end
      if (motor_t_ < motor_len_) {
        const float u = motor_t_ / motor_len_;
        const float env = std::min(1.0f, motor_t_ * 60.0f) * std::min(1.0f, (motor_len_ - motor_t_) * 30.0f);
        motor_ph_ = Wrap(motor_ph_ + (150.0f + 35.0f * u) * dt);
        gear_ph_ = Wrap(gear_ph_ + (1150.0f + 260.0f * u) * dt);
        const float buzz = Sin(motor_ph_) + 0.5f * Sin(3.0f * motor_ph_) + 0.3f * Sin(5.0f * motor_ph_);
        canopy += (motor_bp_.Bandpass(buzz, 0.06f, 0.9f) * 0.10f + Sin(gear_ph_) * 0.012f) * env;
        motor_t_ += dt;
        if (motor_t_ >= motor_len_)
          clunk_ = std::max(clunk_, 0.35f);
      }
      if (clunk_ > 1e-4f) {
        clunk_ph_ = Wrap(clunk_ph_ + 68.0f * dt);
        canopy += (Sin(clunk_ph_) * 0.30f + clunk_lp_.Lowpass(Noise(), 0.04f, 1.0f) * 0.5f) * clunk_;
        clunk_ *= 0.9993f;  // (~30 ms)
      } else {
        clunk_ph_ = 0.0f;
      }

      const float shared = rumble + crack;
      float l = (shared + whine_l + rasp_l + roar_l + jet_l) * vol_ + canopy;
      float rr = (shared * 0.97f + whine_r + rasp_r + roar_r + jet_r) * vol_ + canopy;
      // a soft ceiling at 0.6
      l = 0.6f * std::tanh(l * (1.0f / 0.6f));
      rr = 0.6f * std::tanh(rr * (1.0f / 0.6f));
      lr[2 * i] = l;
      lr[2 * i + 1] = rr;
    }
    rpm_out_.store(rpm_, std::memory_order_relaxed);
    quiet_.store(!running_ && rpm_ < 0.002f && turb_ < 0.01f && boost_ < 0.01f && crack_ < 1e-3f &&
                     hiss_t_ >= hiss_len_ && click_ < 1e-3f && clunk_ < 1e-3f && motor_t_ >= motor_len_,
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
  const Character c_;
  std::atomic<uint32_t> opens_{0}, shuts_{0}, motors_{0}, motor_len_x1000_{400};
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
  float motor_t_ = 1.0f, motor_len_ = 0.0f, motor_ph_ = 0, gear_ph_ = 0;
  uint32_t seed_ = 0x7F4A7C15u;
  Svf thump_lp_, exhaust_bp_, rumble_lp_, howl_bp_, rasp_l_, rasp_r_, roar_l_, roar_r_, jet_l_, jet_r_, crack_bp_,
      hiss_bp_, click_bp_, clunk_lp_, motor_bp_;
};

}  // namespace gta4::car
