# Bundled add-on vehicles

Each folder here is one car the app installs by itself at launch, as **its own
vehicle** next to the stock ones. No stock car is replaced. Spawn them from the
trainer's **Car Spawning** list.

| folder | car | in the trainer |
|---|---|---|
| `batmobile/` | Batmobile (JulioNIB) | Batmobile, with *Batmobile Gadgets* |
| `f16/` | F-16C Fighting Falcon (SkylineGTRFreak) | F-16 Fighter Jet: flies, cannon, missiles |
| `deimos/` | Grotti Deimos GTS (harithd, ported by Milcin) | Grotti Deimos GTS: pop-up lamps on d-pad left |
| `dom645t/` | Vapid Dominator 645 (johndoe968, Rodger Dodger, ported by Milcin) | Vapid Dominator 645 |

Add-on cars are kept out of traffic: nobody else drives them, they only
appear when you spawn them.

The converted model files (`<model>.xft` / `<model>.xtd`) are in the release
zip only, because they contain data from the game. `tools/pc2x360` makes them
from the mods' PC files.

## What the installer does (`src/gta4_addon_install.cpp`)

For every `addons/<folder>/addon.txt` in the app:

1. **vehicles.ide**: puts `vehicles_ide.txt` into the `cars` section, right
   after the donor car's line, rebuilt in the console's format on the
   template car's line, with the traffic frequency set to 0 (unless
   `traffic=yes`).
2. **handling.dat**: puts `handling.txt` right after the donor's line.
3. **carcols.dat**: adds the car's own row from `carcols.txt` to that
   section, or else copies the colour rows of the car named by `colours=`
   (default: the donor).
4. **vehOff.csv**: copies the donor's camera-offset row.
5. **vehicles.img**: appends `<model>.xft` / `<model>.xtd` to every
   `vehicles.img` in the install. The archive's table is decrypted and
   re-encrypted with the game's own key.

The same lines go into the title update and episode copies when those
exist. Each file gets a `.stock-bak` copy before its first change, and a
second launch changes nothing. The installer also puts back the stock FBI
lines that releases up to v1.6 had overwritten (the Batmobile used to take
the FBI's slot).

## addon.txt

```
model=f16        # the model name (lower case); <model>.xft / .xtd sit beside it
donor=fbi        # stock car whose lines it goes next to and whose rows it borrows
template=banshee # optional: the stock car it was converted on (default: the donor)
colours=airtug   # optional: copy this car's colour rows instead of the donor's
traffic=yes      # optional: let it appear in traffic (default: never)
```

`vehicles_ide.txt` must start with the model name. Flags the console game
does not know (ZolikaPatch's `wheelangle:` or `livery`, say) are left out.

The fifth field, the game name, is also what IV picks the engine sound by:
an add-on with a name of its own is silent, so the Batmobile drives with the
Sabre GT's engine and the F-16 with the Infernus's (their names show when you
get in). The Deimos and Dominator keep the names their mods chose (Turismo,
Banshee).
