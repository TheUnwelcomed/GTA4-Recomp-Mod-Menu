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
| `bat/render_preview.cpp` | Renders the Batmobile's synthesized engine (`src/gta4_bat_engine.h`) through a scripted drive (start-up, idle, blips, four gears, cruise, booster, lift-off, the canopy, shut down). |
| `unarc.py` | Unpacks `.rar` / `.7z` / `.zip` mod archives with the system's libarchive (no unrar needed): `python3 -I unarc.py mod.rar outdir`. |

## Game addresses the trainer relies on (360 build)

Found with `re/ppc_asm.py`:

| what | where | how it is used |
|---|---|---|
| vehicle pool | `*(0x82FB0700)`: +0 storage, +4 generation bytes, +12 slot size | handle → `CVehicle` (as `IS_CAR_MODEL`'s worker `sub_82594230` does) |
| model index of an entity | `+0x2E` (s16) | |
| live skeleton of an entity | `sub_8217DB60` | locals at +0x0C (80 bytes a bone), count +0x10, skeleton data +0x04 |
| a bone's local matrix | `sub_822832E8(entity, bone)` | how the game hides extras |
| hidden extras mask | `CVehicle +0xDC0`, set by `sub_8237A280`, applied each frame by `sub_82381FA0` | |
| vehicles.ide cars line | `sub_8256DC70`: game name → model info +96, frequency +148, max +152 | |
| resource load map | `sub_8285C790`: each segment in 16-page chunks, then power-of-two chunks, largest first; its callers keep the map (u16 counts, 12-byte chunk entries) on the stack, room for 128 chunks | why no GPU buffer may cross a chunk edge (`pc2x360/chunkcheck.py`), and why the add-ons are packed in stock-sized chunks |
| bone of a car by name | the live skeleton's data (+0x04): bone records at +0, 0xE0 bytes each, the name first; the count at +20 | the Batmobile's and the F-16's canopy pivots (`interiorlight`) |
