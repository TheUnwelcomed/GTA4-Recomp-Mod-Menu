// LibertyRecomp Menu: the jets' engines (the F-16's, the Batwing's), synthesized.
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
//  - the lock-on tone: two short beeps;
//  - the landing gear's hydraulics (the pump's whine for as long as it
//    moves, the lock's clunk) and the Batwing's canopy (its actuator, the
//    latch, the seal's hiss).
// Two voicings: the F-16's single turbofan, and the Batwing's twin turbines
// (the two beating against each other, lower at idle, a high intake
// whistle over them, more rumble and less crackle).
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

// An engine's character.
struct Voicing {
  float whine_base, whine_span, whine_exp;  // the blade tone: base + span x spool^exp, Hz
  float fan_ratio, whine_level;
  float twin;                               // a second engine, detuned by this (0: one engine)
  float whistle, whistle_ratio;             // an intake whistle's level and pitch over the whine
  float roar_level, roar_cut, roar_span;    // jet noise: level, low-pass from cut to cut + span x spool^2
  float rumble_level;
  float crackle_rate;                       // the afterburner's pops a second, at full burner
};
inline constexpr Voicing kF16Voice{650.0f, 2450.0f, 1.25f, 0.37f, 0.060f, 0.0f, 0.0f, 0.0f,
                                   0.30f,  160.0f,  2300.0f, 0.25f, 55.0f};
inline constexpr Voicing kBatwingVoice{520.0f, 2900.0f, 1.35f, 0.26f, 0.070f, 0.006f, 0.035f, 2.35f,
                                       0.27f,  140.0f,  2100.0f, 0.32f, 30.0f};

class EngineSynth {
 public:
  static constexpr float kRate = 48000.0f;
  static constexpr float kIdle = 0.32f;

  // 0 the F-16's voicing, 1 the Batwing's.
  void SetVoice(int voice) { voice_.store(voice, std::memory_order_relaxed); }
  // The landing gear moving for `seconds` (down or up), its lock at the end.
  void Gear(float seconds, bool down) {
    gear_len_.store(seconds, std::memory_order_relaxed);
    gear_down_.store(down, std::memory_order_relaxed);
    gears_.fetch_add(1, std::memory_order_relaxed);
  }
  // A canopy moving for `seconds`, opening (the latch, then the actuator) or
  // closing (the actuator, then the latch and the seal).
  void Canopy(float seconds, bool opening) {
    canopy_len_.store(seconds, std::memory_order_relaxed);
    canopy_open_.store(opening, std::memory_order_relaxed);
    canopies_.fetch_add(1, std::memory_order_relaxed);
  }

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
    const Voicing& V = voice_.load(std::memory_order_relaxed) == 1 ? kBatwingVoice : kF16Voice;
    if (gears_.exchange(0, std::memory_order_relaxed)) {
      gear_t_ = 0.0f;
      gear_T_ = gear_len_.load(std::memory_order_relaxed);
      gear_dir_down_ = gear_down_.load(std::memory_order_relaxed);
      gear_locked_ = false;
    }
    if (canopies_.exchange(0, std::memory_order_relaxed)) {
      canopy_t_ = 0.0f;
      canopy_T_ = canopy_len_.load(std::memory_order_relaxed);
      canopy_opening_ = canopy_open_.load(std::memory_order_relaxed);
      canopy_end_done_ = false;
      if (canopy_opening_) {
        latch_ = 0.8f;   // the latch lets go
        seal_ = 1.0f;    // the seal breaks
      }
    }

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
      const float f1 = (V.whine_base + V.whine_span * std::pow(rpm, V.whine_exp)) * (1.0f + 0.004f * wander_);
      const float fan = V.fan_ratio * f1;
      ph1_ = Wrap(ph1_ + f1 * dt);
      ph1b_ = Wrap(ph1b_ + f1 * 1.0045f * dt);
      ph2_ = Wrap(ph2_ + 2.0f * f1 * dt);
      ph3_ = Wrap(ph3_ + 3.02f * f1 * dt);
      phf_ = Wrap(phf_ + fan * dt);
      float tones = Sin(ph1_) + 0.55f * Sin(ph1b_) + 0.32f * Sin(ph2_) + 0.10f * Sin(ph3_) + 0.45f * Sin(phf_);
      if (V.twin > 0.0f) {  // the other engine, a little apart: the two beat
        const float f2 = f1 * (1.0f + V.twin);
        pht1_ = Wrap(pht1_ + f2 * dt);
        pht2_ = Wrap(pht2_ + 2.0f * f2 * dt);
        phtf_ = Wrap(phtf_ + V.fan_ratio * f2 * dt);
        tones = 0.62f * tones + 0.62f * (Sin(pht1_) + 0.30f * Sin(pht2_) + 0.40f * Sin(phtf_));
      }
      const float whine_amp = V.whine_level * Smooth(0.02f, 0.5f, rpm) * (0.6f + 0.4f * rpm) * (1.0f - 0.45f * ab_);
      float whine = tones * whine_amp;
      if (V.whistle > 0.0f) {  // the intake's whistle: a narrow band riding over the whine
        const float fw = std::min(V.whistle_ratio * f1 * (1.0f + 0.003f * Sin(phv_)), 15000.0f);
        phv_ = Wrap(phv_ + 5.3f * dt);
        phw_ = Wrap(phw_ + fw * dt);
        const float band = whistle_bp_.Bandpass(Noise(), std::tan(3.14159265f * fw * dt), 0.06f);
        whine += (0.55f * Sin(phw_) + 2.2f * band) * V.whistle * Smooth(0.1f, 0.7f, rpm);
      }

