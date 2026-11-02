// LibertyRecomp Menu: the jets' flight model and autopilot (the F-16, the
// Batwing). Plain C++ with no game code: the menu feeds it the stick, the
// jet's height over the ground and where it is, and sets the jet's attitude
// and speed from it each frame (SET_VEHICLE_QUATERNION, SET_CAR_FORWARD_SPEED:
// the jet flies along its nose). A PC test flies it through takeoffs,
// turns, stalls and landings (tools/jet/flight_test.cpp in the menu's
// repository).
//
// v1.7.24: arcade flight, the way Ace Combat and GTA V do it (v1.7.22's
// load-factor model asked for a 40 m/s takeoff roll the game's physics
// hardly reached, and turned slowly):
//  - a short takeoff roll: full power gets it to its rotate speed (120 km/h
//    for the F-16) in about five seconds; pulled back from there, it flies;
//  - the stick turns it at once: up to 60 degrees a second in pitch and 150
//    in roll, the rates easing in over a tenth of a second, softer slow;
//  - the flight path is the nose: it goes where it points, climbing costs
//    speed and diving gains it, the throttle sets the speed it settles at;
//  - Standard controls (Ace Combat's): left and right bank it, up to 70
//    degrees, and the bank turns it, holding its height; let go and the
//    wings level. Expert: the stick rolls and pulls it like GTA V's jets
//    (banked hands off, it slowly falls off into the turn; loops and rolls);
//  - below its stall speed the controls go soft and the nose drops until
//    the speed comes back (a "STALL" warning); nothing else takes control;
//  - the gear and the air brake add drag; on the wheels, the brakes stop it.
#pragma once

#include <algorithm>
#include <cmath>

