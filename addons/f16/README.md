# F-16C Fighting Falcon (SkylineGTRFreak) on the PS5 port

On PC this jet replaces the airport tug and flies through JulioNIB's **Air
Combat IV** script, a .NET plugin that cannot run on the port. Here it's its own
vehicle (`f16`, the tug is untouched), and the trainer flies it.

## Flying it

Spawn **Car Spawning → F-16 Fighter Jet** (it appears 16 m ahead; it's big),
and switch on *Vehicle Options → F-16 Flight and Weapons* (every option starts
off).

| | |
|---|---|
| **Triangle** | getting in: the canopy rises, you're put in the seat, it closes. Inside, on the ground at a standstill: it opens and sets you down beside the cockpit (rolling or flying, Y is the game's own exit) |
| **RT** | throttle: on the ground it drives; in the air the speed comes from the engine (idle settles near 90 km/h, full power near 250, RT all the way lights the afterburner, near 325) |
| **LT** | air brake |
| **left stick back** at speed (~95 km/h) | take off |
| left stick | pitch and roll; the turn rates build up and ease off and grow with speed. A banked jet turns, and sinks gently unless you pull |
| **LB / RB** (in the air) | rudder |
| **R3** | landing gear up / down (in the air; it stays down on the ground and comes down by itself below flying speed) |
| **d-pad right** (hold) | cannon, both sides |
| **d-pad left** | missile, from the wing pylons in turn (up to 6 in the air at once) |
| a car ahead | locks on after 0.6 s within 12° of the nose (two beeps, a red radar blip, a red light): missiles home on it, the cannon aims at it |
| too slow | the nose drops (a stall) until the dive brings the speed back |
| back near the ground, not diving | it touches down (gear up: on its belly) |

While you're in the jet, R3 doesn't swap the camera, and with the weapons on
the d-pad doesn't change the radio. The gear, the engine sound and the exhaust
work with the flight toggle off too.

## How it works

- **Flight**: once airborne, the trainer sets the jet's attitude and its speed
  along the nose every frame through the game's own natives
  (`SET_VEHICLE_QUATERNION`, `SET_CAR_FORWARD_SPEED`). Physics still handles
  crashes. The model is GTA V's jets' feel rather than an aerodynamic one:
  thrust (idle 1.3, full dry 10.3, afterburner +6.7 m/s^2) against drag
  (0.0021 v^2) gives the speed, with gravity along the climb; stick and
  rudder set turn rates that ease in over ~0.2 s, scaled by airspeed (full
  from 45 m/s); a bank turns the jet; lift holds the flight path while wings
  are level and the jet is above 32 m/s, so hands off it keeps its attitude,
  while slower, banked or inverted the path bends down; below 16 m/s the nose
  falls through. `tools/jet/flight_model_sim.py` runs the same equations
  through a few manoeuvres.
- **Canopy**: the converter hangs the mod's canopy (its `windscreen` bone's
  bubble) on the Cavalcade template's `interiorlight`, hinged at its rear
  edge (0, 2.3, 1.22); the trainer swings it 38° (the front rising, as the
  real jet's does) and moves the player in and out, as the Batmobile's.
- **Lock-on**: every 0.2 s the trainer looks for the nearest working car
  along the nose (`GET_CLOSEST_CAR` at points out to 350 m, kept within 12°);
  held there 0.6 s it locks (`ADD_BLIP_FOR_CAR`, red). A missile fired while
  locked turns towards it at up to 3 rad/s, its nose following
  (`SET_OBJECT_QUATERNION`); the cannon aims both guns at it while it is
  within 15° of the nose.
- **Cannon**: real bullets (`FIRE_SINGLE_BULLET`) from the gun port the
  mod's `ac-iv.ini` gives (0.74, 3.29, 0.55) and its mirror on the other side
  (the real jet has one gun, on the left; both sides here, as asked), 900 m
  out, each with IV's machine-gun muzzle flash (`muz_machine`), and bullet
  impacts (`imp_bullet_default`) where the burst meets the ground.
- **Missiles**: the RPG's rocket (`cj_rpg_rocket`) with its smoke trail
  (`weap_rocket`), launched from the pylons at (±2.5, -3.0, -0.3), exploding on
  the ground, a roof or within 5 m of a vehicle, or after 1.5 km.
- **Engine sound**: IV has no jet engines (the airport's planes are ambient
  sound effects), so the engine is synthesized (`src/gta4_jet_engine.h`) and
  played through the port's own audio mixer: turbine whine whose pitch follows
  the spool, jet roar, afterburner rumble and crackle. It spools up when you
  get in and winds down when you get out, at 40% of the level v1.7.10 had
  (the guns and missiles keep theirs). The car engine sound IV would give
  it is gone: its game name (`F16`) has no engine of its own.
  `tools/jet/render_preview.cpp` renders it to a file.
- **Exhaust**: IV's rocket flame (`weap_rocket`) at the nozzle, small at high
  throttle, large on afterburner.
- **Landing gear**: on PC the script swings the mod's gear as car doors
  (`boot` carries the nose and left main gear, `door_pside_f` the right main
  gear; `gears_open_angle=120`, `bHideGears=1`). Here the converter puts each
  leg on its own spare bone of the template, at its hinge: the nose gear (the
  part of `boot` ahead of y = 1.5 m) on `extra_1` at (0, 2.73, -0.22), the
  left and right main gears on `extra_5` and `extra_6` at their door lines. The
  trainer folds the nose gear aft about the width (95°) and the mains inwards
  about the length (95°), over 2.6 s, then hides them (the fuselage has no
  wheel wells), as the PC mod does. The legs fold sideways because their doors
  are long panels along the fuselage: swung about the width they stood up
  through the wing.

*Trainer Status* lists any native the game did not provide; a feature that
needs it then does nothing instead of crashing. Each missile launch and hit is
logged (`gta4-trainer: F-16 missile ...`).

## Conversion

```
run_vehicle.py <export> <work> f16.wft f16.wtd out f16 cavalcade \
  "extra_1@0,2.73,-0.22=boot:y>1.5;extra_5@-0.658,-0.073,-0.028=boot:y<=1.5;extra_6@0.651,-0.073,-0.027=door_pside_f" airtug,fbi
```

| | |
|---|---|
| Body, cockpit, 3 LODs, textures | the jet's (the two uncompressed textures were compressed to DXT5, at 38 and 55 dB PSNR) |
| Template | the stock **Cavalcade**: its three spare extras on the chassis carry the gear (the FBI has none) |
| Paint | the airport tug's paint shaders (`airtug` above), which carry the liveries (`car_sign_1`-`4`); colours from the tug's colour rows, as on PC |
| Collision and physics | the Cavalcade's: wings and tail pass through things |
| Wheels | physics wheels at the jet's gear with the mod's own radii (nose 0.26 m, mains 0.47 m; the nose wheel fills both front slots); the visible gear is part of the body |
| Flaps, tailplanes (`door_dside_r`, `bonnet`, `door_pside_r`) | fixed in place |
