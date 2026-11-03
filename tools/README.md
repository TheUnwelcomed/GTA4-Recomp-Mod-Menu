# Tools

Everything used to make and check the bundled add-on cars and to find the
game code the trainer hooks. None of it ships game data: point it at your own
console exports and the mods' own files.

| tool | what it does |
|---|---|
| `pc2x360/` | PC car mod (`.wft`/`.wtd`) → Xbox 360 add-on car (`.xft`/`.xtd`), the validator, the rest-pose and physics-part checks, the load-chunk check (`chunkcheck.py`: works on any 360 `.xft`/`.xtd`, stock ones too), `render_posed.py` (a converted car drawn with its doors, canopy or other pivots turned), and `build_bundled.sh` for the bundled set. See its README. |
| `inspect/skeleton.py` | A car's skeleton (PC or 360): parents, local translations, positions, rotations, which bones physics parts drive. Spots parked bones like the Batmobile's 13 m-deep doors. |
| `inspect/palettes.py` | Which bones each geometry is skinned to, and its shader. |
| `re/ppc_asm.py` | A game function's PowerPC assembly, from the comments the recompiler leaves in `generated/gta4_recomp.*.cpp`; `--find` lists the functions using an instruction pattern. |
| `jet/render_preview.cpp` | Renders the F-16's synthesized engine (`src/gta4_jet_engine.h`) to a WAV through a scripted flight (start-up, throttle, afterburner, cannon, missile, shutdown), to hear changes without a console. |
| `jet/flight_model_sim.py` | The F-16's flight model (the trainer's equations) run through takeoff, an idle glide, banks, a pulled turn, a stall, the afterburner and the rudder; prints speed, climb, bank, heading and height. |
| `car/render_preview.cpp` | Renders an add-on car's synthesized engine (`src/gta4_car_engine.h`: `batmobile` or `deimos`) through a scripted drive (start-up, idle, blips, four gears, cruise, booster or full throttle, lift-off, the canopy or the pop-up lamps, shut down). |
| `unarc.py` | Unpacks `.rar` / `.7z` / `.zip` mod archives with the system's libarchive (no unrar needed): `python3 -I unarc.py mod.rar outdir`. |
| `release/pack.py` | Packs the release zip: the packaged app folder (`ps5/tools/ps5_package.sh`'s `PPSA94004`) with its `addons/` replaced by the add-on payload and the install notes at the root, written straight from the folders and checked; the app's AES key left out unless `--keep-key`. |
| `jet/flight_test.cpp` | The jets' flight model and autopilot (`src/gta4_jet_flight.h`) flown through takeoffs, stalls, gear and brake changes, landings ahead and full approaches to Francis International's runway, with pass/fail checks; `FLIGHT_TRACE=1` prints each phase. |
| `pc2x360/remaster.py` | A car's textures remastered from its own colour map: the DXT blocks smoothed (edge-keeping), resized and lightly sharpened, and real normal and specular maps made from its detail where a mod ships flat placeholders (the Batwing). |
| `missions/` | Complete Current Mission's table. `analyze.py` and `table.py` read decompiled mission scripts (not included) for each mission's pass routine, its debug pass key and the stage statics its pass needs, with a value signature to find them in memory; `gen_header.py` writes `src/gta4_mission_pass.h`: `python3 -I gen_header.py <scripts dir> <out.h>`. |
| `maps/arx.py` | Lists or extracts any archive libarchive reads, block by block, so an archive whose checksum fails still yields what was decoded (and says so). |
| `maps/rpf7.py` | Lists, checks and extracts a GTA V RPF7 pack (`dlc.rpf`, unencrypted), nested RPFs included: every resource must inflate to its page flags' size. For the map ports to come. |
| `island/render_preview.cpp` | Renders a place's synthesized ambience (`src/gta4_island_sound.h`: the Keys or the atoll) to a WAV with the level of each 10 s stretch. |
| `maps/alphamask.py` | Which of a mod's textures are cut-out masks (alpha partly clear, partly solid) and which of those are plants' (by name, or used by a plant model): `mapdraw.py --cutout-from` draws an opaque shader with one of them cut out. |
| `maps/plant.py` | Plants a mod's tree clusters: each palm (a trunk and its fronds, found as connected pieces grouped by base) moved to the ground its trunk passes through, with a trunk of collision; `mapset.py`'s `plant` setting. |
| `maps/vimport.py` | GTA V map resources (`.ydr`, `.ytd`, `.ymap`, `.ytyp`) as a set's PC pieces: IV PC `.wdr`/`.wtd` (`pc2x360/pcwrite.py`), their lines, the placements with their full rotation (turned and put where the set wants them: `--anchor`, `--turn`), collision from the render meshes; a V shader becomes the nearest IV preset, its maps by sampler name. |
| `maps/props.py` | Props made here as PC pieces for a set (Benny's double-sided sign and tiki bar: meshes, their own textures drawn with PIL, collision). |
| `pc2x360/pcwrite.py` | Writes GTA IV PC resources from plain data: drawables (shaders cloned from PC samples of the same preset, vertices by usage bits, bounds) and texture dictionaries (DXT levels), laid out as the game's own, read back by everything here. |
| `maps/stocknames.py` | The game's own map model names from the mapsets export's `.ide` files (`maps/stock_models.txt`): a map set names a piece of one of them by that name, with no file of its own; and its vehicle names (`maps/stock_vehicles.txt`) for the parked vehicles a mod's `.wpl` files generate. |
| `maps/boundheaders.py` | A console bound dictionary's or static bounds' headers side by side (pointers as P), and the two conventions they differ in checked: a composite's centre of gravity (the dictionaries: the volume-weighted mean of its children's; the static bounds: the plain sum) and a lone mesh's volume (the dictionaries: -FLT_MAX). |

## Game addresses the trainer relies on (360 build)

Found with `re/ppc_asm.py`:

| what | where | how it is used |
|---|---|---|
| vehicle pool | `*(0x82FB0700)`: +0 storage, +4 generation bytes (top bit: free), +8 slot count, +12 slot size | handle → `CVehicle` (as `IS_CAR_MODEL`'s worker `sub_82594230` does); the jet's targeting walks it |
| an entity's position | `+32` its matrix (position row at +48), or inline at `+16` without one | the player's and, once checked against `GET_CAR_COORDINATES`, the targets' |
| the executing script thread | `*(0x8319277C)` during a native call; its program key (joaat of the script's name) at +8 | Bring Your Own Ride answers only mission scripts; Complete Current Mission finds the mission's thread and statics |
| the current viewport | `0x831C2200` (a `grcViewport`, or a pointer to one): camera position +0x70, view-projection +0x100 | the jet's target marks, used only while its camera matches `GET_CAM_POS` |
| model index of an entity | `+0x2E` (s16) | |
| live skeleton of an entity | `sub_8217DB60` | locals at +0x0C (80 bytes a bone), count +0x10, skeleton data +0x04 |
| a bone's local matrix | `sub_822832E8(entity, bone)` | how the game hides extras |
| hidden extras mask | `CVehicle +0xDC0`, set by `sub_8237A280`, applied each frame by `sub_82381FA0` | |
| vehicles.ide cars line | `sub_8256DC70`: game name → model info +96, frequency +148, max +152 | |
| resource load map | `sub_8285C790`: each segment in 16-page chunks, then power-of-two chunks, largest first; its callers keep the map (u16 counts, 12-byte chunk entries) on the stack, room for 128 chunks | why no GPU buffer may cross a chunk edge (`pc2x360/chunkcheck.py`), and why the add-ons are packed in stock-sized chunks |
| bone of a car by name | the live skeleton's data (+0x04): bone records at +0, 0xE0 bytes each, the name first; the count at +20 | the Batmobile's and the F-16's canopy pivots (`interiorlight`) |
| bounds store (bound dictionaries) | pool `*(0x82C6C3F4)`: +0 entries, +4 a flag byte each (0x80 free), +8 count (330), +12 entry size; entry +0 the dictionary while in, +4 references, +8 the name's hash; its streaming module `*(0x82A98C1C)` | a map set's collision: the trainer finds its dictionary's slot by name and asks for it |
| a model's bound | model info +0x04 the archetype, +0x52 (s16) its dictionary's slot, set by SetPhysics (`sub_82360060`) when the dictionary loads (the store's set callback `sub_82288E90` → `sub_82288890` per key) or from the cache's records at boot (`sub_82288AD8`); an object gets physics only with +0x52 set (`sub_82500400`) | why a map set's pieces had no collision till v1.7.34 |
| streaming requests | `sub_821D00F0(slot, module, flags)` (0x10 priority, 0x2 kept), `sub_821CFF60(slot, module)` clears 0x2 | the trainer keeps a map set's dictionary in while the place stands |
| object pool | made by `sub_82326E10` at boot: `sub_82507368(pool, 1300, name, 800)` into `*(0x82DE5A50)` (+0 the objects, +4 a flag byte each, +8 count, +12 size, +20 in use); `sub_822B9B18` turns the city's dummies into objects only with 50 free | the trainer makes it with room for 3400 (v1.7.37: Wake Island's 1,400 pieces) |
