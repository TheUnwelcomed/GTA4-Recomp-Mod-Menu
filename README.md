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
dialogs use), so you won't punch anyone while navigating. The one exception is
**R2 while you're in a vehicle**: the throttle still reaches the game, so the car
keeps going while you browse.

## Features (Simple Native Trainer's core set)

- **Player** — God mode, Never Wanted, set wanted level 0–6, +$100,000, heal & armor
- **Vehicle**: vehicle god mode, repair, warp-into-spawned-car toggle, Batmobile
  gadgets, F-16 flight and weapons (on from the start: it only acts inside
  the jet), Deimos and Nexus pop-up lights (d-pad left in the car, animated,
  or automatic with the headlights), and **Bring Your Own
  Ride**: a mission that wants a particular vehicle accepts the one you're in, as
  long as it's the same kind (anything on wheels for a car or bike, a helicopter
  for a helicopter, a boat for a boat). A mission's own story vehicle is a
  different check and still counts
- **Car Spawning**: 29 stock cars, bikes and helicopters plus six bundled
  **add-on vehicles**, spawned in front of you. Episode-only vehicles are
  detected and refused outside their episode
- **Add-on vehicles**, converted to the 360 format and installed by the app
  itself as their own vehicles (no stock car replaced; see `addons/`):
  - **Batmobile** (JulioNIB), with its gadgets re-created: a lock-on missile
    on L1 that homes in on the car ahead, twin miniguns ahead and behind, a
    bomb on the horn, a booster that flames and roars, and a canopy that pops
    open as you get in and out
  - **F-16C Fighting Falcon** (SkylineGTRFreak): flies the way GTA V's jets
    do (speed from the engine, stalls, rudder on LB/RB), with a cannon on
    both sides and missiles that lock on a car ahead (the PC mod's Air
    Combat IV script, re-done in the trainer), a canopy that rises as you
    get in and out, a synthesized jet engine that spools with the throttle,
    landing gear on R3 and an afterburner flame
  - **Grotti Deimos GTS** (harithd / Milcin), with pop-up headlights the
    trainer swings on their hinge and opening doors
  - **Vapid Dominator 645** (johndoe968, Rodger Dodger / Milcin), with
    opening doors
  - **Dinka Nexus** (IGNO / Milcin), with gullwing doors that lift as you
    get in and out and pop-up headlights like the Deimos's
  - **Imponte Arbiter X** (johndoe968 / Milcin), with opening doors
  - Each car has its own engine, synthesized in the trainer (no recordings):
    the Batmobile's turbine over a V8, the Deimos's flat-plane V8, the
    Nexus's V6 with its cam switch-over wail, the Arbiter's choppy big V8,
    the Dominator's raspier small V8, the F-16's jet
  - All kept out of traffic: nobody else drives them
- **Teleporting** — to the map waypoint, save/restore a position, landmark presets.
  The destination is loaded first (`LOAD_SCENE`) and you're put on its ground;
  if it isn't ready yet you wait there, held still, until it is. Driving, the
  car comes along
- **Time & Weather** — set the clock, force or release any weather
- **Weapons** — full loadout with ammo, or strip all weapons
- **Chaos**: raining cars (random cars dropped around you), gravity on/off
- **Headlights on D-pad Left** (on by default, the one option that is):
  switch any car's lights on and off; getting out switches them off
- **Trainer Status** — shows which game natives resolved, for debugging

