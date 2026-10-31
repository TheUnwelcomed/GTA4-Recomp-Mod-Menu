# Batmobile (JulioNIB) on the PS5 port — as an add-on car

Installed as a **new vehicle** named `batmobile`: every stock car, the FBI Buffalo
included, stays exactly as it is. The trainer's Car Spawning page has a
**"Batmobile (add-on)"** entry that spawns it once installed — until then the entry
answers "not installed" and does nothing.

The original mod has two halves:

| Part | On this port |
|---|---|
| The car (`batmobile.wft` / `batmobile.wtd` + two text lines) | Installable (model files experimental, below) |
| The gadget script (ScriptHookDotNet `.net.dll`s) | **Cannot run** — Windows x86 .NET plugins. The trainer re-creates the essentials natively instead: with *Vehicle Options → Batmobile Gadgets* on, while driving the Batmobile, **d-pad left** fires the front guns (explosive volley ahead) and **holding d-pad right** is the afterburner. |

## 1. Register the vehicle (text edits — these definitely work)

The port reads these as loose files; edit them over FTP, after backing both up:

```
/data/homebrew/PPSA94004/game/common/data/handling.dat
/data/homebrew/PPSA94004/game/common/data/vehicles.ide
```

- **Append** the line in `handling-batmobile.txt` to `handling.dat`, near the other
  handling lines (do not touch the existing `FBI` line).
- **Append** the line in `vehicles-ide-batmobile.txt` to `vehicles.ide` inside the
  `cars` section, beside the other car lines.

Spawn frequency in that line is 10 with no special flags, so ambient traffic won't
be full of Batmobiles; the trainer spawns it on demand.

## 2. The model files (experimental)

`batmobile.wft` / `batmobile.wtd` must be **added** (not replacing anything) to the
vehicles image:

1. FTP `game/xbox360/models/cdimages/vehicles.img` (if there is no `xbox360`
   folder, look under `game/pc/models/cdimages/`) to your computer. **Keep an
   untouched copy.**
2. On PC, open it with SparkIV or OpenIV, **add** `batmobile.wft` and
   `batmobile.wtd` from this folder, rebuild.
3. FTP it back and launch. Spawn it from the trainer.

**The honest caveat:** these two files are PC-format RAGE resources (`RSC\x05`,
little-endian; the .wft is a 16 MB fragment). This port runs the Xbox 360 build,
whose resource layout is big-endian, so the engine may refuse them — an invisible
car or a crash at spawn means exactly that. Because this is an add-on, recovery is
trivial: restore the backup `vehicles.img` (or just never press the spawn entry).
Everything else — the registration lines, the trainer entry, the gadgets — stays
harmless either way.

## Why there's no PC→360 converter in this folder

Converting means re-serializing the whole model graph big-endian and re-encoding
vertex, index and texture streams; no public tool does it for IV, and a hand-written
one can't be validated without crash-testing on a console. Two real routes, both
iterative projects: (1) a converter developed against your console with a
spawn → log → fix loop, or (2) teaching the port's loaders to accept PC-endian
resources — the cleaner fix (the port already runs on x86), which would open the
whole PC car-mod catalog, not just this one.
