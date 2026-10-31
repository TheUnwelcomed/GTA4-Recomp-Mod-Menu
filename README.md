# Liberty Trainer — a mod menu for GTA IV on PS5 (LibertyRecomp)

A single-player trainer menu built **into** elripalda's
[gta4-ps5](https://github.com/elripalda/gta4-ps5) port (LibertyRecomp), modeled on
sjaak327's **Simple Native Trainer** — the most-used GTA IV trainer — with the same
approach under the hood: every action goes through the game's own script natives.

Nothing here touches your game files. The menu is compiled into the app itself, so it
survives reinstalls of the game data and works in GTA IV, TLAD and TBoGT alike.

## Controls

| | |
|---|---|
| **L3 + R3** | open / close the menu |
| D-pad up / down | move (hold to repeat) |
| D-pad left / right | change a value (wanted level, hour, weather) |
| A (Cross) | select |
| B (Circle) | back / close |

While the menu is open the game sees no controller input (the same rule the system
dialogs use), so you won't punch anyone while navigating.

## Features (Simple Native Trainer's core set)

- **Player** — God mode, Never Wanted, set wanted level 0–6, +$100,000, heal & armor
- **Vehicle**: vehicle god mode, repair, warp-into-spawned-car toggle, Batmobile
  gadgets, F-16 flight and weapons
- **Car Spawning**: 29 stock cars, bikes and helicopters plus three bundled
  **add-on vehicles**, spawned in front of you. Episode-only vehicles are
  detected and refused outside their episode
- **Add-on vehicles**, converted to the 360 format and installed by the app
  itself as their own vehicles (no stock car replaced; see `addons/`):
  - **Batmobile** (JulioNIB), with its gadgets re-created
  - **F-16C Fighting Falcon** (SkylineGTRFreak): flies, with a nose cannon and
    missiles (the PC mod's Air Combat IV script, re-done in the trainer)
  - **Grotti Deimos GTS** (harithd / Milcin), with pop-up headlights switched
    by the trainer
- **Teleporting** — to the map waypoint, save/restore a position, landmark presets
- **Time & Weather** — set the clock, force or release any weather
- **Weapons** — full loadout with ammo, or strip all weapons
- **Chaos**: raining cars (random cars dropped around you), gravity on/off
- **Trainer Status** — shows which game natives resolved, for debugging

The menu is **single-player only**: it closes itself and refuses to act inside a
network session.

## How it works

- The game registers its script natives at boot; the port already hooks that
  registration (`sub_82845600`), and the trainer captures the handlers it needs
  (`GET_PLAYER_CHAR`, `CREATE_CAR`, `SET_CHAR_COORDINATES`, …).
- Once per frame, from the port's input-poll hook (`sub_828CCD60`) on the game's own
  thread, the trainer reads the pad, runs the menu, and calls natives using the
  game's real native-call context layout — the same technique the port's Quicksave
  feature uses, and the same natives Simple Native Trainer called on PC.
- The overlay is drawn with the port's ImGui layer, beside the FPS counter.
- A `gta4_trainer` cvar (on by default) gates the whole thing.

## Files

| | |
|---|---|
| `src/gta4_trainer.h` / `src/gta4_trainer.cpp` | the whole trainer |
| `src/gta4_addon_install.h` / `.cpp` | installs the bundled add-on vehicles into the game at launch |
| `patches/liberty-trainer.patch` | the five small wiring edits to the port |
| `addons/` | the add-on vehicles: what each registers, notes and limits |
| `tools/pc2x360/` | PC → Xbox 360 vehicle converter, verified against the stock FBI |

## Building

Apply this on top of `elripalda/gta4-ps5` and build exactly as its `ps5/BUILD.md`
describes:

```
git clone https://github.com/elripalda/gta4-ps5 && cd gta4-ps5 && ./setup.sh
git apply /path/to/patches/liberty-trainer.patch
cp /path/to/src/gta4_trainer.* glue/rexglue-sdk-main/gta4-recomp/src/
# then the cmake + ninja commands from ps5/BUILD.md
# (glslangValidator must be built with ENABLE_OPT=ON: the shader overrides use -Os)
```

Or just install the prebuilt `PPSA94004` from this repo's release over your existing
one in `/data/homebrew/` — your installed game, saves and settings stay.

## Credits

- **OZORDI** — Liberty Recompiled; **vaduur** — the Android fork; **elripalda** — the
  PS5 port this builds on; **ReXGlue** underneath.
- **sjaak327** — Simple Native Trainer, the model for the menu.
- **JulioNIB**: the Batmobile mod (the PC gadget script cannot run here) and Air
  Combat IV, whose controls the F-16's flight follows.
- **SkylineGTRFreak**: the F-16C model.
- **harithd** (original model) and **Milcin** (IV port): the Grotti Deimos GTS.

Single-player fun only. Don't bring it into other people's sessions.
