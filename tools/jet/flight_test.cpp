// Flies gta4_jet_flight.h on the PC: takeoff, top speeds, a stall, a level
// turn hands off, the autopilot's hold, a landing straight ahead and a
// route to a runway from 5 km out. Prints what happened; fails on nonsense.
// build: c++ -std=c++20 -O2 -I<menu src> flight_test.cpp -o flight_test
#include <cstdio>
#include <cstdlib>
#include "gta4_jet_flight.h"

using namespace gta4::flight;

namespace {
constexpr float kDt = 1.0f / 60.0f;
constexpr float kGround = 5.8f, kRest = 1.2f;
int failures = 0;
void Check(bool ok, const char* what) {
  std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
  failures += ok ? 0 : 1;
}

struct Sim {
  const Params& P;
  State st;
  V3 pos;
  bool flying = false;
  float t = 0;
  explicit Sim(const Params& p) : P(p) {}
  float Radar() const { return pos.z - kGround - kRest; }
  void Step(const Input& in) {
    if (flying) {
      Fly(P, st, in, kDt);
    }
    const Attitude a = Look(st.q);
    pos = pos + a.f * (st.speed * kDt);
    t += kDt;
  }
};

void TopSpeeds(const Params& P, const char* name) {
  std::printf("%s: level speeds\n", name);
  for (int mode = 0; mode < 3; ++mode) {
    Sim s(P);
    s.flying = true;
    s.st.speed = 70;
    s.pos = {0, 0, 300};
    Input in;
    in.gear_down = false;
    in.path = true;
    in.gamma = 0;
    in.throttle = mode == 0 ? 0.0f : 1.0f;
    in.burner = mode == 2;
    float sink_start = 0;
    for (int i = 0; i < 60 * 90; ++i) {
      if (mode == 0 && s.st.speed < StallSpeed(P, false) && sink_start == 0)
        sink_start = s.t;
      s.Step(in);
    }
    std::printf("  %-12s settles at %5.1f m/s, height %6.1f%s\n", mode == 0 ? "idle" : mode == 1 ? "full dry" : "afterburner",
                s.st.speed, s.pos.z, mode == 0 && sink_start ? " (below the stall: sinking)" : "");
    if (mode == 1)
      Check(s.st.speed > 70 && s.st.speed < 90, "full dry power tops out at 70-90 m/s");
    if (mode == 2)
      Check(s.st.speed > 95 && s.st.speed <= P.max_speed, "the afterburner tops out at 95+ m/s");
    if (mode == 0)
      Check(s.pos.z < 250, "at idle it can't hold its height");
  }
}

void Takeoff(const Params& P, const char* name) {
  std::printf("%s: takeoff on full dry power\n", name);
  Sim s(P);
  s.pos = {0, 0, kGround + kRest};
  float dist = 0;
  const float vr = RotateSpeed(P, true);
  while (s.st.speed < vr && s.t < 60) {
    s.st.speed = Roll(P, s.st.speed, 1.0f, false, 0.0f, true, kDt);
    dist += s.st.speed * kDt;
    s.t += kDt;
  }
  std::printf("  rotate speed %.1f m/s after %.1f s, %.0f m\n", vr, s.t, dist);
  Check(s.t > 5 && s.t < 15, "the roll to rotate speed takes 5-15 s");
  s.flying = true;
  s.st.q = Facing(0);
  Input in;
  in.throttle = 1;
  in.gear_down = true;
  float t0 = s.t;
  float max_z = 0;
  while (s.t - t0 < 12) {
    in.pitch = s.t - t0 < 4 ? 0.6f : 0.0f;
    in.gear_down = s.t - t0 < 5;
    s.Step(in);
    max_z = std::max(max_z, s.pos.z);
    if (s.Radar() < -0.01f) {
      std::printf("  back on the ground %.1f s after rotating\n", s.t - t0);
      break;
    }
  }
  const Attitude a = Look(s.st.q);
  std::printf("  12 s after rotating: %.0f m up, %.1f m/s, climbing at %.1f degrees\n", s.Radar(), s.st.speed,
              a.gamma * 57.3f);
  Check(s.Radar() > 30, "it climbs away after rotating");
}

void Turn(const Params& P, const char* name) {
  std::printf("%s: a 45-degree bank, hands off\n", name);
  Sim s(P);
  s.flying = true;
  s.st.speed = 75;
  s.pos = {0, 0, 300};
  Input in;
  in.gear_down = false;
  in.throttle = 0.9f;
  float yaw0 = 0;
  for (int i = 0; i < 60 * 30; ++i) {
    const Attitude a = Look(s.st.q);
    in.roll = a.bank < 0.785f && i < 60 * 3 ? 0.5f : (a.bank > 0.79f ? -0.3f : 0.0f);
    if (i == 60 * 3)
      yaw0 = a.yaw;
    s.Step(in);
  }
  const Attitude a = Look(s.st.q);
  std::printf("  after 30 s: bank %.0f, height %.0f (from 300), %.1f m/s, turned %.0f degrees in 27 s\n",
              a.bank * 57.3f, s.pos.z, s.st.speed, Wrap(a.yaw - yaw0) * 57.3f);
  Check(std::fabs(s.pos.z - 300) < 40, "a banked turn holds its height hands off");
}

void Stall(const Params& P, const char* name) {
  std::printf("%s: pulled up at idle until it stalls\n", name);
  Sim s(P);
  s.flying = true;
  s.st.speed = 60;
  s.pos = {0, 0, 600};
  Input in;
  in.gear_down = false;
  in.throttle = 0;
  in.pitch = 1;
  float min_speed = 99, lowest_gamma = 0;
  bool stalled = false;
  for (int i = 0; i < 60 * 25; ++i) {
    s.Step(in);
    min_speed = std::min(min_speed, s.st.speed);
    stalled |= s.st.stalled;
    lowest_gamma = std::min(lowest_gamma, Look(s.st.q).gamma);
    if (!std::isfinite(s.st.speed) || !std::isfinite(s.pos.z)) {
      Check(false, "numbers stay finite");
      return;
    }
  }
  std::printf("  slowest %.1f m/s, stalled %s, nose down to %.0f degrees, height now %.0f\n", min_speed,
              stalled ? "yes" : "no", lowest_gamma * 57.3f, s.pos.z);
  Check(stalled && lowest_gamma < -0.2f, "a stall drops the nose");
}

// An autopilot run: until stopped on the ground or the time runs out.
bool Autoland(const Params& P, Sim& s, Pilot& pilot, float limit, float& touch_sink, float& touch_speed,
              V3& touch_at) {
  bool touched = false;
  while (s.t < limit) {
    Situation sit;
    sit.pos = s.pos;
    sit.radar = s.Radar();
    sit.on_ground = !s.flying;
    const Phase before = pilot.phase;
    Input in = Steer(P, pilot, s.st, sit, kDt);
    if (pilot.phase != before && std::getenv("FLIGHT_TRACE"))
      std::printf("    t=%5.1f phase %d -> %d at (%.0f, %.0f, %.0f) along %.0f cross %.0f speed %.1f\n", s.t, int(before),
                  int(pilot.phase), s.pos.x, s.pos.y, s.pos.z, AlongTrack(pilot.runway, s.pos),
                  CrossTrack(pilot.runway, s.pos), s.st.speed);
    if (s.flying) {
      Fly(P, s.st, in, kDt);
      const Attitude a = Look(s.st.q);
      s.pos = s.pos + a.f * (s.st.speed * kDt);
      if (s.Radar() <= 0.0f) {
        touched = true;
        s.flying = false;
        touch_sink = -s.st.speed * a.f.z;
        touch_speed = s.st.speed;
        touch_at = s.pos;
        s.pos.z = kGround + kRest;
        pilot.phase = Phase::kRollout;
      }
    } else {
      s.st.speed = Roll(P, s.st.speed, 0, false, 1, true, kDt);
      const Attitude a = Look(s.st.q);
      s.pos = s.pos + V3{a.f.x, a.f.y, 0} * (s.st.speed * kDt);
      if (s.st.speed < 0.5f)
        return touched;
    }
    s.t += kDt;
    if (pilot.mode == Mode::kHold) {
      std::printf("  aborted: %s\n", pilot.why);
      return false;
    }
  }
  return touched;
}

void LandAhead(const Params& P, const char* name) {
  std::printf("%s: land straight ahead from 120 m up at 70 m/s\n", name);
  Sim s(P);
  s.flying = true;
  s.st.speed = 70;
  s.st.q = Facing(0.3f);
  s.pos = {0, 0, kGround + kRest + 120};
  Pilot pilot;
  Situation sit;
  sit.pos = s.pos;
  sit.radar = s.Radar();
  const Attitude a = Look(s.st.q);
  Runway r;
  const float out = std::max(700.0f, sit.radar / kGlide);
  r.aim = s.pos + V3{a.f.x, a.f.y, 0} * (out * (1.0f / std::sqrt(a.f.x * a.f.x + a.f.y * a.f.y)));
  r.aim.z = kGround;
  r.yaw = a.yaw;
  Engage(pilot, Mode::kLand, a, sit, &r);
  float sink = 0, speed = 0;
  V3 at;
  const bool ok = Autoland(P, s, pilot, 300, sink, speed, at);
  const float miss = Len(V3{at.x - r.aim.x, at.y - r.aim.y, 0});
  std::printf("  touched down %s: %.0f m from the aim, sinking %.2f m/s at %.1f m/s; stopped after %.0f s, %.0f m on\n",
              ok ? "yes" : "no", miss, sink, speed, s.t, Len(V3{s.pos.x - at.x, s.pos.y - at.y, 0}));
  Check(ok && sink < 2.5f && miss < 400, "lands gently near the aim");
}

void Route(const Params& P, const char* name, float start_x, float start_y, float start_yaw) {
  std::printf("%s: fly to a runway and land, from (%.0f, %.0f) heading %.0f\n", name, start_x, start_y,
              start_yaw * 57.3f);
  Sim s(P);
  s.flying = true;
  s.st.speed = 72;
  s.st.q = Facing(start_yaw);
  s.pos = {start_x, start_y, 250};
  Runway r;
  r.aim = {2392.4f, -62.0f, kGround};
  r.yaw = std::atan2(-(2607.4f - 2392.4f), (218.6f - -62.0f));
  Pilot pilot;
  Situation sit;
  sit.pos = s.pos;
  sit.radar = s.Radar();
  Engage(pilot, Mode::kRoute, Look(s.st.q), sit, &r);
  float sink = 0, speed = 0;
  V3 at;
  const bool ok = Autoland(P, s, pilot, 600, sink, speed, at);
  const float along = AlongTrack(r, at), cross = CrossTrack(r, at);
  std::printf("  touched down %s after %.0f s: %.0f m %s the aim, %.1f m %s of the line, sinking %.2f m/s at %.1f m/s\n",
              ok ? "yes" : "no", s.t, std::fabs(along), along > 0 ? "short of" : "past", std::fabs(cross),
              cross > 0 ? "right" : "left", sink, speed);
  Check(ok && sink < 2.5f && std::fabs(cross) < 15 && along > -400 && along < 250, "lands on the runway");
}
}  // namespace

int main() {
  for (int k = 0; k < 2; ++k) {
    const Params& P = k ? kBatwing : kF16;
    const char* name = k ? "Batwing" : "F-16";
    TopSpeeds(P, name);
    Takeoff(P, name);
    Turn(P, name);
    Stall(P, name);
    LandAhead(P, name);
    Route(P, name, 0, 0, 0);
    Route(P, name, 4000, 2500, 2.5f);
    Route(P, name, 2600, -1500, -1.0f);
  }
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
