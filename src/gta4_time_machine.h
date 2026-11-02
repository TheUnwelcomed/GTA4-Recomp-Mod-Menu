// Liberty Trainer: the DeLorean time machine's sounds, synthesized.
//
// Original sounds made from scratch here in the spirit of the three films'
// car (nothing in them comes from the films or any recording): the time
// circuits' keypad and their power-up chirps, the flux capacitor fluxing,
// the electrical build-up towards 88 mph (a rising whine, arcs crackling),
// the departure (a surge, the implosion's boom and the air rushing in, the
// fire trails roaring), the arrival's triple boom, the cold vents' hiss with
// the ice cracking, the plutonium chamber's reload (Part I), Mr. Fusion
// blending its rubbish (Parts II and III), the wheels folding for flight
// (Part II) and the hover thrusters (an '80s synth's take, v1.7.30), the
// empty-reactor warning.
//
// Plain C++ like gta4_car_engine.h: the game thread sets levels and events
// (atomics), the mixer's thread renders. tools/car/render_time_machine.cpp
// (in the trainer repository) renders them to a file.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gta4::car {

class TimeMachineSynth {
 public:
  static constexpr float kRate = 48000.0f;

  // Events.
  void CircuitsOn() { on_seq_.fetch_add(1, std::memory_order_relaxed); }
  void CircuitsOff() { off_seq_.fetch_add(1, std::memory_order_relaxed); }
  void Key(int digit) {
    key_digit_.store(uint32_t(std::clamp(digit, 0, 11)), std::memory_order_relaxed);
    key_seq_.fetch_add(1, std::memory_order_relaxed);
  }
  void Enter() { enter_seq_.fetch_add(1, std::memory_order_relaxed); }
  void Error() { error_seq_.fetch_add(1, std::memory_order_relaxed); }
  // film: 1-3; flying: no fire trails (Part II in the air)
  void Depart(bool flying) {
    depart_fly_.store(flying, std::memory_order_relaxed);
    depart_seq_.fetch_add(1, std::memory_order_relaxed);
  }
  void Arrive() { arrive_seq_.fetch_add(1, std::memory_order_relaxed); }
  void Vents() { vent_seq_.fetch_add(1, std::memory_order_relaxed); }
  void Plutonium() { pluto_seq_.fetch_add(1, std::memory_order_relaxed); }
  void Fusion() { fusion_seq_.fetch_add(1, std::memory_order_relaxed); }
  void Convert(bool to_flight) {
    convert_up_.store(to_flight, std::memory_order_relaxed);
    convert_seq_.fetch_add(1, std::memory_order_relaxed);
  }
  void Empty() { empty_seq_.fetch_add(1, std::memory_order_relaxed); }
  // Held levels, 0..1: the flux capacitor (0: off), the build-up towards
  // 88 mph, the hover thrusters (0: wheels down).
  void SetFlux(float v) { flux_t_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed); }
  void SetCharge(float v) { charge_t_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed); }
  void SetHover(float v) { hover_t_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed); }

  bool Quiet() const { return quiet_.load(std::memory_order_relaxed); }

  void Render(float* lr, size_t frames) {
    Take(on_seq_, on_seen_, [&] { chirp_t_ = 0.0f, chirp_up_ = true, relay_ = 1.0f; });
    Take(off_seq_, off_seen_, [&] { chirp_t_ = 0.0f, chirp_up_ = false, relay_ = 1.0f; });
    Take(key_seq_, key_seen_, [&] {
      key_t_ = 0.0f;
      key_hz_ = 980.0f + 115.0f * float(key_digit_.load(std::memory_order_relaxed));
    });
    Take(enter_seq_, enter_seen_, [&] { enter_t_ = 0.0f; });
    Take(error_seq_, error_seen_, [&] { error_t_ = 0.0f; });
    Take(depart_seq_, depart_seen_, [&] {
      depart_t_ = 0.0f, depart_flying_ = depart_fly_.load(std::memory_order_relaxed);
    });
    Take(arrive_seq_, arrive_seen_, [&] { arrive_t_ = 0.0f; });
    Take(vent_seq_, vent_seen_, [&] { vent_t_ = 0.0f; });
    Take(pluto_seq_, pluto_seen_, [&] { pluto_t_ = 0.0f; });
    Take(fusion_seq_, fusion_seen_, [&] { fusion_t_ = 0.0f; });
    Take(convert_seq_, convert_seen_, [&] {
      convert_t_ = 0.0f, convert_to_flight_ = convert_up_.load(std::memory_order_relaxed);
    });
    Take(empty_seq_, empty_seen_, [&] { empty_t_ = 0.0f; });
    const float flux_target = flux_t_.load(std::memory_order_relaxed);
    const float charge_target = charge_t_.load(std::memory_order_relaxed);
    const float hover_target = hover_t_.load(std::memory_order_relaxed);
    constexpr float dt = 1.0f / kRate;
    bool busy = false;
    for (size_t i = 0; i < frames; ++i) {
      flux_ += (flux_target - flux_) * 6.0f * dt;
      charge_ += (charge_target - charge_) * 8.0f * dt;
      hover_ += (hover_target - hover_) * (hover_target > hover_ ? 1.5f : 2.5f) * dt;
      float s = 0.0f, side = 0.0f;  // centre; a little spread for the big ones

      // ---- the time circuits powering up (rising chirps) or down, a relay
      if (chirp_t_ < 0.5f) {
        const float t = chirp_t_;
        const int step = std::min(2, int(t / 0.12f));
        static constexpr float kUp[3] = {880.0f, 1175.0f, 1568.0f};
        const float hz = chirp_up_ ? kUp[step] : kUp[2 - step];
        const float u = t - 0.12f * float(step);
        if (t < 0.36f) {
          chirp_ph_ = Wrap(chirp_ph_ + hz * dt);
          const float tone = Sin(chirp_ph_) + 0.3f * Sin(3.0f * chirp_ph_);
          s += tone * 0.05f * std::min(1.0f, u * 400.0f) * std::exp(-u / 0.07f);
        }
        chirp_t_ += dt;
      }
      if (relay_ > 1e-4f) {
        s += relay_bp_.Bandpass(Noise() * relay_, G(3000.0f), 1.4f) * 0.35f;
        relay_ *= 0.997f;
      }
      // ---- keypad: one tone a key (each key its own pitch), a click
      if (key_t_ < 0.09f) {
        const float t = key_t_;
        key_ph_ = Wrap(key_ph_ + key_hz_ * dt);
        s += (Sin(key_ph_) + 0.18f * Sin(3.0f * key_ph_)) * 0.06f * std::min(1.0f, t * 800.0f) *
             std::min(1.0f, (0.09f - t) * 200.0f);
        if (t < 0.003f)
          s += click_bp_.Bandpass(Noise(), G(4000.0f), 1.5f) * 0.2f;
        key_t_ += dt;
      }
      if (enter_t_ < 0.24f) {
        const float t = enter_t_;
        const float hz = t < 0.11f ? 1480.0f : 1975.0f;
        const float u = t < 0.11f ? t : t - 0.12f;
        enter_ph_ = Wrap(enter_ph_ + hz * dt);
        if (u >= 0.0f && u < 0.1f)
          s += Sin(enter_ph_) * 0.06f * std::min(1.0f, u * 600.0f) * std::min(1.0f, (0.1f - u) * 200.0f);
        enter_t_ += dt;
      }
      if (error_t_ < 0.3f) {
        error_ph_ = Wrap(error_ph_ + 210.0f * dt);
        s += Square(error_ph_) * 0.035f * std::min(1.0f, (0.3f - error_t_) * 40.0f);
        error_t_ += dt;
      }
      if (empty_t_ < 0.5f) {
        const float t = empty_t_;
        const bool beeping = t < 0.16f || (t > 0.26f && t < 0.42f);
        empty_ph_ = Wrap(empty_ph_ + 440.0f * dt);
        if (beeping)
          s += Square(empty_ph_) * 0.03f;
        empty_t_ += dt;
      }
      // ---- the flux capacitor fluxing: a transformer's hum pulsing in a
      // rhythm that quickens with the build-up, a zap of current each pulse
      if (flux_ > 1e-3f) {
        const float rate = 3.0f + 9.0f * flux_ + 10.0f * charge_;
        pulse_ph_ += rate * dt;
        if (pulse_ph_ >= 1.0f) {
          pulse_ph_ -= 1.0f;
          zap_ = 1.0f;
        }
        const float pulse = 0.35f + 0.65f * std::exp(-pulse_ph_ * 5.0f);
        hum_ph_ = Wrap(hum_ph_ + 100.0f * dt);
        const float hum = Sin(hum_ph_) + 0.5f * Sin(2.0f * hum_ph_) + 0.25f * Sin(3.0f * hum_ph_);
        s += hum * 0.04f * flux_ * pulse;
        if (zap_ > 1e-3f) {
          s += zap_bp_.Bandpass(Noise(), G(2600.0f + 1200.0f * flux_), 1.0f) * 0.22f * zap_ * flux_;
          zap_ *= 0.9975f;  // (~8 ms)
        }
      }
      // ---- the build-up: a whine climbing with the speed, arcs crackling
      // and buzzing around the car more and more
      if (charge_ > 1e-3f) {
        busy = true;
        whine_ph_ = Wrap(whine_ph_ + (420.0f + 2600.0f * charge_ * charge_) * (1.0f + 0.01f * Sin(vib_ph_)) * dt);
        vib_ph_ = Wrap(vib_ph_ + 7.0f * dt);
        s += (Sin(whine_ph_) + 0.25f * Sin(2.0f * whine_ph_)) * 0.05f * charge_;
        if (Uniform() < (4.0f + 45.0f * charge_) * dt)
          arc_ = 0.4f + 0.6f * Uniform(), arc_hz_ = 2500.0f + 3500.0f * Uniform();
        if (arc_ > 1e-3f) {
          const float a = arc_bp_.Bandpass(Noise(), G(arc_hz_), 0.8f) * arc_ * 1.0f * charge_;
          s += a;
          side += a * 0.5f * Noise();
          arc_ *= 0.996f;
        }
        buzz_ph_ = Wrap(buzz_ph_ + 120.0f * dt);
        s += buzz_bp_.Bandpass(Noise(), G(1800.0f), 1.2f) * 0.15f * charge_ * charge_ * (Sin(buzz_ph_) > 0.0f ? 1.0f : 0.3f);
      }
      // ---- departure: the surge, then the implosion's boom, the air
      // rushing into the space left, the fire trails roaring after it
      if (depart_t_ < 3.2f) {
        busy = true;
        const float t = depart_t_;
        if (t < 0.25f) {
          const float u = t / 0.25f;
          surge_ph_ = Wrap(surge_ph_ + (300.0f + 2800.0f * u * u) * dt);
          s += Sin(surge_ph_) * 0.05f * u + surge_bp_.Bandpass(Noise(), G(1200.0f + 3000.0f * u), 0.9f) * 0.25f * u;
        } else {
          const float u = t - 0.25f;
          boom_ph_ = Wrap(boom_ph_ + (28.0f + 40.0f * std::exp(-u / 0.06f)) * dt);
          s += Sin(boom_ph_) * 0.5f * std::exp(-u / 0.35f) * std::min(1.0f, u * 3000.0f);
          s += crack_bp_.Bandpass(Noise(), G(2400.0f), 0.6f) * 1.6f * std::exp(-u / 0.008f);
          const float rumble = rumble_lp_.Lowpass(Noise(), G(120.0f), 0.8f) * 3.0f * std::exp(-u / 0.8f);
          s += rumble;
          side += rumble * 0.3f * Noise();
          if (u < 1.0f)
            s += whoosh_bp_.Bandpass(Noise(), G(2000.0f * std::exp(-u * 2.0f) + 300.0f), 0.7f) * 0.35f *
                 std::exp(-u / 0.35f);
          if (!depart_flying_ && u > 0.1f) {
            const float env = std::min(1.0f, (u - 0.1f) * 4.0f) * std::exp(-(u - 0.1f) / 1.4f);
            s += fire_lp_.Lowpass(Noise(), G(550.0f), 0.9f) * 1.4f * env;
            if (Uniform() < 25.0f * env * dt)
              pop_ = 0.3f + 0.7f * Uniform();
          }
        }
        depart_t_ += dt;
      }
      if (pop_ > 1e-3f) {
        s += pop_bp_.Bandpass(Noise(), G(900.0f), 1.0f) * pop_ * 0.25f;
        pop_ *= 0.995f;
      }
      // ---- arrival: three booms in quick succession, the whoosh after
      if (arrive_t_ < 1.6f) {
        busy = true;
        const float t = arrive_t_;
        for (int b = 0; b < 3; ++b) {
          const float u = t - 0.17f * float(b);
          if (u < 0.0f || u > 0.6f)
            continue;
          arr_ph_[b] = Wrap(arr_ph_[b] + (34.0f + 50.0f * std::exp(-u / 0.04f)) * dt);
          s += Sin(arr_ph_[b]) * 0.42f * std::exp(-u / 0.16f) * std::min(1.0f, u * 3000.0f);
          s += arr_crack_[b].Bandpass(Noise(), G(2800.0f), 0.6f) * 1.4f * std::exp(-u / 0.006f);
          s += arr_lp_[b].Lowpass(Noise(), G(160.0f), 0.8f) * 2.2f * std::exp(-u / 0.25f);
        }
        if (t > 0.4f)
          s += whoosh_bp_.Bandpass(Noise(), G(300.0f + 1500.0f * std::exp(-(t - 0.4f) * 3.0f)), 0.7f) * 0.25f *
               std::exp(-(t - 0.4f) / 0.4f);
        arrive_t_ += dt;
      }
      // ---- the cold vents: a strong hiss falling away, the ice cracking
      if (vent_t_ < 7.0f) {
        busy = true;
        const float t = vent_t_;
        const float env = std::min(1.0f, t * 20.0f) * std::exp(-t / 2.2f);
        const float h = vent_bp_.Bandpass(Noise(), G(3800.0f), 0.7f) * 0.5f + vent_lp_.Lowpass(Noise(), G(9000.0f), 0.8f) * 0.1f;
        s += h * env;
        side += h * env * 0.4f * Noise();
        if (t < 4.0f && Uniform() < 9.0f * dt)
          ice_ = 0.3f + 0.7f * Uniform(), ice_hz_ = 3000.0f + 4000.0f * Uniform();
        vent_t_ += dt;
      }
      if (ice_ > 1e-3f) {
        s += ice_bp_.Bandpass(Noise(), G(ice_hz_), 1.6f) * ice_ * 0.3f;
        ice_ *= 0.993f;
      }
      // ---- Part I's reload: the chamber's latch, the canister sliding in
      // and seating, the lid, the seal's hiss, the reactor humming up
      if (pluto_t_ < 2.6f) {
        busy = true;
        const float t = pluto_t_;
        s += Clank(t, 0.0f, 1800.0f, 0.4f, 0) + Clank(t, 0.02f, 650.0f, 0.3f, 1);
        if (t > 0.35f && t < 0.9f) {
          const float u = (t - 0.35f) / 0.55f;
          s += slide_bp_.Bandpass(Noise(), G(900.0f - 400.0f * u), 1.0f) * 0.12f * Hump(u);
        }
        if (t > 0.92f && t < 1.2f) {
          const float u = t - 0.92f;
          seat_ph_ = Wrap(seat_ph_ + 85.0f * dt);
          s += Sin(seat_ph_) * 0.25f * std::exp(-u / 0.06f) + seat_lp_.Lowpass(Noise(), G(300.0f), 1.0f) * 1.2f * std::exp(-u / 0.04f);
        }
        s += Clank(t, 1.3f, 1500.0f, 0.35f, 2);
        if (t > 1.4f) {
          const float u = t - 1.4f;
          s += seal_bp_.Bandpass(Noise(), G(5000.0f), 0.8f) * 0.25f * std::exp(-u / 0.35f);
          react_ph_ = Wrap(react_ph_ + (60.0f + 60.0f * std::min(1.0f, u)) * dt);
          s += (Sin(react_ph_) + 0.5f * Sin(2.0f * react_ph_)) * 0.03f * std::min(1.0f, u * 3.0f) *
               std::min(1.0f, (1.2f - u) * 4.0f);
        }
        pluto_t_ += dt;
      }
      // ---- Mr. Fusion: the lid pops, the rubbish clatters in, the blender
      // whirs it up, the lid snaps shut, the power comes up
      if (fusion_t_ < 3.3f) {
        busy = true;
        const float t = fusion_t_;
        s += Clank(t, 0.0f, 1200.0f, 0.3f, 0);
        if (t > 0.3f && t < 0.9f && Uniform() < 30.0f * dt)
          tink_ = 0.4f + 0.6f * Uniform(), tink_hz_ = 2200.0f + 3000.0f * Uniform();
        if (t > 1.0f && t < 2.6f) {
          const float u = (t - 1.0f) / 1.6f;
          motor_ph_ = Wrap(motor_ph_ + (160.0f + 60.0f * std::min(1.0f, u * 4.0f)) * dt);
          const float env = std::min(1.0f, u * 12.0f) * std::min(1.0f, (1.0f - u) * 12.0f);
          grind_ += (Noise() - grind_) * 0.02f;
          s += (Sin(motor_ph_) + 0.5f * Sin(2.0f * motor_ph_) + 0.3f * Sin(3.0f * motor_ph_)) * 0.025f * env;
          s += grind_bp_.Bandpass(Noise(), G(900.0f), 0.9f) * 0.25f * env * (0.6f + 0.4f * std::fabs(grind_) * 8.0f);
        }
        s += Clank(t, 2.7f, 2000.0f, 0.3f, 1);
        if (t > 2.75f) {
          const float u = (t - 2.75f) / 0.5f;
          up_ph_ = Wrap(up_ph_ + (200.0f + 600.0f * u) * dt);
          s += Sin(up_ph_) * 0.04f * Hump(std::min(u, 1.0f));
        }
        fusion_t_ += dt;
      }
      if (tink_ > 1e-3f) {
        s += tink_bp_.Bandpass(Noise(), G(tink_hz_), 2.5f) * tink_ * 0.25f;
        tink_ *= 0.996f;
      }
      // ---- Part II's wheels folding (or down again): the motors, a clunk
      // as each pair locks, the thrusters spooling up into flight
      if (convert_t_ < 1.6f) {
        busy = true;
        const float t = convert_t_;
        if (t < 1.0f) {
          servo_ph_ = Wrap(servo_ph_ + (300.0f + 420.0f * t) * dt);
          s += (Sin(servo_ph_) + 0.4f * Sin(2.0f * servo_ph_)) * 0.035f * std::min(1.0f, t * 20.0f) *
               std::min(1.0f, (1.0f - t) * 20.0f);
        }
        s += Clank(t, 0.5f, 900.0f, 0.4f, 2) + Clank(t, 0.95f, 800.0f, 0.45f, 3);
        if (convert_to_flight_ && t > 0.4f) {
          const float u = std::min(1.0f, (t - 0.4f) / 1.2f);
          s += spool_lp_.Lowpass(Noise(), G(300.0f + 2200.0f * u), 0.8f) * 0.6f * Hump(u);
        }
        convert_t_ += dt;
      }
      // ---- hover thrusters (v1.7.30): an '80s synth's take on them, and
      // quiet (v1.7.29's was a roar of noise, 10 dB over everything else):
      // two detuned saw voices an octave over a sine sub, each through a
      // resonant low-pass that opens with the thrust and sweeps slowly (a
      // chorus across the two sides), throbbing gently; a turbine whine with
      // vibrato on top; a breath of air through a swept band-pass.
      if (hover_ > 1e-3f) {
        const float h = hover_;
        const float f0 = 98.0f * (1.0f + 0.35f * h);
        lfo_ph_ = Wrap(lfo_ph_ + 0.23f * dt);
        const float lfo = Sin(lfo_ph_);
        saw1_ph_ = Wrap(saw1_ph_ + f0 * (1.0f + 0.002f * lfo) * dt);
        saw2_ph_ = Wrap(saw2_ph_ + f0 * 1.0059f * dt);
        sub_ph_ = Wrap(sub_ph_ + f0 * 0.5f * dt);
        const float sub = 0.3f * Sin(sub_ph_);
        const float cut_l = 520.0f + 1100.0f * h + 300.0f * lfo, cut_r = 520.0f + 1100.0f * h - 300.0f * lfo;
        const float vl = pad_l_.Lowpass(Saw(saw1_ph_, f0 * dt) + sub, G(cut_l), 0.38f);
        const float vr = pad_r_.Lowpass(Saw(saw2_ph_, f0 * 1.0059f * dt) + sub, G(cut_r), 0.38f);
        throb_ph_ = Wrap(throb_ph_ + (3.5f + 3.0f * h) * dt);
        const float level = (0.033f + 0.025f * h) * (0.84f + 0.16f * Sin(throb_ph_));
        vib2_ph_ = Wrap(vib2_ph_ + 5.2f * dt);
        thr_ph_ = Wrap(thr_ph_ + (1150.0f + 650.0f * h) * (1.0f + 0.006f * Sin(vib2_ph_)) * dt);
        const float whine = Sin(thr_ph_) * (0.0025f + 0.004f * h);
        const float air = thrust_lp_.Bandpass(Noise(), G(900.0f + 1400.0f * h + 500.0f * Sin(lfo_ph_ + 0.25f)), 1.1f) *
                          (0.012f + 0.018f * h);
        s += 0.5f * (vl + vr) * level + whine + air;
        side += 0.25f * (vl - vr) * level + air * 0.25f * Noise();
      }

      float l = s + side, r = s - side;
      l = 0.65f * std::tanh(l * (1.0f / 0.65f));
      r = 0.65f * std::tanh(r * (1.0f / 0.65f));
      lr[2 * i] = l;
      lr[2 * i + 1] = r;
    }
    quiet_.store(!busy && flux_ < 1e-3f && flux_target == 0.0f && charge_ < 1e-3f && hover_ < 1e-3f &&
                     hover_target == 0.0f && chirp_t_ >= 0.5f && key_t_ >= 0.09f && enter_t_ >= 0.24f &&
                     error_t_ >= 0.3f && empty_t_ >= 0.5f && relay_ < 1e-3f && pop_ < 1e-3f && ice_ < 1e-3f &&
                     tink_ < 1e-3f,
                 std::memory_order_relaxed);
  }

 private:
  template <typename F>
  static void Take(const std::atomic<uint32_t>& seq, uint32_t& seen, F start) {
    const uint32_t now = seq.load(std::memory_order_relaxed);
    if (now != seen) {
      seen = now;
      start();
    }
  }
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
  // A metal clank at `at` seconds: a struck band of noise ringing out.
  float Clank(float t, float at, float hz, float amp, int slot) {
    const float u = t - at;
    if (u < 0.0f || u > 0.12f)
      return 0.0f;
    return clank_bp_[slot & 3].Bandpass(Noise(), G(hz), 0.9f) * amp * 2.0f * std::exp(-u / 0.018f);
  }
  static float G(float hz) { return std::min(3.14159265f * hz / kRate, 1.4f); }
  static float Wrap(float p) { return p - std::floor(p); }
  static float Sin(float p) { return std::sin(6.28318531f * p); }
  static float Square(float p) { return p < 0.5f ? 1.0f : -1.0f; }
  // A sawtooth with its step smoothed (polyBLEP): `inc`, the phase step a sample.
  static float Saw(float p, float inc) {
    float v = 2.0f * p - 1.0f;
    if (p < inc) {
      const float t = p / inc;
      v -= t + t - t * t - 1.0f;
    } else if (p > 1.0f - inc) {
      const float t = (p - 1.0f) / inc;
      v -= t * t + t + t + 1.0f;
    }
    return v;
  }
  static float Hump(float u) {
    const float h = 4.0f * u * (1.0f - u);
    return h < 0.0f ? 0.0f : h;
  }
  float Uniform() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return float(seed_ >> 8) * (1.0f / 16777216.0f);
  }
  float Noise() { return Uniform() * 2.0f - 1.0f; }

  std::atomic<uint32_t> on_seq_{0}, off_seq_{0}, key_seq_{0}, key_digit_{0}, enter_seq_{0}, error_seq_{0};
  std::atomic<uint32_t> depart_seq_{0}, arrive_seq_{0}, vent_seq_{0}, pluto_seq_{0}, fusion_seq_{0};
  std::atomic<uint32_t> convert_seq_{0}, empty_seq_{0};
  std::atomic<bool> depart_fly_{false}, convert_up_{false}, quiet_{true};
  std::atomic<float> flux_t_{0}, charge_t_{0}, hover_t_{0};
  // (the mixer thread's own)
  uint32_t on_seen_ = 0, off_seen_ = 0, key_seen_ = 0, enter_seen_ = 0, error_seen_ = 0, depart_seen_ = 0;
  uint32_t arrive_seen_ = 0, vent_seen_ = 0, pluto_seen_ = 0, fusion_seen_ = 0, convert_seen_ = 0, empty_seen_ = 0;
  float flux_ = 0, charge_ = 0, hover_ = 0;
  float chirp_t_ = 1, chirp_ph_ = 0, relay_ = 0;
  bool chirp_up_ = true;
  float key_t_ = 1, key_hz_ = 1000, key_ph_ = 0, enter_t_ = 1, enter_ph_ = 0, error_t_ = 1, error_ph_ = 0;
  float empty_t_ = 1, empty_ph_ = 0;
  float pulse_ph_ = 0, zap_ = 0, hum_ph_ = 0;
  float whine_ph_ = 0, vib_ph_ = 0, arc_ = 0, arc_hz_ = 3000, buzz_ph_ = 0;
  float depart_t_ = 10, surge_ph_ = 0, boom_ph_ = 0, pop_ = 0;
  bool depart_flying_ = false;
  float arrive_t_ = 10, arr_ph_[3]{};
  float vent_t_ = 10, ice_ = 0, ice_hz_ = 4000;
  float pluto_t_ = 10, seat_ph_ = 0, react_ph_ = 0;
  float fusion_t_ = 10, tink_ = 0, tink_hz_ = 3000, motor_ph_ = 0, grind_ = 0, up_ph_ = 0;
  float convert_t_ = 10, servo_ph_ = 0, thr_ph_ = 0;
  float lfo_ph_ = 0, saw1_ph_ = 0, saw2_ph_ = 0.37f, sub_ph_ = 0, throb_ph_ = 0, vib2_ph_ = 0;
  bool convert_to_flight_ = false;
  uint32_t seed_ = 0x1985BEEFu;
  Svf relay_bp_, click_bp_, zap_bp_, arc_bp_, buzz_bp_, surge_bp_, crack_bp_, rumble_lp_, whoosh_bp_, fire_lp_,
      pop_bp_, arr_crack_[3], arr_lp_[3], vent_bp_, vent_lp_, ice_bp_, slide_bp_, seat_lp_, seal_bp_, grind_bp_,
      tink_bp_, spool_lp_, thrust_lp_, clank_bp_[4], pad_l_, pad_r_;
};

}  // namespace gta4::car
