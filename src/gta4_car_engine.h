// Liberty Trainer: the add-on cars' engines, synthesized.
//
// Original voices made from scratch here: nothing in them comes from films,
// games or any recording. They play through the port's own audio mixer
// (ps5/game/sound.h), each car's game name having no engine (or door) sound
// in IV. One synth, a character per car:
//
//  - the Batmobile: in the spirit of the 1989/1992 film car, a jet turbine
//    over a big, lumpy cross-plane V8, a deep roar on the booster;
//  - the Grotti Deimos, the Dinka Nexus, the Imponte Arbiter X and the
//    Vapid Dominator 645 on a physically informed model (Physical, below)
//    of the engines of the real cars they are drawn from: a late-70s
//    Ferrari's flat-plane 3-litre V8, the early-90s Honda NSX's 3-litre
//    VTEC V6, the 1969 Pontiac GTO's Ram Air 400 V8 and the 1965 Mustang
//    GT's 289 V8.
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
//    latch click first) and shutting (ending in a clunk), or a gullwing's
//    gas struts alone (the game's own door sounds then do the latch); a
//    pop-up lamp's electric motor; a door's latch letting go as it opens,
//    and its slam (a heavy thump, the panel's ring, the latch catching, a
//    little rattle) as it shuts.
//
// The physically informed model: each cylinder fires at its own crank angle
// (the real firing order, each on its bank), and its exhaust pulse (the
// blowdown's sharp spike, then the exhaust stroke's flow with its
// turbulence) varies a little from one cycle to the next, a lot more on a
// lumpy cam at idle. The pulses run into their bank's exhaust: the header,
// the pipe and the tailpipe as delay lines with the reflections at their
// ends (so the resonances sit where the pipes' lengths put them, the revs
// sweeping the harmonics through them, as on a real car), the crossover
// between the banks, the muffler's low-pass and its chamber. The intake's
// suction pulses ring the airbox (a Helmholtz resonance) and the air hisses
// with the flow; a four-barrel's secondaries roar at full throttle; the
// valve gear ticks; the starter whirs. (v1.7.17 gave these three cars the
// original voice, which sounded synthetic.)
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

// A real engine's layout, for the physically informed model.
struct Physical {
  int cylinders;
  float fire_deg[8];               // each firing in the 720-degree cycle, in firing order
  int bank[8];                     // the bank (0 left, 1 right) of each of those firings
  float exhaust_deg;               // how long the exhaust valve is open (crank degrees)
  float blowdown_ms;               // the blowdown spike's decay at 3,000 rpm (slower at idle, sharper high)
  float header_m, pipe_m, tail_m;  // the exhaust's sections, one way (m)
  float header_r, pipe_r, tail_r;  // the reflections at their ends
  float crossover;                 // between the banks: 0 true duals .. 0.5 merged
  float muffler_hz;                // the muffler's low-pass
  float chamber_hz, chamber;       // its chamber's resonance and weight
  float weight;                    // the pulses' strength (displacement)
  float turbulence;                // the flow's noise in each pulse
  float jitter, lope;              // cycle-to-cycle variation; the cam's lumpiness at idle
  float intake, intake_hz;         // the suction pulses and the airbox's resonance
  float air, air_hz;               // the air's hiss with the flow
  float carb_roar;                 // a four-barrel's secondaries at full throttle
  float valvetrain;                // the valve gear's ticking
  float cam_rpm, cam_gain;         // a second cam profile (VTEC): from cam_rpm, more pulse and intake
  float level;                     // the exhaust's level in the mix
  // A diesel's: the combustion's knock ringing through the block with each
  // firing (its strength, the block's ring), and a turbo whistling as the
  // boost builds (its level, its pitch at full boost).
  float knock = 0.0f, knock_hz = 0.0f;
  float turbo = 0.0f, turbo_hz = 0.0f;
};

// What makes one engine sound like itself.
struct Character {
  float idle_rpm = 0.0f, limit_rpm = 0.0f;              // real revs at idle and at the limit
  float beat[8]{};                                      // each firing pulse's strength, in firing order
  float lope = 0.0f;                                    // the idle's lumpiness
  float exhaust_hz = 0.0f, exhaust_per = 0.0f;          // the exhaust resonance: hz + per x the firing rate
  float thump = 0.0f;                                   // the firing rate's own low thump
  float growl = 0.0f, growl_load = 0.0f;                // saturation drive, and more under load
  float level = 0.0f, level_load = 0.0f, level_revs = 0.0f;
  float bright = 0.0f, bright_revs = 0.0f;              // the rumble's top: hz + revs x (0..1)
  float howl = 0.0f;                                    // the carburettors' tone at twice the firing rate
  float sub = 0.0f;                                     // the firing rate's own tone: weight in the low end
  float turbine = 0.0f;                                 // 0: none
  float turbine_hz = 0.0f, turbine_range = 0.0f;        // the whine's pitch: hz + range x spool
  float intake = 0.0f, intake_load = 0.0f;              // rasp level, and under load
  float intake_hz = 0.0f, intake_revs = 0.0f;           // its band: hz + revs x (0..1)
  float pops = 0.0f;                                    // lift-off pops (a rate)
  float flare = 0.0f;                                   // the catch's flare, times idle
  float roar_hz = 0.0f, roar_range = 0.0f;              // the booster's roar: its top, hz + range x boost
  float jet = 0.0f, jet_hz = 0.0f, jet_range = 0.0f;    // a turbine's roar under load: level, top hz + range x revs^2
  // Full throttle, high revs (none at idle): the V8 opening up (hz more of
  // its top), the turbine's roar churning (its depth, with a low roar under
  // it), and the combustion crackling (a rate).
  float snarl = 0.0f, churn = 0.0f, crackle = 0.0f;
  // Set: the physically informed model, which uses only the revs, the pops
  // and the flare above.
  const Physical* physical = nullptr;
};
// The Batmobile, after the film car (Batman Returns): a turbine car. The
// turbine's hum at idle, its roar rising under load (a jet's: noise opening
// up with the revs, kept below ~1.6 kHz), its whine under it but never shrill
// (to ~1.1 kHz), a big slow V8's beat and weight for the body, a deep roar on
// the booster. (v1.7.11-v1.7.13: a bright V8 and a turbine whining to 3.3
// kHz; v1.7.14: the V8 alone, dark and low.)
// v1.7.18: meaner flat out, after the film car's: the V8 snarls (more
// drive, its top opening up in the low mids under load), the turbine's roar
// churns over a low roar with the combustion crackling in it; still no
// whine on top. Idle unchanged.
constexpr Character kBatmobile{650.0f,  5200.0f, {1.00f, 0.80f, 1.12f, 0.93f, 0.86f, 1.06f, 0.78f, 1.16f},
                               0.45f,   55.0f,   1.0f,  2.1f, 2.1f, 3.8f, 0.10f, 0.09f, 0.04f, 480.0f, 820.0f,
                               0.0f,    0.07f,   0.45f, 420.0f, 700.0f, 0.004f, 0.022f, 220.0f, 520.0f, 16.0f,
                               1.8f,    90.0f,   650.0f, 0.24f, 260.0f, 1300.0f, 450.0f, 0.6f, 5.0f};