      // ---- roar: low-passed noise opening up with power (stereo: mostly shared)
      const float common = Noise(), nl = Noise(), nr = Noise();
      const float cut = V.roar_cut + V.roar_span * p2 + 900.0f * ab_;
      const float g = std::tan(3.14159265f * std::min(cut, 12000.0f) * dt);
      const float roar_in_l = 0.78f * common + 0.22f * nl, roar_in_r = 0.78f * common + 0.22f * nr;
      const float roar_l = roar_l_.Lowpass(roar_in_l, g, 0.75f);
      const float roar_r = roar_r_.Lowpass(roar_in_r, g, 0.75f);
      const float roar_amp = (0.035f + V.roar_level * p2 + 0.22f * ab_) * Smooth(0.0f, 0.25f, rpm + ab_);

      // ---- rumble: brown noise, low-passed, with the afterburner's pulsing
      brown_ = brown_ * 0.996f + common * 0.06f;
      rumble_ += (brown_ - rumble_) * (2.0f * 3.14159265f * 110.0f * dt);
      if ((i & 255) == 0) pulse_target_ = 0.75f + 0.25f * Noise();
      pulse_ += (pulse_target_ - pulse_) * 0.002f;
      const float rumble = rumble_ * (V.rumble_level * p2 + 0.9f * ab_ * pulse_);

