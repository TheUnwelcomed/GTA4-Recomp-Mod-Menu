# Batmobile (JulioNIB) on the PS5 port

The Batmobile is its own vehicle (`batmobile`) and comes converted to the
Xbox 360 format the port runs. Copy the release's `PPSA94004` folder over yours
and launch, then pick **Car Spawning → Batmobile**. FBI cars stay FBI cars:
releases up to v1.6 put it in the FBI's slot, and the installer now puts the
stock FBI lines back by itself.

## How it got here

1. The PS5 port runs the 360 build of GTA IV, which streams `.xft`/`.xtd`
   models. The mod ships PC `.wft`/`.wtd` files. The game never looks those
   up, so the first attempts spawned a stock car.
2. `tools/pc2x360` converts the PC Batmobile (the mod's `batmobile.wft` /
   `.wtd`, not kept here) into the 360 format. It builds on the stock 360 FBI
   car exported from a console. Every format rule was checked against the
   stock FBI, which exists on both platforms.
3. The app's add-on installer adds it as a new vehicle (see `../README.md`).

## What you get / known limits

| | |
|---|---|
| Body, all LODs, wheels, textures | the Batmobile's |
| Handling | the mod's (heavy, 9,550 kg) |
| Collision | the FBI Buffalo's: it collides like a Buffalo-sized car |
| Parts it has no mesh for (rear doors, sirens) | not drawn |
| Canopy | pops open when you get in or out (see below); it rides the FBI's `interiorlight` bone, hinged at its rear edge |
| Its doors, bonnet, boot | fixed to the body (the mod parks its door bones 13 m under the car; v1.7.4 drew the parts there, the long black shapes) |
| Engine sound | its own, synthesized (`src/gta4_car_engine.h`; see below). Its game name is `BATMOBILE`, which has no engine sound in IV; up to v1.7.10 it was `SABREGT` and drove with the Sabre GT's |
| Gadget script (ScriptHookDotNet `.net.dll`s) | cannot run on the port. The trainer re-creates its missiles, miniguns, bombs and booster: see below |
| Physics wheels | the mod's own sizes (0.35 m front, 0.39 m rear); on the FBI's 0.40 m wheels it sat high |
| Body shader | the template's `interior2`, as the mod's `interior` maps to: v1.7.5-v1.7.6 borrowed the police car's `interior` and the body came out chrome |

## Canopy

No toggle. Getting in (Triangle), the canopy pops open, you're put in the
seat and it closes. Triangle inside opens it again and sets you down beside
the driver's side. The trainer turns the canopy's bone in the car's live
skeleton, as it turns the Deimos's lamp pods, and moves the player with
`WARP_CHAR_INTO_CAR` / `WARP_CHAR_FROM_CAR_TO_COORD`. The cockpit under the
canopy (seats, dash, wheel) is the mod's own.

Getting out (since v1.7.12, at a standstill; rolling, Triangle is the
game's own exit): the canopy opens, the game's own exit takes the player out
(`CLEAR_CHAR_TASKS`, `TASK_LEAVE_CAR_IMMEDIATELY`, as Rockstar's
`brucie_heli` gets the player out of a car), hidden while he climbs out
(`SET_CHAR_VISIBLE`); on his feet he is put on the ground beside the cockpit
(`SET_CHAR_COORDINATES`, the ground found from above the car's roof) and
shown, and the canopy closes. v1.7.8-v1.7.11 warped him out of the seat
(`WARP_CHAR_FROM_CAR_TO_COORD`) and he dropped through the world. For four
seconds the trainer still watches: dropping, the player is held on the spot
and let go a moment later, up to four times, then put back in the seat.

## Engine sound

An original voice in the spirit of the 1989/1992 film car, synthesized from
scratch (nothing from the films): a big, slow-revving cross-plane V8's firing
pulses (rpm / 15 Hz; 650 to 5,200 rpm, a lopey idle) through a deep exhaust
resonance, with the firing rate's own tone for weight and a growl that grows
with load, kept dark (nothing much above ~1.3 kHz), a soft turbine under it
(420 Hz to ~1.3 kHz), a low intake rasp, pops on lift-off, and a deep roar
with crackles on the booster, at 35% level. v1.7.11-v1.7.13 revved to 6,500
with a bright exhaust and a turbine whining to 3.3 kHz, at full level: shrill
and much louder than the game's cars. The trainer works the revs out of the road
speed through five simulated gears (the throttle revs it standing still);
getting in cranks the starter and it catches; getting out it winds down. The
canopy's hiss and clunk come from the same voice (the F-16's canopy too).
`tools/car/render_preview.cpp batmobile` renders a scripted drive to a WAV.

## Gadgets

*Vehicle Options → Batmobile Gadgets*, then in the car:

| | |
|---|---|
| **L1** | a missile out of the round intake in the nose, locking on the car ahead (3 s reload) |
| **d-pad left** (hold) | the front miniguns, both barrels together |
| **d-pad down** (hold) | the rear miniguns, both barrels together |
| **horn (L3)** | plant a bomb behind the car (a blinking red light marks it); again to set it off |
| **d-pad right** (hold) | booster: the rear turbine flames (`weap_rocket`) and roars (the engine's own booster roar) |

As in JulioNIB's script (its modes *Minigun*, *Rocket*, *Bomb*), redone
through natives:

- **Missile:** the RPG's rocket (`cj_rpg_rocket`) with its smoke trail,
  flown as a physics object the way Rockstar's own scripted rockets fly
  (TBOGT's `tony8`). Its velocity is set every frame, and
  `HAS_OBJECT_COLLIDED_WITH_ANYTHING` sets it off on walls and cars. The
  ground and nearby cars are checked too, and it goes off at 320 m. If the
  physics doesn't take it, it is flown by hand. The log says which.
- **Miniguns:** real bullets (`FIRE_SINGLE_BULLET`) with IV's machine-gun
  flash (`muz_machine`; the script's `muz_minigun` is an episodes effect) on
  each barrel.
- **Bomb:** the `w_grenade` model on the ground with a
  `DRAW_LIGHT_WITH_RANGE` blink, set off as three rocket explosions
  (`ADD_EXPLOSION` type 2). v1.7.7's grenades used type 0 and only made a
  noise.

Those buttons don't drive-by, change the radio or sound the horn while
gadgets are on. D-pad up stays the phone. Missiles in flight and a planted
bomb carry on after you leave the car.

The model is about 7× the polygons of a stock car. Since v1.7.8 its GPU data
loads in 256 KB blocks like a stock car's; v1.7.7's 2 MB blocks could take
17 s to find room for. If the engine still refuses to stream it, the
trainer says so after 60 s. Nothing crashes.

## Files here

| | |
|---|---|
| `addon.txt`, `handling.txt`, `vehicles_ide.txt` | what the installer registers |
| `pc/batmobile.wft`, `pc/batmobile.wtd` | the mod's PC model and textures (the converter's input) |