Every option but the headlight switch and the F-16's flight (which does
nothing outside the jet) starts off: the menu changes nothing else until you
ask it to.

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
- **Bring Your Own Ride** replaces three natives at registration (through the
  same thunks the port's motion-control hooks use): `IS_CHAR_IN_MODEL`,
  `IS_CAR_MODEL` and `IS_CHAR_IN_ANY_POLICE_VEHICLE`. Each runs the game's own
  handler first and, with the option on, turns a "no" into "yes" only for the
  player's own vehicle of the same kind. Off, they pass straight through.
- **Cost per frame**: nothing, when nothing is switched on. The script-stage
  hook decides on a time stamp before touching memory, and each toggle (god
  mode, never wanted, the add-on watchers...) runs on its own clock.
- **Spawn guard**: before `CREATE_CAR`, the trainer checks what the game will
  read (the model is a vehicle model info, its fragment and vehicle structure
  are in place, every extra's bone has a parent), waits while the game is still
  setting the model up, and refuses with a full report in the log otherwise.
- **Vehicle model store**: the game sizes it once at boot and adds cars without
  a capacity check, so the trainer gives it 64 more entries (`sub_821C83C0`),
  as the PC game's limit adjusters do.
- **Where the add-ons' data goes**: the console game reads `vehicles.ide`,
  `handling.dat`, `carcols.dat` and `vehOff.csv` from **inside `common.rpf`**
  (`default.dat` names them on the `common:` device), not from the copy the
  port's installer unpacks beside it, while `vehicles.img` lives loose on the
  `platformimg:` device. So the installer writes the cars' lines (in the
  console's 15-field `vehicles.ide` format, on each car's template line) into a
  **shadow copy of `common.rpf`** in `LibertyRecomp/addon_cache`: the archive
  with the edited files appended and its table of contents pointed at them.
  The app redirects `game:\common.rpf` to it, the way the port already swaps in
  its own `xbox360.rpf` for the PS5 button prompts. The installed archive is
  never modified; deleting `addon_cache` restores the stock data.
- **Data-file checksums**: as the game opens some data files (`vehicles.ide`,
  `handling.dat`...) it compares them with built-in checksums
  (`sub_821B5538`, from the data-file open `sub_821B5600`) and stops on a
  mismatch. The add-ons' lines are such a change, so the trainer skips that
  check while the shadow archive is mounted; without it the game checks as
  always.
- **Launch guard**: a launch that mounts the shadow leaves
  `addon_cache/launch-pending.txt` until the game reaches the city. If it is
  still there at the next launch, that launch starts without the shadow (the
  stock data) and says so, and the one after tries again.
- **Self-measurement**: every 10 seconds the log gets a
  `gta4-trainer: cost over 10.0 s` line with the trainer's own time per frame.
- **Deimos pop-ups**: the converter hangs the lamp pods on the template's
  `extra_1`, placed on their hinge; the trainer turns that bone's local
  matrix in the car's live skeleton (`sub_8217DB60` gives the skeleton, the
  same place `sub_822832E8` hands the game when it hides an extra), eased,
  and keeps the extra shown, turning it on only when the game reports it
  off (`IS_VEHICLE_EXTRA_TURNED_ON`): turning it on while the player sat in
  the car snapped the open door shut. D-pad left is kept from the radio in
  the car.
- **Batmobile missile**: an RPG rocket object (`cj_rpg_rocket`) flown by the
  physics engine, its velocity set every frame as TBOGT's `tony8` flies its
  rockets. At launch it locks on the nearest car along the nose's line
  (`GET_CLOSEST_CAR` sampled out to 160 m, wrecks skipped), shown with a red
  radar blip; in flight its line turns towards that car at up to 3 rad/s and
  its nose follows (`SET_OBJECT_QUATERNION`). One every 3 seconds.

## Changes

- **v1.7.17**: the F-16 flies again: *F-16 Flight and Weapons* started off at
  every launch, and off the jet is a car that cannot take off (now it starts
  on); the takeoff rotates the nose up off the wheels at once and nothing ends
  the flight in its first 1.5 s; log lines for what the takeoff sees. The
  Dinka Nexus gets gullwing doors that follow the game's own (unseen) doors
  as Niko opens and shuts them, pop-up headlights like the Deimos's, and its
  own engine (an even V6 to 8,000 rpm with a cam switch-over at 5,800); the
  Imponte Arbiter X (a choppy big cross-plane V8) and the Vapid Dominator 645
  (a raspier small one) get theirs. Their game names (NEXUS, ARBITERX,
  DOM645T) have no IV engine, so the borrowed Infernus and Banshee engines
  are gone.
- **v1.7.16**: the Batmobile's engine leans toward the film car: a turbine's
  roar rising under load over the V8, the idle hum and canopy sounds kept.
- **v1.7.15**: the Dinka Nexus and the Imponte Arbiter X added; the F-16
  takes off as soon as its engines are at full power (pull back), leaving the
  ground at flying speed.
- **v1.7.14**: the Batmobile's engine deeper (a slow-revving V8 with a dark
  exhaust and weight in the low end, the turbine soft and low) and about half
  as loud; the Deimos's engine at a similar level.
- **v1.7.13**: the Grotti Deimos gets its own synthesized engine (a
  high-revving flat-plane Italian V8 with a carburettor howl) and a motor
  whir for its pop-up lamps; its game name is now `DEIMOS` (no Turismo
  engine).