// The Imponte Arbiter X: the 1969 GTO's Ram Air 400. GM's firing order
// 1-8-4-3-6-5-7-2, odd cylinders on the left bank: each bank fires unevenly
// (270, 180, 90, 180 degrees apart), which on true dual exhausts is the
// cross-plane V8's burble. A long-duration cam (a choppy idle), cast-iron
// manifolds into long pipes and free-flowing mufflers, the Quadrajet's
// secondaries and the hood scoops roaring when it is floored.
constexpr Physical kArbiterEngine{8, {0, 90, 180, 270, 360, 450, 540, 630}, {0, 1, 1, 0, 1, 0, 0, 1},
                                  262.0f, 1.4f, 0.7f, 2.6f, 0.45f, -0.25f, 0.25f, -0.3f, 0.12f,
                                  750.0f, 90.0f, 0.3f, 1.0f, 0.35f, 0.06f, 0.55f, 0.25f, 140.0f,
                                  0.08f, 1800.0f, 0.5f, 0.04f, 0.0f, 0.0f, 1.54f};
// The Vapid Dominator 645: the 1965 Mustang GT's 289 Hi-Po. Ford's firing
// order 1-5-4-2-6-3-7-8, cylinders 1-4 on the right bank: uneven on each
// bank too, a smaller, raspier V8 than the Pontiac, revving higher, dual
// exhausts with an H-pipe, a solid-lifter cam (a milder lope, the valve
// gear ticking at idle), an Autolite four-barrel.
constexpr Physical kDominatorEngine{8, {0, 90, 180, 270, 360, 450, 540, 630}, {1, 0, 1, 1, 0, 1, 0, 0},
                                    250.0f, 1.1f, 0.6f, 2.3f, 0.35f, -0.25f, 0.25f, -0.3f, 0.2f,
                                    1000.0f, 130.0f, 0.25f, 0.75f, 0.45f, 0.05f, 0.3f, 0.2f, 170.0f,
                                    0.07f, 2200.0f, 0.35f, 0.07f, 0.0f, 0.0f, 2.14f};
// The Dinka Nexus: the early-90s NSX's 3-litre V6, its 90-degree vee on
// offset crankpins firing evenly (1-4-2-5-3-6, 120 degrees apart, the
// banks taking turns) into one exhaust, equal-length headers, a short run
// to the muffler (mid-engined), fuel injection (a steady idle). From 5,800
// rpm the high-lift cam: stronger pulses, a sharper blowdown and the intake
// opening up into a wail to 8,000.
constexpr Physical kNexusEngine{6, {0, 120, 240, 360, 480, 600}, {0, 1, 0, 1, 0, 1},
                                240.0f, 0.75f, 0.5f, 1.1f, 0.3f, -0.3f, 0.25f, -0.3f, 0.5f,
                                1500.0f, 210.0f, 0.2f, 0.55f, 0.3f, 0.025f, 0.04f, 0.9f, 700.0f,
                                0.08f, 2600.0f, 0.0f, 0.025f, 5800.0f, 0.8f, 2.27f};
// The Grotti Deimos: the late-70s/early-80s mid-engined Ferrari V8 it
// echoes, a 3-litre flat-plane V8 (1-5-3-7-4-8-2-6, the banks taking turns,
// each firing evenly every 180 degrees: two fours in one, a hard, even
// rasp), long tubular headers merging in one muffler box (a short run,
// mid-engined), four twin-choke carburettors on individual runners (the
// intake's howl and honk at full throttle), revving to 7,700. (v1.7.13-17:
// the original voice.)
constexpr Physical kDeimosEngine{8, {0, 90, 180, 270, 360, 450, 540, 630}, {0, 1, 0, 1, 0, 1, 0, 1},
                                 240.0f, 0.8f, 0.65f, 0.7f, 0.25f, -0.3f, 0.25f, -0.3f, 0.4f,
                                 1800.0f, 240.0f, 0.2f, 0.6f, 0.4f, 0.04f, 0.12f, 0.6f, 600.0f,
                                 0.1f, 3000.0f, 0.3f, 0.035f, 0.0f, 0.0f, 2.49f};
// The Bravado Gauntlet Classic: the 1970 Challenger's 440 Six Pack, 7.2
// litres of cross-plane V8 (Chrysler's firing order 1-8-4-3-6-5-7-2, odd
// cylinders on the left bank: the GTO's uneven banks, bigger and slower), a
// long cam (a heavy lope at idle), cast-iron manifolds into long true duals
// and big, low mufflers (deeper than the Pontiac), and three two-barrel
// carburettors whose outer pair open when it is floored: the Six Pack's roar.
constexpr Physical kGauntletEngine{8, {0, 90, 180, 270, 360, 450, 540, 630}, {0, 1, 1, 0, 1, 0, 0, 1},
                                   268.0f, 1.6f, 0.75f, 2.9f, 0.5f, -0.25f, 0.25f, -0.3f, 0.08f,
                                   620.0f, 80.0f, 0.35f, 1.2f, 0.38f, 0.06f, 0.65f, 0.3f, 120.0f,
                                   0.08f, 1600.0f, 0.75f, 0.035f, 0.0f, 0.0f, 1.40f};
