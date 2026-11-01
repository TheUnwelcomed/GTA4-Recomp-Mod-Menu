// LibertyRecomp Menu: the jets' flight model and autopilot (the F-16, the
// Batwing). Plain C++ with no game code: the menu feeds it the stick, the
// jet's height over the ground and where it is, and sets the jet's attitude
// and speed from it each frame (SET_VEHICLE_QUATERNION, SET_CAR_FORWARD_SPEED:
// the jet flies along its nose). A PC test flies it through takeoffs,
// turns, stalls and landings (tools/jet/flight_test.cpp in the menu's
// repository).
//
// What it is meant to feel like (v1.7.22; before, the jet turned at once and
// held 25 m/s at idle, so it could not be slowed down to land):
//  - weight: the engines take seconds to change the speed (5 m/s^2 at full
//    dry power, 8.5 with the afterburner, at most), the stick asks for a
//    load factor that the jet takes half a second to reach, the roll builds
//    up over a third of a second; pulling g bleeds speed (induced drag);
//  - the wing: it lifts 1 g at its stall speed (42 m/s clean, 36 with the
//    gear and flaps down) and the square of the speed over that, up to 7 g:
//    slow, it cannot hold itself up and sinks, nose first;
//  - hands off, the fly-by-wire holds the flight path: level, it flies
//    level; banked up to 60 degrees it turns level; past that it sinks;
//  - the gear: down, it drags (70% more) and lowers the stall speed; the air
//    brake doubles the drag; on the wheels, the brakes stop it in about
//    7 seconds from landing speed;
//  - the autopilot: hold (heading, height, speed), land straight ahead (a
//    3.5 degree approach to the ground in front, a flare, the brakes) and
//    fly to a runway and land on it (a fix 3.2 km out on its line first).
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
  float k_induced;                      // drag from lift: k (n g)^2 / v^2
  float stall_clean, stall_gear;        // 1 g stall speeds, m/s
  float max_g, min_g;
  float roll_rate, yaw_rate;            // rad/s at full authority
  float roll_tau, g_tau;                // how long the roll and the load factor take to answer, s
  float max_speed;
  float rolling, wheel_brake;           // on the wheels, m/s^2
};
// The F-16 (GTA's scale: tops out at 80 m/s on full dry power, 104 with the
// afterburner; lands at about 40).
inline constexpr Params kF16{0.6f,  5.0f,  3.5f,  0.00078f, 0.7f, 1.0f, 1.0f,  10.0f, 42.0f,
                             36.0f, 7.0f, -2.5f, 2.0f,     0.30f, 0.35f, 0.5f, 110.0f, 0.25f, 6.0f};
// The Batwing: more engine, a bigger wing (slower stall), a quicker roll.
inline constexpr Params kBatwing{0.7f,  5.8f,  4.0f,  0.00082f, 0.6f, 1.1f, 1.0f,  9.0f, 38.0f,
                                 33.0f, 7.5f, -2.5f, 2.4f,     0.32f, 0.30f, 0.45f, 115.0f, 0.25f, 6.5f};

inline float StallSpeed(const Params& p, bool gear_down) { return gear_down ? p.stall_gear : p.stall_clean; }
// The speed it may leave the ground at (pulling back): 10% over the stall.
inline float RotateSpeed(const Params& p, bool gear_down) { return 1.1f * StallSpeed(p, gear_down); }

struct Input {
  float pitch = 0, roll = 0, yaw = 0;  // the stick (pull back +), the rudder (right +); -1..1
  float throttle = 0, airbrake = 0;    // 0..1
  bool burner = false, gear_down = true;
  // The autopilot flies a climb angle (rad) instead of the stick's pitch.
  bool path = false;
  float gamma = 0;
};
struct State {
  Q q;
  float speed = 0;
  float n = 1, p = 0, r = 0;  // load factor (g), roll and yaw rates (rad/s)
  bool stalled = false;
};

// The load factor that keeps the flight path where it is, hands off.
inline float TrimG(const Attitude& a) {
  const float cg = std::cos(a.gamma);
  const float cb = std::cos(a.bank);
  if (cb <= 0.0f)
    return 0.0f;  // upside down: no help
  return cg / std::max(cb, 0.5f);
}

// One step in the air.
inline void Fly(const Params& P, State& s, const Input& in, float dt) {
  if (dt <= 0.0f)
    return;
  const Attitude a = Look(s.q);
  const float v = std::max(s.speed, 1.0f);
  const float vs = StallSpeed(P, in.gear_down);
  const float lift = (v * v) / (vs * vs);  // g the wing gives at full angle of attack
  const float n_max = std::min(P.max_g, lift), n_min = std::max(P.min_g, -0.6f * lift);
  const float trim = Clamp(TrimG(a), n_min, n_max);
  float want;
  if (in.path)
    want = trim + Clamp(1.4f * (in.gamma - a.gamma) * v / kG, -1.2f, 2.0f);
  else if (in.pitch >= 0.0f)
    want = trim + in.pitch * (n_max - trim);
  else
    want = trim + in.pitch * (trim - n_min);
  want = Clamp(want, n_min, n_max);
  s.stalled = v < vs * 0.98f;
  s.n += (want - s.n) * (1.0f - std::exp(-dt / P.g_tau));
  s.n = Clamp(s.n, n_min, n_max);
  // Roll and rudder: authority grows with the speed up to just over the stall.
  const float authority = Clamp((v - 12.0f) / (P.stall_clean * 1.1f - 12.0f), 0.25f, 1.0f);
  const float follow = 1.0f - std::exp(-dt / P.roll_tau);
  s.p += (Clamp(in.roll, -1.0f, 1.0f) * P.roll_rate * authority - s.p) * follow;
  s.r += (Clamp(in.yaw, -1.0f, 1.0f) * P.yaw_rate * authority - s.r) * follow;
  s.q = Mul(s.q, Axis({0, 1, 0}, s.p * dt));    // right: the right wing down
  s.q = Mul(s.q, Axis({0, 0, 1}, -s.r * dt));   // right rudder: the nose right
  // The path bends with what pulls across it: the wing's lift (n g, along
  // its up axis) and gravity.
  const V3 pull = a.u * (s.n * kG) + V3{0, 0, -kG};
  const V3 across = pull - a.f * Dot(pull, a.f);
  const V3 axis = Cross(a.f, across);
  const float m = Len(axis);
  if (m > 1e-5f)
    s.q = Mul(Axis(axis * (1.0f / m), m / v * dt), s.q);
  // Speed: thrust against drag (the airframe's, the gear's, the air brake's,
  // the lift's) and gravity along the path.
  const float thrust = P.idle + (P.dry - P.idle) * Clamp(in.throttle, 0.0f, 1.0f) + (in.burner ? P.burner : 0.0f);
  const float cd = P.cd0 * (1.0f + (in.gear_down ? P.gear_drag : 0.0f) + in.airbrake * P.brake_drag);
  const float ng = s.n * kG;
  const float drag = cd * v * v + in.airbrake * P.brake_flat + P.k_induced * ng * ng / std::max(v * v, 400.0f);
  s.speed = Clamp(s.speed + (thrust - drag - kG * a.f.z) * dt, 0.0f, P.max_speed);
  s.q = Norm(s.q);
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
constexpr float kApproachSpeed = 48.0f, kCruiseSpeed = 72.0f;

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
