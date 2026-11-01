// Flies gta4_jet_flight.h on the PC: takeoff, top speeds, Standard and
// Expert turns, a stall, the autopilot's hold, a landing straight ahead and
// a route to a runway from 5 km out. Prints what happened; fails on nonsense.
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
      Check(s.st.speed > 28 && s.st.speed < 45, "idle settles at 28-45 m/s");
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
  Check(s.t > 3 && s.t < 8, "the roll to rotate speed takes 3-8 s");
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
  std::printf("%s: Standard controls, the stick 65%% left for 20 s, then let go\n", name);
  {
    Sim s(P);
    s.flying = true;
    s.st.speed = 70;
    s.pos = {0, 0, 300};
    Input in;
    in.assist = true;
    in.gear_down = false;
    in.throttle = 0.8f;
    float yaw0 = Look(s.st.q).yaw, max_bank = 0;
    for (int i = 0; i < 60 * 20; ++i) {
      in.roll = -0.65f;
      s.Step(in);
      max_bank = std::max(max_bank, std::fabs(Look(s.st.q).bank));
    }
    const Attitude a = Look(s.st.q);
    const float turned = std::fabs(Wrap(a.yaw - yaw0)) * 57.3f;
    std::printf("  after 20 s: bank %.0f, height %.0f (from 300), %.1f m/s, turned %.0f degrees\n", a.bank * 57.3f,
                s.pos.z, s.st.speed, turned);
    Check(std::fabs(s.pos.z - 300) < 40, "a Standard turn holds its height");
    Check(std::fabs(a.bank) > 0.6f && std::fabs(a.bank) < 0.95f, "the stick 65% over banks it 35-55 degrees");
    in.roll = 0;
    float t_level = -1;
    for (int i = 0; i < 60 * 4; ++i) {
      s.Step(in);
      if (t_level < 0 && std::fabs(Look(s.st.q).bank) < 0.05f)
        t_level = i / 60.0f;
    }
    std::printf("  let go: wings level after %.1f s\n", t_level);
    Check(t_level >= 0 && t_level < 2.0f, "let go, the wings level within 2 s");
  }
  std::printf("%s: Expert, full roll for 0.5 s, full pull for 2 s\n", name);
  {
    Sim s(P);
    s.flying = true;
    s.st.speed = 70;
    s.pos = {0, 0, 300};
    Input in;
    in.gear_down = false;
    in.throttle = 0.8f;
    for (int i = 0; i < 30; ++i) {
      in.roll = 1;
      s.Step(in);
    }
    const float bank = Look(s.st.q).bank * 57.3f;
    in.roll = 0;
    const float gamma0 = Look(s.st.q).gamma;
    in.pitch = 1;
    for (int i = 0; i < 120; ++i)
      s.Step(in);
    std::printf("  rolled %.0f degrees in 0.5 s; then pulled 2 s: %.1f m/s\n", bank, s.st.speed);
    Check(std::fabs(bank) > 45, "it rolls 45+ degrees in half a second");
    (void)gamma0;
  }
  std::printf("%s: Expert, a loop on full pull from 80 m/s with the afterburner\n", name);
  {
    Sim s(P);
    s.flying = true;
    s.st.speed = 80;
    s.pos = {0, 0, 600};
    Input in;
    in.gear_down = false;
    in.throttle = 1;
    in.burner = true;
    in.pitch = 1;
    float turned = 0, last = Look(s.st.q).gamma;
    bool inverted = false;
    for (int i = 0; i < 60 * 12 && !(inverted && Look(s.st.q).u.z > 0.9f && Look(s.st.q).gamma > -0.05f); ++i) {
      s.Step(in);
      inverted |= Look(s.st.q).u.z < -0.5f;
      turned += std::fabs(Look(s.st.q).gamma - last);
      last = Look(s.st.q).gamma;
    }
    std::printf("  over the top: %s, speed %.1f m/s, height %.0f\n", inverted ? "yes" : "no", s.st.speed, s.pos.z);
    Check(inverted, "it loops");
  }
}

void Stall(const Params& P, const char* name) {
  std::printf("%s: nose held 50 degrees up at idle until it stalls\n", name);
  Sim s(P);
  s.flying = true;
  s.st.speed = 60;
  s.pos = {0, 0, 600};
  Input in;
  in.assist = true;
  in.gear_down = false;
  in.throttle = 0;
  float min_speed = 99, lowest_gamma = 0, top_gamma = 0;
  bool stalled = false;
  float stalled_at = -1;
  for (int i = 0; i < 60 * 25; ++i) {
    const float g = Look(s.st.q).gamma;
    // The pilot pulls for 50 degrees; 3 s into the stall he lets go and adds power.
    const bool recover = stalled_at >= 0 && s.t - stalled_at > 3.0f;
    in.pitch = recover ? 0.0f : Clamp(3.0f * (0.87f - g), -1.0f, 1.0f);
    in.throttle = recover ? 1.0f : 0.0f;
    if (stalled_at < 0 && s.st.stalled)
      stalled_at = s.t;
    s.Step(in);
    min_speed = std::min(min_speed, s.st.speed);
    stalled |= s.st.stalled;
    top_gamma = std::max(top_gamma, Look(s.st.q).gamma);
    if (stalled)
      lowest_gamma = std::min(lowest_gamma, Look(s.st.q).gamma);
    if (!std::isfinite(s.st.speed) || !std::isfinite(s.pos.z)) {
      Check(false, "numbers stay finite");
      return;
    }
  }
  std::printf("  nose up to %.0f, slowest %.1f m/s, stalled %s, then the nose fell to %.0f degrees; %.1f m/s now\n",
              top_gamma * 57.3f, min_speed, stalled ? "yes" : "no", lowest_gamma * 57.3f, s.st.speed);
  Check(stalled && lowest_gamma < 0.0f, "a stall drops the nose, pulling or not");
  Check(s.st.speed > StallSpeed(P, false), "let go, with power, it flies again");
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
