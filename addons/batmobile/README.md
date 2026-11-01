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
| Its doors, canopy, bonnet, boot | fixed to the body (the mod parks those bones 13 m under the car; v1.7.4 drew the parts there, the long black shapes) |
| Engine sound | the Sabre GT's (IV picks sounds by the name a car shows) |
| Gadget script (ScriptHookDotNet `.net.dll`s) | cannot run on the port. The trainer re-creates its minigun, grenades and booster: see below |
| Physics wheels | the mod's own sizes (0.35 m front, 0.39 m rear); on the FBI's 0.40 m wheels it sat high |
| Body shader | the template's `interior2`, as the mod's `interior` maps to: v1.7.5-v1.7.6 borrowed the police car's `interior` and the body came out chrome |

## Gadgets

*Vehicle Options → Batmobile Gadgets*, then in the car:

| | |
|---|---|
| **d-pad left** (hold) | minigun ahead, from both front barrels in turn |
| **d-pad down** (hold) | minigun behind |
| **horn (L3)** | drop three grenades behind; press again to set them off (they go off by themselves after 10 s) |
| **d-pad right** (hold) | booster |

As in JulioNIB's script (its strings: *Minigun*, *Releasing grenades, press
again to detonate!*, the `w_grenade` model), redone through natives: real
bullets (`FIRE_SINGLE_BULLET`) with IV's machine-gun flash (`muz_machine`;
the script's `muz_minigun` is an episodes effect), grenade explosions
(`ADD_EXPLOSION`), the shots' sound from the trainer's own mixer. Those buttons
don't change the radio or sound the horn while gadgets are on; d-pad up stays
the phone.

The model is about 7× the polygons of a stock car. If the engine refuses to
stream it, the trainer says "The model did not load." Nothing crashes.

## Files here

| | |
|---|---|
| `addon.txt`, `handling.txt`, `vehicles_ide.txt` | what the installer registers |
| `pc/batmobile.wft`, `pc/batmobile.wtd` | the mod's PC model and textures (the converter's input) |
