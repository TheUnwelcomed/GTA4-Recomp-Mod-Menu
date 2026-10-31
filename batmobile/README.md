# Batmobile (JulioNIB) on the PS5 port

The Batmobile is **converted to the Xbox 360 format** the port runs, and the app
installs it by itself: copy the release's `PPSA94004` folder over yours and
launch. The trainer's **Car Spawning → Batmobile** entry spawns it.

## How it gets there

1. The PS5 port runs the 360 build of GTA IV, which streams `.xft`/`.xtd` model
   files. The mod ships PC `.wft`/`.wtd` files, so earlier versions that
   installed the PC files spawned a stock car: the game never looked them up.
2. `tools/pc2x360` converts the PC Batmobile into the 360 format by
   transplanting it into the stock 360 FBI car from your own console (exported
   by the trainer build to `logs/export/`). Every format rule was verified
   against the stock FBI, which exists on both platforms — see that folder's
   README for the results.
3. On launch the app's add-on installer (`src/gta4_addon_install.cpp`) puts the
   converted `fbi.xft`/`fbi.xtd` into `vehicles.img` (decrypting and
   re-encrypting its table with the game's own key) and applies the
   Batmobile's handling line to the FBI slot. First launch rewrites the archive
   once; later launches change nothing.

The converted files are bundled in the release zip only (they contain data
from the game, so they are not committed here).

## What you get / known limits

| | |
|---|---|
| Body, all LODs, wheels, textures | the Batmobile's |
| Handling | the Batmobile mod's (heavy, 9,550 kg) |
| Collision | the FBI Buffalo's (it collides like a Buffalo-sized car) |
| Parts the Batmobile has no mesh for (rear doors, sirens) | invisible |
| Gadget script (ScriptHookDotNet `.net.dll`s) | cannot run on the port — the trainer re-creates the essentials: *Vehicle Options → Batmobile Gadgets*, then in the car **d-pad left** fires the front guns, **hold d-pad right** for the afterburner |
| FBI cars in 4-star+ chases | are Batmobiles too (it takes the FBI slot) |

The model is about 7× the polygons of a stock car. If the engine refuses to
stream it, the trainer says "The model did not load." — nothing crashes.

## Undo

Next to `vehicles.img`, `handling.dat` and `vehicles.ide` the installer leaves
`.stock-bak` copies made before its first change. Put those back (and remove
`addons/batmobile` from the app folder) for the stock FBI.

## Files here

| | |
|---|---|
| `batmobile.wft` / `batmobile.wtd` | the mod's PC model and textures (the converter's input, as `fbi.wft`/`fbi.wtd`) |
| `handling_fbi.txt` / `vehicles_ide_fbi.txt` | the mod's lines for the FBI slot (bundled with the app) |
| `restore_fbi.txt` | the stock FBI lines |