// The Lazar 3: a big turbo-diesel inline six (an armoured carrier's
// 9-litre class), firing 1-5-3-6-2-4 every 120 degrees into one manifold,
// the turbo's turbine taking the edge off the pulses, a short run to a big
// muffler and the stacks at the back; idling at 650, governed at 2,300. Its
// combustion knocks through the block with every firing (the diesel's
// clatter, loudest at idle and pulling hard) and the turbo whistles up as
// the boost builds.
constexpr Physical kLazarEngine{6, {0, 120, 240, 360, 480, 600}, {0, 0, 0, 0, 0, 0},
                                250.0f, 2.4f, 0.9f, 1.6f, 0.6f, -0.2f, 0.2f, -0.25f, 0.5f,
                                430.0f, 70.0f, 0.45f, 1.7f, 0.3f, 0.035f, 0.0f, 0.35f, 85.0f,
                                0.07f, 1300.0f, 0.0f, 0.03f, 0.0f, 0.0f, 1.15f,
                                0.22f, 1300.0f, 0.22f, 3800.0f};
constexpr Character kDeimos{.idle_rpm = 950.0f, .limit_rpm = 7700.0f, .pops = 8.0f, .flare = 2.0f,
                            .physical = &kDeimosEngine};
constexpr Character kGauntlet{.idle_rpm = 800.0f, .limit_rpm = 5800.0f, .pops = 11.0f, .flare = 1.9f,
                              .physical = &kGauntletEngine};
constexpr Character kLazar{.idle_rpm = 650.0f, .limit_rpm = 2300.0f, .pops = 0.0f, .flare = 1.5f,
                           .physical = &kLazarEngine};
constexpr Character kArbiter{.idle_rpm = 850.0f, .limit_rpm = 5600.0f, .pops = 10.0f, .flare = 1.9f,
                             .physical = &kArbiterEngine};
constexpr Character kDominator{.idle_rpm = 750.0f, .limit_rpm = 6500.0f, .pops = 9.0f, .flare = 2.0f,
                               .physical = &kDominatorEngine};
constexpr Character kNexus{.idle_rpm = 800.0f, .limit_rpm = 8000.0f, .pops = 3.0f, .flare = 1.8f,
                           .physical = &kNexusEngine};

class EngineSynth {
 public:
  static constexpr float kRate = 48000.0f;
  static constexpr float kIdle = 0.16f;  // normalized revs at idle

  explicit EngineSynth(const Character& character) : c_(character), p_(character.physical) {
    if (!p_)
      return;
    constexpr float kSound = 560.0f;  // m/s in the hot exhaust gas
    auto samples = [](float metres) { return std::clamp(int(2.0f * metres / kSound * kRate + 0.5f), 1, kDelay - 1); };
    // (the right bank's pipes a little longer, as a real car's routing makes
    // them: the banks' unevenness survives in the sum, the burble)
    for (int b = 0; b < 2; ++b) {
      const float skew = b ? 1.13f : 1.0f;
      header_d_[b] = samples(p_->header_m * skew);
      pipe_d_[b] = samples(p_->pipe_m * skew);
      tail_d_[b] = samples(p_->tail_m * (b ? 1.07f : 1.0f));
    }
    for (int c = 0; c < std::clamp(p_->cylinders, 1, 8); ++c) {
      fire_[c] = p_->fire_deg[c] / 720.0f;
      trim_[c] = 1.0f + 0.08f * std::sin(2.4f * float(c) + 0.7f);  // no two cylinders quite alike
    }
  }