      // ---- crackle: sparse pops while the afterburner burns
      if (ab_ > 0.05f && Uniform() < ab_ * V.crackle_rate * dt) crack_ = 0.6f + 0.4f * Uniform();
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
      // ---- the landing gear: the hydraulic pump's whine while it moves,
      // the lock's clunk at the end (heavier going down, onto its stops)
      float mech = 0.0f;
      if (gear_t_ < gear_T_ + 0.8f) {
        const float t = gear_t_, T = gear_T_;
        const float env = Smooth(0.0f, 0.08f, t) * (1.0f - Smooth(T - 0.2f, T, t));
        if (env > 0.0f) {
          const float fp = 410.0f + 70.0f * std::sin(3.14159265f * std::min(t / T, 1.0f)) - 25.0f * (gear_dir_down_ ? 1.0f : 0.0f);
          pump_ph_ = Wrap(pump_ph_ + fp * dt);
          const float pump = Sin(pump_ph_) + 0.45f * Sin(2.0f * pump_ph_) + 0.2f * Sin(3.0f * pump_ph_);
          const float flow = flow_hp_.Bandpass(Noise(), 0.35f, 0.9f);
          mech += (0.030f * pump + 0.040f * flow) * env;
        }
        if (t >= T && !gear_locked_) {
          gear_locked_ = true;
          clunk_ = gear_dir_down_ ? 1.0f : 0.7f;
        }
        gear_t_ += dt;
      }
      // ---- the canopy: the latch, the actuator's whine and grind, the seal
      if (canopy_t_ < canopy_T_ + 0.8f) {
        const float t = canopy_t_, T = canopy_T_;
        const float env = Smooth(0.03f, 0.12f, t) * (1.0f - Smooth(T - 0.12f, T, t));
        if (env > 0.0f) {
          const float k = std::min(t / T, 1.0f);
          const float fm = canopy_opening_ ? 175.0f + 70.0f * k : 245.0f - 70.0f * k;
          motor_ph_ = Wrap(motor_ph_ + fm * dt);
          const float saw = 2.0f * motor_ph_ - 1.0f;
          const float motor = motor_lp_.Lowpass(saw, 0.08f, 0.8f);
          const float grind = gear_bp_.Bandpass(Noise(), 0.12f, 0.5f) * (0.6f + 0.4f * Sin(motor_ph_));
          mech += (0.10f * motor + 0.035f * grind) * env;
        }
        if (t >= T && !canopy_end_done_) {
          canopy_end_done_ = true;
          if (!canopy_opening_) {
            clunk_ = 0.85f;  // the latch
            seal_ = 0.6f;    // the seal pressing home
          } else {
            clunk_ = 0.35f;  // up against its stop
          }
        }
        canopy_t_ += dt;
      }
      if (latch_ > 1e-4f) {
        mech += latch_bp_.Bandpass(Noise(), 0.30f, 0.6f) * latch_ * 0.35f;
        latch_ *= 0.9965f;  // (~6 ms)
      }
      if (seal_ > 1e-4f) {
        const float n = Noise();
        seal_lp_ += (n - seal_lp_) * 0.3f;
        mech += (n - seal_lp_) * seal_ * 0.06f;
        seal_ *= 0.99985f;  // (~150 ms)
      }
      if (clunk_ > 1e-4f) {
        clunk_ph_ = Wrap(clunk_ph_ + 72.0f * dt);
        mech += (clunk_lp_.Lowpass(Noise(), 0.05f, 0.9f) * 0.5f + Sin(clunk_ph_) * 0.35f) * clunk_;
        clunk_ *= 0.9988f;  // (~17 ms)
      }
      const float engine = whine + rumble + crack;
      float l = ((engine + roar_l * roar_amp + hiss) * vol_ + gun + whoosh + mech) * 1.6f;
      float r = ((engine + roar_r * roar_amp + hiss) * vol_ + gun + whoosh + mech) * 1.6f;
      l = std::tanh(l * 1.2f) * 0.85f + beep;
      r = std::tanh(r * 1.2f) * 0.85f + beep;
      lr[2 * i] = l;
      lr[2 * i + 1] = r;
    }
    rpm_out_.store(rpm_, std::memory_order_relaxed);
    quiet_.store(target <= 0.0f && rpm_ < 0.01f && ab_ < 0.01f && launch_ < 1e-3f && shot_ < 1e-3f &&
                     beep_t_ >= 0.2f && gear_t_ >= gear_T_ + 0.8f && canopy_t_ >= canopy_T_ + 0.8f &&
                     clunk_ < 1e-3f && seal_ < 1e-3f && gears_.load(std::memory_order_relaxed) == 0 &&
                     canopies_.load(std::memory_order_relaxed) == 0,
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
  std::atomic<uint32_t> shots_{0}, launches_{0}, beeps_{0}, gears_{0}, canopies_{0};
  std::atomic<int> voice_{0};
  std::atomic<float> gear_len_{2.6f}, canopy_len_{1.2f};
  std::atomic<bool> gear_down_{true}, canopy_open_{true};
  std::atomic<bool> quiet_{true};
  std::atomic<float> rpm_out_{0};
  // (the mixer thread's own)
  float rpm_ = 0, ab_ = 0, vol_ = 0, wander_ = 0;
  float ph1_ = 0, ph1b_ = 0.37f, ph2_ = 0, ph3_ = 0, phf_ = 0, thump_ph_ = 0;
  float brown_ = 0, rumble_ = 0, pulse_ = 1, pulse_target_ = 1, crack_ = 0, hiss_lp_ = 0;
  float shot_ = 0, thump_ = 0, launch_ = 0, beep_t_ = 1.0f, beep_ph_ = 0;
  uint32_t seed_ = 0x9E3779B9u;
  Svf roar_l_, roar_r_, crack_l_, gun_lp_, launch_lp_;
  // the twin engines and the whistle
  float pht1_ = 0.21f, pht2_ = 0, phtf_ = 0, phv_ = 0, phw_ = 0;
  Svf whistle_bp_;
  // the gear and the canopy
  float gear_t_ = 1e3f, gear_T_ = 0, canopy_t_ = 1e3f, canopy_T_ = 0;
  bool gear_dir_down_ = true, gear_locked_ = true, canopy_opening_ = true, canopy_end_done_ = true;
  float pump_ph_ = 0, motor_ph_ = 0, clunk_ph_ = 0, clunk_ = 0, latch_ = 0, seal_ = 0, seal_lp_ = 0;
  Svf flow_hp_, motor_lp_, gear_bp_, latch_bp_, clunk_lp_;
};

}  // namespace gta4::jet
