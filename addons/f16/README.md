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
| **RT** | throttle: on the ground it drives, in the air it speeds up; full throttle at speed lights the afterburner |
| **LT** | air brake |
| **left stick back** at speed (~95 km/h) | take off |
| left stick | pitch and roll. A banked jet turns |
| **R3** | landing gear up / down (in the air; it stays down on the ground and comes down by itself below flying speed) |
| **d-pad right** (hold) | cannon |
| **d-pad left** | missile, from the wing pylons in turn (up to 6 in the air at once) |
| slow below ~60 km/h | it stops flying and glides down; land it |

While you're in the jet, R3 doesn't swap the camera, and with the weapons on
the d-pad doesn't change the radio. The gear, the engine sound and the exhaust
work with the flight toggle off too.

## How it works

- **Flight**: once airborne, the trainer sets the jet's attitude and its speed
  along the nose every frame through the game's own natives
  (`SET_VEHICLE_QUATERNION`, `SET_CAR_FORWARD_SPEED`). Physics still handles
  crashes.
- **Cannon**: real bullets (`FIRE_SINGLE_BULLET`) from the gun port the
  mod's `ac-iv.ini` gives (0.74, 3.29, 0.55), 900 m out, with IV's machine-gun
  muzzle flash (`muz_machine`) and bullet impacts (`imp_bullet_default`) where
  the burst meets the ground.
- **Missiles**: the RPG's rocket (`cj_rpg_rocket`) with its smoke trail
  (`weap_rocket`), launched from the pylons at (±2.5, -3.0, -0.3), exploding on
  the ground, a roof or within 5 m of a vehicle, or after 1.5 km.
- **Engine sound**: IV has no jet engines (the airport's planes are ambient
  sound effects), so the engine is synthesized (`src/gta4_jet_engine.h`) and
  played through the port's own audio mixer: turbine whine whose pitch follows
  the spool, jet roar, afterburner rumble and crackle. It spools up when you
  get in and winds down when you get out. The car engine sound IV would give
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