  // rpm: 0 off, kIdle idle, 1 the rev limit (the caller works it out from road
  // speed and gears); throttle and boost 0..1; volume 0..1.
  void SetTarget(float rpm, float throttle, float boost, float volume) {
    target_rpm_.store(std::clamp(rpm, 0.0f, 1.0f), std::memory_order_relaxed);
    target_throttle_.store(std::clamp(throttle, 0.0f, 1.0f), std::memory_order_relaxed);
    target_boost_.store(std::clamp(boost, 0.0f, 1.0f), std::memory_order_relaxed);
    target_volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
  }
  // The canopy's hiss; latch: its click first opening, its clunk shutting
  // (false: the struts alone, for a door the game itself latches).
  void CanopyOpen(bool latch = true) {
    (latch ? opens_ : struts_up_).fetch_add(1, std::memory_order_relaxed);
  }
  void CanopyShut(bool latch = true) {
    (latch ? shuts_ : struts_down_).fetch_add(1, std::memory_order_relaxed);
  }
  // A door: its latch letting go as it opens; its slam as it shuts.
  void DoorOpen() { door_opens_.fetch_add(1, std::memory_order_relaxed); }
  void DoorShut() { door_shuts_.fetch_add(1, std::memory_order_relaxed); }
  // A pop-up lamp's motor: a whir for `seconds`, a soft clunk at the end.
  void Motor(float seconds) {
    motor_len_x1000_.store(uint32_t(std::clamp(seconds, 0.1f, 2.0f) * 1000.0f), std::memory_order_relaxed);
    motors_.fetch_add(1, std::memory_order_relaxed);
  }
  // An armoured car's (the Lazar's): its cannon's report; its air brakes
  // letting the pressure go at a stop; its ramps' hydraulics for `seconds`
  // (opening: the locks clank first; either way the ramp lands at the end);
  // its turret's traverse motors (0 still .. 1 slewing flat out, held).
  void Cannon() { cannons_.fetch_add(1, std::memory_order_relaxed); }
  void AirBrake() { air_brakes_.fetch_add(1, std::memory_order_relaxed); }
  void Hydraulic(float seconds, bool opening) {
    hyd_len_x1000_.store(uint32_t(std::clamp(seconds, 0.2f, 4.0f) * 1000.0f), std::memory_order_relaxed);
    (opening ? hyd_opens_ : hyd_shuts_).fetch_add(1, std::memory_order_relaxed);
  }
  void Servo(float level) { servo_target_.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed); }

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
    for (uint32_t n = struts_up_.exchange(0, std::memory_order_relaxed); n; --n)
      hiss_t_ = 0.0f, hiss_len_ = 0.55f, hiss_rising_ = true;
    for (uint32_t n = struts_down_.exchange(0, std::memory_order_relaxed); n; --n)
      hiss_t_ = 0.0f, hiss_len_ = 0.6f, hiss_rising_ = false, clunk_at_ = 0.0f;
    for (uint32_t n = door_opens_.exchange(0, std::memory_order_relaxed); n; --n)
      door_open_t_ = 0.0f;
    for (uint32_t n = door_shuts_.exchange(0, std::memory_order_relaxed); n; --n)
      door_shut_t_ = 0.0f, slam_ph_ = 0.0f;
    for (uint32_t n = motors_.exchange(0, std::memory_order_relaxed); n; --n) {
      motor_t_ = 0.0f;
      motor_len_ = float(motor_len_x1000_.load(std::memory_order_relaxed)) / 1000.0f;
    }
    for (uint32_t n = cannons_.exchange(0, std::memory_order_relaxed); n; --n)
      cannon_t_ = 0.0f, boom_ph_ = 0.0f;
    for (uint32_t n = air_brakes_.exchange(0, std::memory_order_relaxed); n; --n)
      air_t_ = 0.0f;
    for (int open = 0; open < 2; ++open)
      for (uint32_t n = (open ? hyd_opens_ : hyd_shuts_).exchange(0, std::memory_order_relaxed); n; --n) {
        hyd_t_ = 0.0f;
        hyd_len_ = float(hyd_len_x1000_.load(std::memory_order_relaxed)) / 1000.0f;
        hyd_opening_ = open != 0;
      }
    const float servo_target = servo_target_.load(std::memory_order_relaxed);
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

      // ---- the engine: the physically informed model, or the original voice
      float eng_l = 0.0f, eng_r = 0.0f;
      if (p_)
        PhysicalSample(rpm_real, r, cranking, eng_l, eng_r);
      else
        ClassicSample(i, rpm_real, r, cranking, eng_l, eng_r);

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
        // (about 9 dB down from v1.7.17, which played it over the engine)
        canopy += (motor_bp_.Bandpass(buzz, 0.06f, 0.9f) * 0.035f + Sin(gear_ph_) * 0.0045f) * env;
        motor_t_ += dt;
        if (motor_t_ >= motor_len_)
          clunk_ = std::max(clunk_, 0.13f);
      }
      if (clunk_ > 1e-4f) {
        clunk_ph_ = Wrap(clunk_ph_ + 68.0f * dt);
        canopy += (Sin(clunk_ph_) * 0.30f + clunk_lp_.Lowpass(Noise(), 0.04f, 1.0f) * 0.5f) * clunk_;
        clunk_ *= 0.9993f;  // (~30 ms)
      } else {
        clunk_ph_ = 0.0f;
      }
      // ---- a door opening: the latch lets go (a crisp click and a lower
      // clack from the handle's linkage), then the door comes off its seal
      // (a soft, low thud) 25 ms later
      if (door_open_t_ < 0.12f) {
        const float t = door_open_t_;
        float d = door_hi_bp_.Bandpass(Noise(), 3.14159265f * 2800.0f / kRate, 1.6f) * 0.22f * std::exp(-t / 0.004f);
        d += door_mid_bp_.Bandpass(Noise(), 3.14159265f * 1150.0f / kRate, 1.4f) * 0.16f * std::exp(-t / 0.010f);
        if (t > 0.025f) {
          const float u = t - 0.025f;
          seal_ph_ = Wrap(seal_ph_ + 135.0f * dt);
          d += (door_lo_lp_.Lowpass(Noise(), 3.14159265f * 300.0f / kRate, 1.0f) * 0.20f + Sin(seal_ph_) * 0.08f) *
               std::exp(-u / 0.022f);
        }
        canopy += d;
        door_open_t_ += dt;
      }
      // ---- a door shutting: a heavy thump falling in pitch, the panel's
      // ring and the body's boom, the latch catching 6 ms in, a light rattle
      if (door_shut_t_ < 0.25f) {
        const float t = door_shut_t_;
        slam_ph_ = Wrap(slam_ph_ + (62.0f + 34.0f * std::exp(-t / 0.03f)) * dt);
        float d = Sin(slam_ph_) * 0.20f * std::exp(-t / 0.055f) * std::min(1.0f, t * 900.0f);
        d += slam_bp_.Bandpass(Noise(), 3.14159265f * 450.0f / kRate, 1.2f) * 0.13f * std::exp(-t / 0.035f);
        d += slam_lp_.Lowpass(Noise(), 3.14159265f * 220.0f / kRate, 1.0f) * 0.12f * std::exp(-t / 0.045f);
        if (t > 0.006f)
          d += latch_bp_.Bandpass(Noise(), 3.14159265f * 2200.0f / kRate, 1.6f) * 0.09f *
               std::exp(-(t - 0.006f) / 0.005f);
        if (t > 0.015f)
          d += rattle_bp_.Bandpass(Noise(), 3.14159265f * 1400.0f / kRate, 1.0f) * 0.03f *
               std::exp(-(t - 0.015f) / 0.04f);
        canopy += d;
        door_shut_t_ += dt;
      }
      // ---- an armoured car's cannon: the muzzle's crack, the blast's boom
      // falling in pitch with its body, the rumble rolling off, the breech
      // clanking the next round in
      if (cannon_t_ < 0.7f) {
        const float t = cannon_t_;
        boom_ph_ = Wrap(boom_ph_ + (36.0f + 74.0f * std::exp(-t / 0.035f)) * dt);
        float d = Sin(boom_ph_) * 0.42f * std::exp(-t / 0.10f) * std::min(1.0f, t * 2000.0f);
        d += crack_hi_.Bandpass(Noise(), 3.14159265f * 2600.0f / kRate, 0.7f) * 1.5f * std::exp(-t / 0.005f);
        d += blast_lp_.Lowpass(Noise(), 3.14159265f * 380.0f / kRate, 0.9f) * 2.8f * std::exp(-t / 0.045f);
        d += rumble_lp_tail_.Lowpass(Noise(), 3.14159265f * 140.0f / kRate, 0.9f) * 2.2f * std::exp(-t / 0.22f) *
             std::min(1.0f, t * 40.0f);
        if (t > 0.085f)
          d += breech_bp_.Bandpass(Noise(), 3.14159265f * 1700.0f / kRate, 1.3f) * 0.45f *
               std::exp(-(t - 0.085f) / 0.012f);
        canopy += d;
        cannon_t_ += dt;
      }
      // ---- air brakes at a stop: the valve's pop, then the pressure's hiss
      if (air_t_ < 1.0f) {
        const float t = air_t_;
        const float env = std::min(1.0f, t * 300.0f) * std::exp(-t / 0.28f);
        float d = (brake_bp_.Bandpass(Noise(), 3.14159265f * 4200.0f / kRate, 0.9f) * 0.55f +
                   brake_lp_.Lowpass(Noise(), 3.14159265f * 9000.0f / kRate, 0.8f) * 0.12f) * env;
        if (t < 0.012f)
          d += pop_bp_.Bandpass(Noise(), 3.14159265f * 900.0f / kRate, 1.0f) * 0.6f * (1.0f - t / 0.012f);
        canopy += d;
        air_t_ += dt;
      }
      // ---- ramp hydraulics: the pump's strained whine rising with the
      // pressure, the oil rushing through the valves; opening, the locks
      // clank first; at the end the ramp lands (on its stop out, home in its
      // frame shut)
      if (hyd_t_ < hyd_len_) {
        const float t = hyd_t_, u = t / hyd_len_;
        const float env = std::min(1.0f, t * 12.0f) * std::min(1.0f, (hyd_len_ - t) * 10.0f);
        pump_ph_ = Wrap(pump_ph_ + (310.0f + 60.0f * u) * dt);
        const float whine =
            Sin(pump_ph_) + 0.45f * Sin(2.0f * pump_ph_) + 0.25f * Sin(3.0f * pump_ph_) + 0.12f * Sin(5.0f * pump_ph_);
        float d = whine * 0.03f * env;
        d += fluid_bp_.Bandpass(Noise(), 3.14159265f * 1500.0f / kRate, 1.0f) * 0.1f * env;
        if (hyd_opening_ && t < 0.25f) {
          d += lock_bp_.Bandpass(Noise(), 3.14159265f * 1900.0f / kRate, 1.2f) * 0.5f * std::exp(-t / 0.012f);
          if (t > 0.09f)
            d += lock2_bp_.Bandpass(Noise(), 3.14159265f * 1300.0f / kRate, 1.2f) * 0.35f *
                 std::exp(-(t - 0.09f) / 0.015f);
        }
        canopy += d;
        hyd_t_ += dt;
        if (hyd_t_ >= hyd_len_)
          land_t_ = 0.0f, land_ph_ = 0.0f;
      }
      if (land_t_ < 0.3f) {
        const float t = land_t_;
        land_ph_ = Wrap(land_ph_ + (46.0f + 30.0f * std::exp(-t / 0.02f)) * dt);
        canopy += (Sin(land_ph_) * 0.32f * std::exp(-t / 0.07f) * std::min(1.0f, t * 1500.0f) +
                   land_lp_.Lowpass(Noise(), 3.14159265f * 260.0f / kRate, 1.0f) * 1.6f * std::exp(-t / 0.04f) +
                   land_bp_.Bandpass(Noise(), 3.14159265f * 900.0f / kRate, 1.4f) * 0.45f * std::exp(-t / 0.02f)) *
                  (hyd_opening_ ? 0.75f : 1.0f);
        land_t_ += dt;
      }
      // ---- a turret's traverse: the motors' whine rising with the slew,
      // the gear ring's whine over it
      servo_ += (servo_target - servo_) * 15.0f * dt;
      if (servo_ > 1e-3f) {
        servo_ph_ = Wrap(servo_ph_ + (180.0f + 240.0f * servo_) * dt);
        ring_ph_ = Wrap(ring_ph_ + (1400.0f + 900.0f * servo_) * dt);
        const float buzz = Sin(servo_ph_) + 0.5f * Sin(2.0f * servo_ph_) + 0.3f * Sin(4.0f * servo_ph_);
        canopy += (servo_bp_.Bandpass(buzz, 3.14159265f * 520.0f / kRate, 0.8f) * 0.07f + Sin(ring_ph_) * 0.007f) *
                  std::min(1.0f, servo_ * 3.0f);
      }

      float l = eng_l * vol_ + canopy;
      float rr = eng_r * vol_ + canopy;
      // a soft ceiling at 0.6
      l = 0.6f * std::tanh(l * (1.0f / 0.6f));
      rr = 0.6f * std::tanh(rr * (1.0f / 0.6f));
      lr[2 * i] = l;
      lr[2 * i + 1] = rr;
    }
    rpm_out_.store(rpm_, std::memory_order_relaxed);
    quiet_.store(!running_ && rpm_ < 0.002f && turb_ < 0.01f && boost_ < 0.01f && crack_ < 1e-3f &&
                     hiss_t_ >= hiss_len_ && click_ < 1e-3f && clunk_ < 1e-3f && motor_t_ >= motor_len_ &&
                     door_open_t_ >= 0.12f && door_shut_t_ >= 0.25f && cannon_t_ >= 0.7f && air_t_ >= 1.0f &&
                     hyd_t_ >= hyd_len_ && land_t_ >= 0.3f && servo_ < 1e-3f && servo_target < 1e-3f &&
                     spool_ < 0.02f,
                 std::memory_order_relaxed);
  }

 private:
  static constexpr float kDt = 1.0f / kRate;
  // ---- the original voice (the Batmobile's, the Deimos's): one sample
  void ClassicSample(size_t i, float rpm_real, float r, bool cranking, float& out_l, float& out_r) {
    constexpr float dt = kDt;
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
    // (flat out, the snarl: the top opens further in the low mids)
    const float flat_out = throttle_ * r;
    rumble = rumble_lp_.Lowpass(dc_y_, 3.14159265f * (c_.bright + c_.bright_revs * r + c_.snarl * flat_out) / kRate,
                                0.9f);
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
      if (c_.churn > 0.0f) {
        // flat out the roar churns (a turbulent flutter, 10-40 Hz) over a
        // low roar of its own: a jet at full power, not a hiss
        churn_ += (Noise() - churn_) * 0.003f;
        const float depth = c_.churn * flat_out;
        const float flutter = 1.0f + depth * std::clamp(churn_ * 6.0f, -1.0f, 1.0f);
        const float low = jet_lo_.Lowpass(Noise(), 3.14159265f * 160.0f / kRate, 0.8f) * jamp * depth * 1.6f;
        jet_l = jet_l * flutter + low;
        jet_r = jet_r * flutter + low;
      }
    }

    // ---- booster: a deep roar opening up, and sparse crackles
    const float roar_g = 3.14159265f * (c_.roar_hz + c_.roar_range * boost_) / kRate;
    const float roar_amp = 0.33f * boost_;
    const float roar_l = roar_l_.Lowpass(Noise(), roar_g, 0.8f) * roar_amp;
    const float roar_r = roar_r_.Lowpass(Noise(), roar_g, 0.8f) * roar_amp;
    float crack = 0.0f;
    if (boost_ > 0.05f && Uniform() < boost_ * 30.0f * dt)
      crack_ = 0.5f + 0.5f * Uniform();
    // flat out, the combustion crackles now and then
    if (c_.crackle > 0.0f && running_ && throttle_ > 0.85f && r > 0.5f &&
        Uniform() < c_.crackle * (r - 0.5f) * 2.0f * dt)
      crack_ = std::max(crack_, 0.2f + 0.25f * Uniform());
    // lift-off: the throttle shut at high revs pops the exhaust
    if (running_ && throttle_ < 0.15f && r > 0.35f && Uniform() < (r - 0.3f) * c_.pops * dt)
      crack_ = std::max(crack_, 0.35f + 0.45f * Uniform());
    if (crack_ > 1e-4f) {
      crack = crack_bp_.Bandpass(Noise() * crack_, 0.06f, 1.2f) * 0.6f;
      crack_ *= 0.9985f;  // (~15 ms)
    }
    const float shared = rumble + crack;
    out_l = shared + whine_l + rasp_l + roar_l + jet_l;
    out_r = shared * 0.97f + whine_r + rasp_r + roar_r + jet_r;
  }

  // ---- the physically informed engine: one sample
  float Gauss() { return (Uniform() + Uniform() + Uniform() - 1.5f) * 2.0f; }  // about N(0, 1)
  static float Hump(float u) {  // a smooth bump on 0..1, 1 at its middle (close to sin^2(pi u))
    const float h = 4.0f * u * (1.0f - u);
    return h * h;
  }
  struct Pulse {
    float amp = 0.0f, t = 0.0f, inv_len = 0.0f;
    float spike = 0.0f, decay = 0.0f;  // the blowdown's envelope and its per-sample decay
    int wait = 0, bank = 0;
    bool intake = false;
  };
  void Spawn(const Pulse& q) {
    for (auto& slot : pulses_)
      if (slot.amp == 0.0f) {
        slot = q;
        return;
      }
  }
  // A cylinder fires: its exhaust pulse (and the intake's suction for the
  // cycle), with this cycle's variation; the valve gear ticks.
  void Fire(int c, float rpm_real, float r, float load, float cam, bool cranking) {
    const Physical& p = *p_;
    const float rpm = std::max(rpm_real, 60.0f);
    const float idle = (1.0f - r) * (1.0f - r);
    float amp = p.weight * trim_[c] * load * (0.85f + 0.15f * std::sin(3.14159265f * r)) *
                (1.0f + 0.5f * p.cam_gain * cam);
    amp *= std::max(0.2f, 1.0f + (p.jitter + 0.5f * p.lope * idle) * Gauss());
    if (Uniform() < 0.12f * p.lope * idle)
      amp *= 0.4f;  // a lazy firing on a lumpy cam
    if (cranking)
      amp = 0.12f * p.weight;  // compression, no fire yet
    const float period = 120.0f / rpm / float(p.cylinders);  // seconds between firings
    Pulse q;
    q.amp = amp;
    q.bank = p.bank[c] & 1;
    q.inv_len = 6.0f * rpm / p.exhaust_deg;
    // the blowdown follows the valve's opening: slower at idle, sharper high
    const float blow = std::clamp(p.blowdown_ms * 0.001f * (3000.0f / rpm), 0.0003f, 0.008f);
    q.decay = std::exp(-kDt / (blow * (1.0f - 0.25f * cam)));
    q.wait = int(Uniform() * (0.025f + 0.05f * p.lope * idle) * period * kRate);
    Spawn(q);
    // a diesel's combustion knocks: hardest at idle and pulling hard
    if (p.knock > 0.0f && !cranking)
      knock_ = std::max(knock_, p.knock * (0.45f + 0.55f * load) * (1.15f - 0.55f * r) *
                                    (0.85f + 0.3f * Uniform()));
    Pulse in;
    in.amp = (cranking ? 0.1f : 0.25f + 0.75f * throttle_) * (1.0f + p.cam_gain * cam);
    in.inv_len = 6.0f * rpm / 230.0f;
    in.decay = std::exp(-kDt / std::clamp(0.0004f * (3000.0f / rpm), 0.00015f, 0.003f));
    in.wait = q.wait;
    in.intake = true;
    Spawn(in);
    tick_ = 1.0f;
  }
  void PhysicalSample(float rpm_real, float r, bool cranking, float& out_l, float& out_r) {
    const Physical& p = *p_;
    constexpr float dt = kDt;
    // the charge in each cylinder: the throttle's, with an idle's own (firm
    // enough to burble); closed at speed (engine braking) much less
    const float closed = 0.4f - 0.28f * r;
    const float load = cranking ? 0.0f : std::clamp(throttle_ + (1.0f - throttle_) * closed, 0.0f, 1.0f);
    const float cam = p.cam_rpm > 0.0f ? Smooth(p.cam_rpm - 150.0f, p.cam_rpm + 150.0f, rpm_real) : 0.0f;
    // the revs never sit quite still: a slow wander, wider on a lumpy idle
    if (++wander_n_ >= 512) {
      wander_n_ = 0;
      rpm_wander_ += (Noise() - rpm_wander_) * 0.08f;
    }
    const float steady = 1.0f + rpm_wander_ * (0.006f + 0.03f * p.lope * (1.0f - r) * (1.0f - r));
    // the cycle: two turns of the crank; each cylinder fires as its angle passes
    const float before = cyc_ph_;
    cyc_ph_ += rpm_real * steady / 120.0f * dt;
    const bool wrapped = cyc_ph_ >= 1.0f;
    if (wrapped)
      cyc_ph_ -= std::floor(cyc_ph_);
    if (rpm_ > 0.004f)
      for (int c = 0; c < std::clamp(p.cylinders, 1, 8); ++c) {
        const float f = fire_[c];
        if (wrapped ? (f > before || f <= cyc_ph_) : (f > before && f <= cyc_ph_))
          Fire(c, rpm_real, r, load, cam, cranking);
      }
    // the gas's turbulence: noise darkened above ~2 kHz, one source per bank
    float flow[2];
    for (int b = 0; b < 2; ++b) {
      flow_lp_[b] += 0.25f * (Noise() - flow_lp_[b]);
      flow[b] = flow_lp_[b] * 2.0f;
    }
    // the pulses in flight: the exhaust's into their banks (the blowdown's
    // spike, then the stroke's flow, the turbulence riding mostly on the
    // spike: the rasp), the intake's suction
    float bank[2] = {0.0f, 0.0f}, suction = 0.0f;
    const float rasp = p.turbulence * (0.3f + 0.7f * load);  // the harder it works, the rougher the flow
    for (auto& q : pulses_) {
      if (q.amp == 0.0f)
        continue;
      if (q.wait > 0) {
        --q.wait;
        continue;
      }
      const float u = q.t * q.inv_len;
      const float hump = u < 1.0f ? Hump(u) : 0.0f;
      if (q.intake) {
        q.spike = q.t < 0.0001f ? q.t * (1.0f / 0.0001f) : q.spike * q.decay;
        suction -= q.amp * (0.6f * q.spike + hump);
        if (u >= 1.0f)
          q.amp = 0.0f;
      } else {
        if (q.t < 0.00015f)
          q.spike = q.t * (1.0f / 0.00015f);
        else
          q.spike *= q.decay;
        bank[q.bank] += q.amp * (q.spike + 0.35f * hump + rasp * (0.7f * q.spike + 0.15f * hump) * flow[q.bank]);
        if (u >= 1.0f && q.spike < 1e-3f)
          q.amp = 0.0f;
      }
      q.t += dt;
    }
    // lift-off: the throttle shut at high revs, unburnt fuel pops in the exhaust
    if (running_ && throttle_ < 0.15f && r > 0.35f && Uniform() < (r - 0.3f) * c_.pops * dt)
      crack_ = std::max(crack_, 0.35f + 0.45f * Uniform());
    if (crack_ > 1e-4f) {
      const float pop = crack_ * Noise() * 1.6f * p.weight;
      bank[0] += pop;
      bank[1] += pop * (0.6f + 0.4f * Noise());
      crack_ *= 0.9985f;  // (~15 ms)
    }
    // the exhaust: headers (their reflection back from the collector), the
    // crossover, the pipes, the mufflers, the tailpipes
    float side[2];
    for (int b = 0; b < 2; ++b) {
      header_lp_[b] += 0.55f * (header_[b].Read(header_d_[b]) - header_lp_[b]);
      const float h = bank[b] + p.header_r * header_lp_[b];
      header_[b].Write(h);
      side[b] = h;
    }
    const float x = p.crossover;
    const float into[2] = {(1.0f - x) * side[0] + x * side[1], (1.0f - x) * side[1] + x * side[0]};
    const float gm = 3.14159265f * p.muffler_hz / kRate, gc = 3.14159265f * p.chamber_hz / kRate;
    float pipe_out[2];
    for (int b = 0; b < 2; ++b) {
      pipe_lp_[b] += 0.4f * (pipe_[b].Read(pipe_d_[b]) - pipe_lp_[b]);
      const float pp = into[b] + p.pipe_r * pipe_lp_[b];
      pipe_[b].Write(pp);
      const float m = muffler_[b].Lowpass(pp, gm, 1.0f) + p.chamber * chamber_[b].Bandpass(pp, gc, 0.6f);
      tail_lp_[b] += 0.5f * (tail_[b].Read(tail_d_[b]) - tail_lp_[b]);
      float tl = m + p.tail_r * tail_lp_[b];
      tail_[b].Write(tl);
      // the gas leaving the tailpipe roars with its own flow: noise riding
      // on the pressure there, rougher the harder the engine works
      env_[b] += 0.01f * (std::fabs(tl) - env_[b]);
      const float hiss = jet_[b].Lowpass(Noise(), 3.14159265f * (900.0f + 900.0f * r) / kRate, 0.9f);
      tl += hiss * env_[b] * p.turbulence * 0.8f * (0.3f + 0.7f * load);
      // radiated from the tailpipe: the one-sided pulses' DC goes (25 Hz)
      rad_y_[b] = tl - rad_x_[b] + 0.9967f * rad_y_[b];
      rad_x_[b] = tl;
      pipe_out[b] = rad_y_[b];
    }
    // the intake: the suction rings the airbox, the air hisses with the flow,
    // a four-barrel's secondaries roar when floored
    const float airflow = std::min(1.0f, rpm_real / c_.limit_rpm) * (0.15f + 0.85f * throttle_);
    float intake = intake_bp_.Bandpass(suction, 3.14159265f * p.intake_hz / kRate, 0.4f) * p.intake * 0.3f;
    intake += air_bp_.Bandpass(Noise(), 3.14159265f * p.air_hz / kRate, 1.2f) * p.air * 0.25f * airflow *
              (1.0f + p.cam_gain * cam);
    if (p.carb_roar > 0.0f && throttle_ > 0.6f)
      intake += carb_lp_.Lowpass(Noise(), 3.14159265f * 900.0f / kRate, 0.9f) * p.carb_roar *
                ((throttle_ - 0.6f) / 0.4f) * (0.3f + 0.7f * r) * 0.5f;
    // the valve gear: a small tick with each cylinder's valves
    float mech = 0.0f;
    if (tick_ > 1e-4f) {
      mech = tick_bp_.Bandpass(Noise() * tick_, 3.14159265f * 3500.0f / kRate, 1.4f) * p.valvetrain * 0.1f;
      tick_ *= 0.985f;  // (~1.4 ms)
    }
    // a diesel's knock: each combustion's hard edge ringing through the block
    if (knock_ > 1e-4f) {
      const float g = 3.14159265f * p.knock_hz / kRate;
      mech += (knock_hi_.Bandpass(Noise(), g, 1.1f) * 0.9f + knock_lo_.Bandpass(Noise(), g * 0.42f, 0.9f) * 0.6f) *
              knock_ * 0.35f;
      knock_ *= 0.9875f;  // (~1.7 ms)
    }
    // its turbo: the boost building with the load and the revs (lagging,
    // spinning down slower), a whistle and the compressor's rush
    if (p.turbo > 0.0f) {
      const float want = running_ && !cranking ? load * (0.25f + 0.75f * r) : 0.0f;
      spool_ += (want - spool_) * (want > spool_ ? 0.9f : 0.6f) * dt;
      const float f = p.turbo_hz * (0.3f + 0.7f * spool_);
      turbo_ph_ = Wrap(turbo_ph_ + f * (1.0f + 0.003f * rpm_wander_) * dt);
      const float amp = p.turbo * spool_ * spool_ * 0.012f;
      mech += Sin(turbo_ph_) * amp + turbo_bp_.Bandpass(Noise(), 3.14159265f * f / kRate, 0.15f) * amp * 2.0f;
    }
    // the starter: its motor's whir while it turns the engine over
    if (cranking) {
      starter_ph_ = Wrap(starter_ph_ + 190.0f * dt);
      mech += (Sin(starter_ph_) + 0.45f * Sin(3.0f * starter_ph_) + 0.2f * Sin(5.0f * starter_ph_)) * 0.03f +
              starter_bp_.Bandpass(Noise(), 3.14159265f * 1400.0f / kRate, 1.2f) * 0.02f;
    }
    // the two banks' tailpipes left and right (heard from behind the car),
    // the intake and the engine bay in the middle
    const float centre = intake + mech;
    out_l = ((pipe_out[0] * 0.72f + pipe_out[1] * 0.28f) * 0.5f + centre) * p.level;
    out_r = ((pipe_out[1] * 0.72f + pipe_out[0] * 0.28f) * 0.5f + centre) * p.level;
  }

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
  const Physical* const p_;
  // the physically informed model's state (the mixer thread's own)
  static constexpr int kDelay = 1024;  // samples (21 ms): the longest exhaust section's round trip fits
  struct Delay {
    float buf[kDelay]{};
    int w = 0;
    float Read(int d) const { return buf[(w - d) & (kDelay - 1)]; }
    void Write(float v) {
      buf[w] = std::fabs(v) < 1e-20f ? 0.0f : v;
      w = (w + 1) & (kDelay - 1);
    }
  };
  Delay header_[2], pipe_[2], tail_[2];
  int header_d_[2] = {1, 1}, pipe_d_[2] = {1, 1}, tail_d_[2] = {1, 1};
  float header_lp_[2]{}, pipe_lp_[2]{}, tail_lp_[2]{}, rad_x_[2]{}, rad_y_[2]{};
  float fire_[8]{}, trim_[8]{}, cyc_ph_ = 0.0f, tick_ = 0.0f, starter_ph_ = 0.0f, flow_lp_[2]{};
  float env_[2]{}, rpm_wander_ = 0.0f, churn_ = 0.0f;
  int wander_n_ = 0;
  Pulse pulses_[24]{};
  std::atomic<uint32_t> opens_{0}, shuts_{0}, motors_{0}, motor_len_x1000_{400};
  std::atomic<uint32_t> struts_up_{0}, struts_down_{0}, door_opens_{0}, door_shuts_{0};
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
  float door_open_t_ = 1.0f, door_shut_t_ = 1.0f, seal_ph_ = 0, slam_ph_ = 0;
  // an armoured car's: the cannon, the air brakes, the ramps' hydraulics, the
  // turret's motors; a diesel's knock and turbo
  std::atomic<uint32_t> cannons_{0}, air_brakes_{0}, hyd_opens_{0}, hyd_shuts_{0}, hyd_len_x1000_{1500};
  std::atomic<float> servo_target_{0};
  float cannon_t_ = 1.0f, boom_ph_ = 0, air_t_ = 1.0f;
  float hyd_t_ = 1.0f, hyd_len_ = 0.0f, pump_ph_ = 0, land_t_ = 1.0f, land_ph_ = 0;
  bool hyd_opening_ = false;
  float servo_ = 0, servo_ph_ = 0, ring_ph_ = 0;
  float knock_ = 0, spool_ = 0, turbo_ph_ = 0;
  Svf crack_hi_, blast_lp_, rumble_lp_tail_, breech_bp_, brake_bp_, brake_lp_, pop_bp_, fluid_bp_, lock_bp_,
      lock2_bp_, land_lp_, land_bp_, servo_bp_, knock_hi_, knock_lo_, turbo_bp_;
  uint32_t seed_ = 0x7F4A7C15u;
  Svf thump_lp_, exhaust_bp_, rumble_lp_, howl_bp_, rasp_l_, rasp_r_, roar_l_, roar_r_, jet_l_, jet_r_, crack_bp_,
      hiss_bp_, click_bp_, clunk_lp_, motor_bp_, door_hi_bp_, door_mid_bp_, door_lo_lp_, slam_bp_, slam_lp_,
      latch_bp_, rattle_bp_, muffler_[2], chamber_[2], intake_bp_, air_bp_, carb_lp_, tick_bp_, starter_bp_, jet_[2], jet_lo_;
};

}  // namespace gta4::car