- **v1.7.12**: getting out through a canopy uses the game's own exit
  (`TASK_LEAVE_CAR_IMMEDIATELY`, hidden while the player climbs out), then
  puts the player beside the cockpit: warping out of the seat
  (`WARP_CHAR_FROM_CAR_TO_COORD`, v1.7.8-v1.7.11) dropped him through the
  world. Canopies open only at a standstill.
- **v1.7.11**: the F-16 gets a canopy that rises as you get in and out, flies
  the way GTA V's jets do (speed from the engine, stalls, LB/RB rudder),
  locks its missiles and cannon on a car ahead, fires from both sides and is
  quieter; the Batmobile gets its own synthesized engine (turbine over a V8)
  and canopy sounds; teleports load the destination before putting you on
  its ground; the Batmobile's set-down holds and retries instead of losing
  you under the map; the Dominator 645 takes the mod's coloured set; the
  headlight switch is on by default and switches the lamps off when you get
  out, and the Deimos's pop-ups flip up getting in and down getting out.
- **v1.7.10**: the Batmobile missile locks on the car ahead and homes in,
  leaves from the round intake with a launch flash, and reloads in 3
  seconds; the Deimos door no longer slams shut as Niko sits down (the
  pop-up extra was turned on again at every entry).
- **v1.7.9**: getting out of the Batmobile sets you down at street level
  (`CLEAR_CHAR_TASKS_IMMEDIATELY`, then `WARP_CHAR_FROM_CAR_TO_COORD` onto
  the ground, as Rockstar's missions do), with a catch if you start to
  fall; per-car shaders moved to the front of the shader group (the white
  Dominator 645).
- **v1.7.8**: opening doors on the Deimos and Dominator 645 (each on its
  own hinge), the Batmobile canopy and gadget rework, add-ons packed in
  stock-sized load chunks (the failed respawns).
- **v1.7.5**: the long black lines on the add-on cars are gone (the
  converter moved bones in only one of the three places the file keeps a
  pose: see `tools/pc2x360/README.md`); Batmobile front wheels at its own
  wheel spots; F-16 liveries (paint shaders from the stock airtug); engine
  sounds for the Batmobile and F-16; add-ons kept out of traffic; Deimos
  pop-ups on d-pad left with a real swing; Vapid Dominator 645 added; the
  tools used for all this in `tools/`.

## Files

| | |
|---|---|
| `src/gta4_trainer.h` / `src/gta4_trainer.cpp` | the whole trainer |
| `src/gta4_jet_engine.h` | the F-16's synthesized engine (plain C++, played through the port's own mixer; `tools/jet/` renders it to a file) |
| `src/gta4_car_engine.h` | the add-on cars' synthesized engines (the Batmobile's, the Deimos's, the Nexus's, the Arbiter X's, the Dominator 645's) and their canopy, gullwing and pop-up lamp sounds (the same way; `tools/car/` renders them) |
| `src/gta4_addon_install.h` / `.cpp` | installs the bundled add-on vehicles into the game at launch: model files into `vehicles.img`, data lines into a shadow `common.rpf` |
| `patches/liberty-trainer.patch` | the five small wiring edits to the port |
| `addons/` | the add-on vehicles: what each registers, notes and limits |
| `tools/pc2x360/` | PC → Xbox 360 vehicle converter, verified against the stock FBI, with the rest-pose check |
| `tools/inspect/`, `tools/re/`, `tools/unarc.py` | car file inspection, the game-code disassembly helper, the mod-archive unpacker (see `tools/README.md`) |

## Building

Apply this on top of `elripalda/gta4-ps5` and build exactly as its `ps5/BUILD.md`
describes:

```
git clone https://github.com/elripalda/gta4-ps5 && cd gta4-ps5 && ./setup.sh
git apply /path/to/patches/liberty-trainer.patch
cp /path/to/src/gta4_trainer.* /path/to/src/gta4_addon_install.* /path/to/src/gta4_jet_engine.h /path/to/src/gta4_car_engine.h glue/rexglue-sdk-main/gta4-recomp/src/
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
- **johndoe968** and **Rodger Dodger** (original model) and **Milcin** (IV
  port): the Vapid Dominator 645.
- **IGNO** (original model) and **Milcin** (IV port): the Dinka Nexus.
- **johndoe968** (original model) and **Milcin** (IV port): the Imponte
  Arbiter X.

Single-player fun only. Don't bring it into other people's sessions.