namespace gta4::flight {

constexpr float kG = 9.81f;
constexpr float kPi = 3.14159265f;

// ---------------------------------------------------------------- math
struct V3 {
  float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }

// The menu's quaternion (x, y, z, w; the car's own axes: x right, y nose, z up).
struct Q {
  float x = 0, y = 0, z = 0, w = 1;
};
inline Q Mul(const Q& a, const Q& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q Axis(V3 a, float angle) {
  const float h = angle * 0.5f, s = std::sin(h);
  return {a.x * s, a.y * s, a.z * s, std::cos(h)};
}
inline Q Norm(Q q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(n > 1e-6f) || !std::isfinite(n))
    return {};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}
inline V3 Rot(const Q& q, V3 v) {
  const float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
  return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
          v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
// Angles in (-pi, pi].
inline float Wrap(float a) {
  while (a > kPi)
    a -= 2 * kPi;
  while (a <= -kPi)
    a += 2 * kPi;
  return a;
}
inline float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
// A quaternion facing `yaw` (the game's heading: 0 north, counter-clockwise),
// nose `pitch` up, wings level.
inline Q Facing(float yaw, float pitch = 0.0f) {
  return Mul(Axis({0, 0, 1}, yaw), Axis({1, 0, 0}, pitch));
}

// Where the jet points: its flight path's climb angle, its bank (right wing
// down positive), its heading (as Facing's) and its axes.
struct Attitude {
  float gamma = 0, bank = 0, yaw = 0;
  V3 f, r, u;
};
inline Attitude Look(const Q& q) {
  Attitude a;
  a.f = Rot(q, {0, 1, 0});
  a.r = Rot(q, {1, 0, 0});
  a.u = Rot(q, {0, 0, 1});
  a.gamma = std::asin(Clamp(a.f.z, -1.0f, 1.0f));
  a.bank = std::atan2(-a.r.z, a.u.z);
  a.yaw = std::atan2(-a.f.x, a.f.y);
  return a;
}

// ---------------------------------------------------------------- the jet
struct Params {
  float idle, dry, burner;              // thrust over mass, m/s^2 (the burner's on top of full dry)
  float cd0, gear_drag, brake_drag;     // drag over mass per (m/s)^2; the gear's and the air brake's share
  float brake_flat;                     // the air brake's own, m/s^2
  float stall_clean, stall_gear;        // below these (m/s) the controls go soft and the nose drops
  float pitch_rate, roll_rate, yaw_rate;  // rad/s at full authority
  float rate_tau;                       // how long the rates take to follow the stick, s
  float max_bank;                       // Standard controls: the bank full stick asks for, rad
  float max_speed;
  float rolling, wheel_brake;           // on the wheels, m/s^2
  float rotate;                         // m/s: pulled back from here on the wheels, it flies
};
// The F-16 (GTA's scale: settles at about 75 m/s on full dry power and 95
// with the afterburner; flies from 120 km/h; stalls below 90).
inline constexpr Params kF16{1.6f,  7.0f,  5.0f,  0.00125f, 0.6f, 1.2f, 4.0f,  25.0f, 22.0f,
                             1.05f, 2.6f,  0.45f, 0.10f,    1.22f, 110.0f, 0.3f, 7.0f, 33.0f};
// The Batwing: more engine, a bigger wing (slower), a quicker roll.
inline constexpr Params kBatwing{1.7f,  7.6f,  5.0f,  0.00130f, 0.5f, 1.2f, 4.0f,  23.0f, 20.0f,
                                 1.10f, 2.9f,  0.50f, 0.09f,    1.22f, 112.0f, 0.3f, 7.0f, 30.0f};

inline float StallSpeed(const Params& p, bool gear_down) { return gear_down ? p.stall_gear : p.stall_clean; }
// The speed it may leave the ground at (pulling back).
inline float RotateSpeed(const Params& p, bool gear_down) {
  (void)gear_down;
  return p.rotate;
}

struct Input {
  float pitch = 0, roll = 0, yaw = 0;  // the stick (pull back +), the rudder (right +); -1..1
  float throttle = 0, airbrake = 0;    // 0..1
  bool burner = false, gear_down = true;
  // Standard controls: `roll` asks for a bank (of max_bank), and the bank
  // turns the jet holding its height; centred, the wings level.
  bool assist = false;
  // The autopilot flies a climb angle (rad) instead of the stick's pitch
  // (its turns coordinated as Standard's).
  bool path = false;
  float gamma = 0;
};
struct State {
  Q q;
  float speed = 0;
  float n = 1, p = 0, r = 0, pq = 0;  // load factor (g, for the HUD); roll, yaw and pitch rates (rad/s)
  bool stalled = false;
};

// How much of the controls' rates it has at speed `v`: a fifth at a crawl,
// all of it from a little over the stall.
inline float Authority(const Params& P, float v, bool gear_down) {
  const float vs = StallSpeed(P, gear_down);
  return Clamp((v - 0.4f * vs) / (0.8f * vs), 0.2f, 1.0f);
}

// One step in the air.
inline void Fly(const Params& P, State& s, const Input& in, float dt) {
  if (dt <= 0.0f)
    return;
  const Attitude a = Look(s.q);
  const float v = std::max(s.speed, 1.0f);
  const float vs = StallSpeed(P, in.gear_down);
  const float auth = Authority(P, v, in.gear_down);
  const float lift = std::min(1.0f, (v * v) / (vs * vs));  // 1: the wing holds it up
  s.stalled = v < vs * 0.98f;
  const bool coordinated = in.assist || in.path;
  // The turn a bank makes, level (the autopilot's and Standard's): g tan(bank) / v.
  float turn_q = 0.0f, turn_r = 0.0f;
  if (coordinated && std::fabs(a.bank) < 1.45f) {
    const float w = kG * std::tan(a.bank) / std::max(v, 15.0f) * lift;
    turn_q = w * std::sin(a.bank);
    turn_r = w * std::cos(a.bank);
  }
  float want_p, want_q, want_r;
  if (in.path) {
    // The autopilot: the stick's roll as a rate, the climb angle held.
    want_p = Clamp(in.roll, -1.0f, 1.0f) * P.roll_rate * auth;
    const float gamma_rate = Clamp(1.5f * (in.gamma - a.gamma), -0.25f, 0.25f);
    want_q = gamma_rate / std::max(std::cos(a.bank), 0.3f) + turn_q;
    want_r = Clamp(in.yaw, -1.0f, 1.0f) * P.yaw_rate * auth + turn_r;
  } else if (in.assist) {
    // Standard: the stick asks for a bank; centred, the wings level.
    const float want_bank = Clamp(in.roll, -1.0f, 1.0f) * P.max_bank;
    want_p = Clamp(3.5f * Wrap(want_bank - a.bank), -P.roll_rate, P.roll_rate) * auth;
    want_q = Clamp(in.pitch, -1.0f, 1.0f) * P.pitch_rate * auth + turn_q;
    want_r = Clamp(in.yaw, -1.0f, 1.0f) * P.yaw_rate * auth + turn_r;
  } else {
    // Expert: the stick rolls and pulls.
    want_p = Clamp(in.roll, -1.0f, 1.0f) * P.roll_rate * auth;
    want_q = Clamp(in.pitch, -1.0f, 1.0f) * P.pitch_rate * auth;
    want_r = Clamp(in.yaw, -1.0f, 1.0f) * P.yaw_rate * auth;
  }
  // Stalled, pulling back does less and less (the nose drops, pulled or not).
  if (want_q > 0.0f)
    want_q *= lift;
  const float follow = 1.0f - std::exp(-dt / P.rate_tau);
  s.p += (want_p - s.p) * follow;
  s.pq += (want_q - s.pq) * follow;
  s.r += (want_r - s.r) * follow;
  s.q = Mul(s.q, Axis({0, 1, 0}, s.p * dt));    // right: the right wing down
  s.q = Mul(s.q, Axis({1, 0, 0}, s.pq * dt));   // back: the nose up
  s.q = Mul(s.q, Axis({0, 0, 1}, -s.r * dt));   // right rudder: the nose right
  // Gravity: what the wing doesn't hold up pulls the nose towards the ground
  // (slow: the stall; Expert, banked steeply: falling off into the turn).
  const float held = coordinated ? lift : lift * std::fabs(std::cos(a.bank));
  const float fall = (kG / std::max(v, 8.0f)) * std::cos(a.gamma) * (1.0f - held);
  if (fall > 1e-5f) {
    const Attitude b = Look(s.q);
    const V3 down{0, 0, -1};
    V3 axis = Cross(b.f, down);
    const float m = Len(axis);
    if (m > 1e-4f)
      s.q = Mul(Axis(axis * (1.0f / m), std::min(fall * dt, 0.05f)), s.q);
  }
  // Speed: thrust against drag (the airframe's, the gear's, the air
  // brake's) and gravity along the path.
  const float thrust = P.idle + (P.dry - P.idle) * Clamp(in.throttle, 0.0f, 1.0f) + (in.burner ? P.burner : 0.0f);
  const float cd = P.cd0 * (1.0f + (in.gear_down ? P.gear_drag : 0.0f) + in.airbrake * P.brake_drag);
  const float drag = cd * v * v + in.airbrake * P.brake_flat;
  const float climb = Look(s.q).f.z;
  s.speed = Clamp(s.speed + (thrust - drag - kG * climb) * dt, 0.0f, P.max_speed);
  s.q = Norm(s.q);
  // The load factor the HUD shows: the pull's and gravity's.
  s.n = Clamp(v * s.pq / kG + std::cos(a.gamma) * std::cos(a.bank), -4.0f, 9.0f);
}

// On the wheels: the speed after `dt` (the engines against rolling
// resistance, the air's drag and the brakes).
inline float Roll(const Params& P, float speed, float throttle, bool burner, float brake, bool gear_down, float dt) {
  const float thrust = P.idle + (P.dry - P.idle) * Clamp(throttle, 0.0f, 1.0f) + (burner ? P.burner : 0.0f);
  const float cd = P.cd0 * (1.0f + (gear_down ? P.gear_drag : 0.0f));
  float a = thrust - cd * speed * speed - P.rolling - Clamp(brake, 0.0f, 1.0f) * P.wheel_brake;
  if (throttle < 0.05f && speed < 3.0f)
    a = std::min(a, 0.0f);  // (idle thrust doesn't creep it along)
  return std::max(0.0f, speed + a * dt);
}

// ---------------------------------------------------------------- autopilot
enum class Mode { kOff, kHold, kLand, kRoute };
enum class Phase { kHold, kToFix, kIntercept, kFinal, kFlare, kRollout, kStopped, kAbort };

constexpr float kGlide = 0.0611f;  // 3.5 degrees
constexpr float kFixOut = 3200.0f;  // the route's fix, metres before the touchdown point
constexpr float kApproachSpeed = 42.0f, kCruiseSpeed = 70.0f;

struct Runway {
  V3 aim;          // the touchdown point (its z the ground there)
  float yaw = 0;   // the landing direction, as Facing's heading
};
inline V3 RunwayDir(const Runway& r) { return {-std::sin(r.yaw), std::cos(r.yaw), 0}; }
inline V3 RunwayRight(const Runway& r) { return {std::cos(r.yaw), std::sin(r.yaw), 0}; }

struct Pilot {
  Mode mode = Mode::kOff;
  Phase phase = Phase::kHold;
  float hold_yaw = 0, hold_z = 0, hold_speed = kCruiseSpeed;
  Runway runway;
  float throttle_i = 0.5f;  // the speed loop's integrator
  float fix_out = 3200.0f;  // how far out the route's fix is (further after a pass too close)
  int retries = 0;          // approaches flown again (too high, too close)
  bool gear_down = false, brakes = false;
  const char* why = "";      // the last abort's reason
  const char* where = "FRANCIS INTL";  // what the route flies to (the HUD's words)
};
struct Situation {
  V3 pos;              // the jet's origin
  float radar = 100;   // its height over the ground under it, less its height standing on its wheels
  bool on_ground = false;
  float ground_speed = 0;
};

inline void Engage(Pilot& p, Mode mode, const Attitude& a, const Situation& s, const Runway* runway = nullptr) {
  p.mode = mode;
  p.why = "";
  p.throttle_i = 0.55f;
  p.brakes = false;
  if (mode == Mode::kHold) {
    p.phase = Phase::kHold;
    p.hold_yaw = a.yaw;
    p.hold_z = s.pos.z + std::max(0.0f, 60.0f - s.radar);  // (at least 60 m up)
    p.hold_speed = kCruiseSpeed;
  } else if (runway) {
    p.runway = *runway;
    p.fix_out = kFixOut;
    p.retries = 0;
    p.phase = mode == Mode::kRoute ? Phase::kToFix : Phase::kFinal;
  }
}

// The aim's distance ahead along the runway, and how far right of its line the jet is.
inline float AlongTrack(const Runway& r, V3 pos) { return Dot(r.aim - pos, RunwayDir(r)); }
inline float CrossTrack(const Runway& r, V3 pos) { return Dot(pos - r.aim, RunwayRight(r)); }

// The stick, throttle and brakes the autopilot asks for this frame.
inline Input Steer(const Params& P, Pilot& p, const State& st, const Situation& sit, float dt) {
  Input in;
  const Attitude a = Look(st.q);
  const float v = st.speed;
  float want_yaw = a.yaw, want_gamma = 0, want_speed = p.hold_speed;
  bool flare = false;
  p.gear_down = false;
  switch (p.phase) {
    case Phase::kHold:
      want_yaw = p.hold_yaw;
      want_gamma = Clamp(0.006f * (p.hold_z - sit.pos.z), -0.15f, 0.15f);
      want_speed = p.hold_speed;
      break;
    case Phase::kToFix: {
      // Out to the fix on the runway's line, at the glide's height there.
      // Coming in from in front of the fix, it is aimed for from the side
      // (a base leg, 1.2 km out to the side the jet is on), so the turn onto
      // the line is a quarter turn, not a U-turn.
      const V3 dir = RunwayDir(p.runway);
      const float along = AlongTrack(p.runway, sit.pos), cross = CrossTrack(p.runway, sit.pos);
      V3 fix = p.runway.aim - dir * p.fix_out;
      if (along < p.fix_out - 300.0f)
        fix = fix + RunwayRight(p.runway) * (cross >= 0.0f ? 1200.0f : -1200.0f);
      const float fix_z = p.runway.aim.z + p.fix_out * kGlide;
      const V3 to = fix - sit.pos;
      want_yaw = std::atan2(-to.x, to.y);
      want_gamma = Clamp(0.006f * (fix_z + 40.0f - sit.pos.z), -0.15f, 0.15f);
      want_speed = kCruiseSpeed;
      if (std::sqrt(to.x * to.x + to.y * to.y) < 600.0f)
        p.phase = Phase::kIntercept;
      break;
    }
    case Phase::kIntercept: {
      // Onto the line (cutting in at up to 57 degrees), turning however far
      // it takes, then down the glide. Too close to get down: out again.
      const float along = AlongTrack(p.runway, sit.pos), cross = CrossTrack(p.runway, sit.pos);
      want_yaw = Wrap(p.runway.yaw + Clamp(0.003f * cross, -1.0f, 1.0f));
      // (down towards the glide's height here, a little over it)
      want_gamma = Clamp(0.006f * (p.runway.aim.z + std::max(along, 0.0f) * kGlide + 25.0f - sit.pos.z), -0.12f,
                         0.15f);
      want_speed = kCruiseSpeed;
      if (along < 1500.0f) {
        p.fix_out = 4500.0f;
        p.phase = Phase::kToFix;
      } else if (std::fabs(cross) < 120.0f && std::fabs(Wrap(a.yaw - p.runway.yaw)) < 0.35f) {
        p.phase = Phase::kFinal;
      }
      break;
    }
    case Phase::kFinal:
    case Phase::kFlare: {
      const float along = AlongTrack(p.runway, sit.pos), cross = CrossTrack(p.runway, sit.pos);
      // Off the line, past the aim or far too high: around again (a route,
      // twice), else give up, climbing away.
      const float glide_z = p.runway.aim.z + std::max(along, 0.0f) * kGlide;
      const bool high = along < 1200.0f && sit.pos.z - glide_z > 60.0f && p.phase == Phase::kFinal;
      if (std::fabs(cross) > 900.0f || along < -150.0f || high) {
        p.why = std::fabs(cross) > 900.0f ? "off the runway's line" : along < -150.0f ? "past the runway" : "too high";
        if (p.mode == Mode::kRoute && p.retries < 2 && p.phase == Phase::kFinal) {
          ++p.retries;
          p.fix_out = 4500.0f;
          p.phase = Phase::kToFix;
        } else {
          p.phase = Phase::kAbort;
        }
        break;
      }
      want_yaw = Wrap(p.runway.yaw + Clamp(0.004f * cross, -0.5f, 0.5f));
      p.gear_down = along < 3600.0f || p.phase == Phase::kFlare;
      want_speed = p.gear_down ? kApproachSpeed : kCruiseSpeed;
      want_gamma = -kGlide + Clamp(0.012f * (glide_z - sit.pos.z), -0.07f, 0.06f);
      if (sit.radar < 7.0f && along < 900.0f)
        p.phase = Phase::kFlare;
      if (p.phase == Phase::kFlare) {
        flare = true;
        want_gamma = -0.012f - 0.004f * Clamp(sit.radar, 0.0f, 7.0f);
        want_yaw = p.runway.yaw;
      }
      break;
    }
    case Phase::kRollout:
    case Phase::kStopped:
      p.gear_down = true;
      p.brakes = true;
      in.gear_down = true;
      in.throttle = 0;
      in.airbrake = 1;
      return in;
    case Phase::kAbort:
      // Climb away on the heading it has, then hold.
      p.mode = Mode::kHold;
      p.phase = Phase::kHold;
      p.hold_yaw = a.yaw;
      p.hold_z = sit.pos.z + 120.0f;
      p.hold_speed = kCruiseSpeed;
      want_yaw = a.yaw;
      want_gamma = 0.1f;
      break;
  }
  // Heading: bank towards it (35 degrees at most; 15 on the glide), the roll damped.
  const float max_bank = p.phase == Phase::kFinal || p.phase == Phase::kFlare ? 0.26f
                         : p.phase == Phase::kIntercept                           ? 0.70f
                                                                                  : 0.61f;
  const float bank = Clamp(-1.1f * Wrap(want_yaw - a.yaw), -max_bank, max_bank);
  in.roll = Clamp(2.0f * (bank - a.bank) - 0.45f * st.p, -1.0f, 1.0f);
  in.path = true;
  in.gamma = want_gamma;
  in.gear_down = p.gear_down;
  // Speed: the throttle's integrator trims it; the air brake if far too fast.
  const float e = want_speed - v;
  if (flare) {
    in.throttle = 0.0f;
  } else {
    p.throttle_i = Clamp(p.throttle_i + 0.02f * e * dt, 0.0f, 1.0f);
    in.throttle = Clamp(p.throttle_i + 0.10f * e, 0.0f, 1.0f);
  }
  in.airbrake = e < -6.0f ? Clamp(-(e + 6.0f) * 0.12f, 0.0f, 1.0f) : 0.0f;
  in.burner = false;
  (void)P;
  return in;
}

}  // namespace gta4::flight
