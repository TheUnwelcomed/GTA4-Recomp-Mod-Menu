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
2. `tools/pc2x360` converts the PC Batmobile (`pc/`) into the 360 format. It
   builds on the stock 360 FBI car exported from a console. Every format rule
   was checked against the stock FBI, which exists on both platforms.
3. The app's add-on installer adds it as a new vehicle (see `../README.md`).

## What you get / known limits

| | |
|---|---|
| Body, all LODs, wheels, textures | the Batmobile's |
| Handling | the mod's (heavy, 9,550 kg) |
| Collision | the FBI Buffalo's: it collides like a Buffalo-sized car |
| Parts it has no mesh for (rear doors, sirens) | not drawn |
| Gadget script (ScriptHookDotNet `.net.dll`s) | cannot run on the port. The trainer re-creates the essentials: *Vehicle Options → Batmobile Gadgets*, then in the car **d-pad left** fires the front guns and **holding d-pad right** is the afterburner. Those buttons don't change the radio while you're in it |

The model is about 7× the polygons of a stock car. If the engine refuses to
stream it, the trainer says "The model did not load." Nothing crashes.

## Files here

| | |
|---|---|
| `addon.txt`, `handling.txt`, `vehicles_ide.txt` | what the installer registers |
| `pc/batmobile.wft`, `pc/batmobile.wtd` | the mod's PC model and textures (the converter's input) |
