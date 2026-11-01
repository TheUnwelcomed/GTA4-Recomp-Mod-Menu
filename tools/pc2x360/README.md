# pc2x360 — PC GTA IV vehicle → Xbox 360 vehicle converter

The PS5 port runs the **Xbox 360** build of GTA IV, which streams `.xft`/`.xtd`
models (big-endian structures, Xenos GPU formats, XMem LZX compression). PC car
mods ship `.wft`/`.wtd`. This converts a PC car into the 360 format by
**transplanting** it into a stock 360 car of your own game:

- kept from the stock 360 car: physics/collision, fragment groups, skeleton
  topology and rotations, the non-wheel children;
- taken from the PC car: the body (all LODs), the wheel, shaders (cloned from
  the same-named 360 shaders - the template's or another stock car's - values
  transferred by parameter name hash), textures (merged into the `.xtd`),
  bounds, and the positions of the template's point bones (wheels, hubs and
  suspension, seats, lamps, exhausts), moved consistently everywhere the
  file keeps a bone's pose.

## Verified against ground truth

Every format rule here was checked against a car that exists on both
platforms — the stock FBI Buffalo, PC vs the 360 files exported from a console.
`identity_test.py` converts the stock **PC** FBI into the stock **360** FBI and
compares with the real 360 file:

| check | result |
|---|---|
| skeleton: 58 bones + 3 × 58 transform matrices | byte-identical |
| both ready-made skeleton poses and the collision parts | untouched (nothing to move) |
| fragment and drawable bounds | byte-identical |
| all 9 shaders: parameter values and texture names | byte-identical |
| lod0 vertices (position, normal, tangent, UVs, colour, weighted bones*) | 13,913 / 13,913 identical |
| GPU vertex/index descriptors | consistent |

\* Parts the converter deliberately draws rigid on the chassis (glass,
lamps: see the skeleton section) are compared as bound to the chassis.

Texture conversion (8-in-16 endian swap + Xenos 2D tiling) reproduces the 360
FBI's texture blocks exactly at every mip level. Rockstar's tools truncate
toward zero when packing normals (`Dec3N`) and UVs (half floats); the converter
does the same.

### The bundled cars

`validate_vehicle.py` decodes every converted vertex back and compares with the
PC file. It also checks GPU descriptors, bone bindings, shader inputs, and that
every donor texture untiles back to its source.

| car | template | result |
|---|---|---|
| Batmobile | FBI | 22/22 checks: canopy on a pivot, 262,043 vertices skinned at rest, 0.00000 m off |
| F-16C (airtug slot on PC) | Cavalcade | 22/22 checks: paint2/paint3 from the airtug, landing gear on three pivots (12,872 vertex influences split by position), the canopy on a fourth (`interiorlight`, hinged at its rear edge), 74,436 vertices exact |
| Grotti Deimos GTS | Banshee | 22/22 checks: doors on the Deimos's hinges, lamp pods on a pivot, 119,139 vertices exact |
| Vapid Dominator 645 | Banshee | 22/22 checks: doors on the Dominator's hinges, 91 geometries, 293,238 vertices exact |
| Dinka Nexus | Banshee | 22/22 checks: gullwing doors kept shut (they turn about the length), 108 geometries |
| Imponte Arbiter X | Banshee | 22/22 checks: doors on the Arbiter's hinges (0.40 m), 119 geometries |

`build_bundled.sh` converts, packs and validates all four in one go.

### The skeleton (the v1.7.4 black lines)

A converted car drew long black slivers across the map. The game keeps a
car's bone poses in several places: the skeleton data (each bone's local
translation at +0x20, its position in the car at +0x60, and three matrix
arrays: global rotations, their inverses, local matrices), **two ready-made
copies of the whole pose in the fragment** (fragType +0x1DC and +0x1E0: the
pose a spawned car starts from), and the collision composite's part matrices
for the physics parts (doors, bonnet, glass, bumpers, wheels). Skinning
draws a vertex with the bone's current matrix times the inverse of its rest
pose. v1.7.4 moved bones in the skeleton data only, so for every moved bone
the two disagreed: the Batmobile's door and windscreen bones sit 13 m under
the car in the mod, and their parts were drawn 14 m up. The game also hides a
bone (an extra switched off, a part broken off, a smashed window or lamp) by
overwriting its local matrix with a constant that sends it, and everything
under it, to the world's origin: the Deimos's rear glass hung under the
Banshee's roof extra the pop-ups switched.

Since v1.7.5:

- A moved bone is moved everywhere, its local translation recomputed against
  its template parent, and its collision part (wheels) with it; the
  composite's bounds grow to hold it.
- Only point bones move: wheels, hubs, suspension, seats, lamps, exhausts,
  engine, interior light. Never a part the physics poses (except the wheels,
  which the vehicle code drives). A donor bone parked outside its own model
  is ignored.
- The donor's geometry rides only on bones the game never hides or poses
  from a different car's parts: the chassis, the body shell, the wheel
  assembly and a pivot. Everything else is drawn rigid on the chassis, where
  the modder put it.
- `restpose.py` checks it the way the game draws it: every vertex skinned
  with the ready-made pose and the rest pose must land where it is stored,
  using only those bones, with the collision parts on their bones. The
  shipped v1.7.4 files fail it (Batmobile 13.97 m, F-16 5.87 m, Deimos
  2.76 m); every car since passes with 0.

### The page rule (the v1.7 crash)

The console loads a resource's virtual segment in 4 KiB pages and translates
pointers page by page, so **no object reached through a pointer may straddle
a page boundary**. Rockstar's packer keeps to this: of 15 stock 360 cars, 11
have no crossing at all. v1.7's converter appended its structures without
regard to pages, so some geometry, vertex-buffer and index-buffer records
straddled. On the console the part past the line came from the wrong memory,
and the cars crashed on load or spawn: a vertex buffer's header pointer read
as null in its placement, and a model whose fragment never attached left
its vehicle structure null. The allocator now starts any object that would
cross on the next page, and `validate_vehicle.py` checks every pointed
object (records, arrays, strings, texture headers) against the rule.

The **physical (GPU) segment is loaded in chunks**, each allocated on its own:
the game's resource map (`sub_8285C790`) cuts a segment of N pages into as
many 16-page chunks as it holds, then the remainder into power-of-two chunks,
largest first. A vertex buffer, index buffer or texture level that runs past
its chunk's end is read from whatever memory follows that chunk. Up to v1.7.5
the converter packed the physical segment as one block, and on the PS5 the
add-on cars grew huge stretched planes and spikes that changed as you drove
(the port's log flagged each straddling buffer as "changed without an
unlock", at the exact offset of the chunk edge). Rockstar's packer never lets
a buffer cross (15 stock cars, 30 files: none does). `pack_physical` now
picks the page size and count and places every buffer and texture inside one
chunk, keeping a template dictionary's own chunks whole; `chunkcheck.py`
checks a file against the rule, and `validate_vehicle.py` runs it.

**Chunk size.** Each chunk is one allocation the streaming heap must find
whole. Stock cars load in 128 and 256 KiB chunks. Up to v1.7.7 the converter
packed for the smallest file, and the add-ons came out in chunks of up to
2 MiB. Fresh after a launch the console found room for those; after a while
of streaming the world it often didn't: a second spawn of an add-on did not
load (log 053), and the first Batmobile took 17 s. Now the page size is the
smallest whose 16-page chunk holds the biggest object, and at least 16 KiB:
256 KiB chunks for most files. A 1024-pixel texture (the F-16's liveries) or
a big vertex buffer (the Dominator's body) still needs 512 KiB-1 MiB. The
page count may pass 63 (the field has 11 bits, and Rockstar's own Infernus
textures use 70). The resource map's callers keep room for 128 chunks; the
packer stays under 100.

### What the converter handles (beyond a like-for-like car)

- **Donor bones the template lacks** (`misc_*`, `steeringwheel`, ...), and
  parts the game would pose from the template's physics parts (bonnet, boot,
  bumpers, glass) or hide (lamps, extras), are drawn rigid on the chassis.
  Vertices are stored in model space, so parts keep their place and lose only
  their own motion.
- **Doors** swing. A door hangs on a joint the game builds from the skeleton
  when it swings: the bone's position is the hinge, its rotation limits
  (+0xB0 min, +0xC0 max) the axis and travel. Its physics part's fragment
  child keeps the inverse of the bone's rest matrix (+0x10) and its link's
  centre of gravity (+0x50), and fragType +0xF8 each part's offset from that
  centre. Every stock car keeps these consistent with the skeleton. For each
  template door the donor has too (on the car, not parked; neither bone
  rotated; turning about the same single axis; with geometry), the template's
  door bone goes to the donor's hinge. Its collision part, child data and
  travel follow, and the donor's door and what hangs under it (windows) ride
  on it. A door is hidden only when it breaks off, and the game then draws
  it on its own from the same drawable. The Deimos's hinges are 0.39 m from
  the Banshee's, the Dominator's 0.28 m. The F-16's "doors" are control
  surfaces in rotated frames and the Batmobile's are parked, so both stay
  rigid. Bonnets and boots stay rigid: mods hinge them at either end (the
  Deimos's engine cover, the Dominator's bonnet at the windscreen), so the
  template's lid would hang the wrong way round. `render_posed.py` draws a
  converted car with its doors (or pivots) turned, to check the hinges.
- **Moved physics parts' child data** follow their bones (`_move_children`),
  for the wheels too: up to v1.7.7 their +0x10 and offsets still named the
  template's wheel positions, which the game reads once the car is
  articulated (a door open). `validate_vehicle.py` checks every part.
- **A missing wheel** takes its opposite's place (a tricycle's nose wheel fills
  both front slots). **No wheel mesh at all** hides the template's wheels.
- **Shaders the template lacks** map to its nearest one (`paint2`/`paint3` to
  `paint1`, `interior` to `interior2`, `generic` to `mesh`, `default` to
  `interior2`), which the game colours and lights like its own car's. Named
  stock cars (`sources` argument) can supply the real shader instead, name
  strings copied along: the F-16 takes `paint2`/`paint3` from the airtug for
  its liveries. It's opt-in: v1.7.5 took shaders from other cars for every
  add-on, and the Dominator's borrowed `paint3` stayed white while the
  Batmobile's borrowed `interior` came out chrome. When the 360 shader reads a
  texcoord set the part lacks, it is filled from set 0.
- **Per-car shaders go first.** Stock cars keep the shaders the game drives
  per car (paint, lamps on and off, glass, tyres) in the first 12 slots of
  the shader group (paint at 0, or at 10-11 on the police car and taxi).
  When one of them would sit past slot 11, the converter moves them to the
  front (paint, lamps, glass, tyres, then the rest in the donor's order) and
  remaps every geometry's shader index; the log says `shaders: per-car
  shaders moved to the front`. (v1.7.9 did this hoping it was why the
  Dominator 645 stayed grey; it wasn't. Its colour list was: see
  `addons/dom645t/README.md`.)
- **A donor drawn with paint2/paint3 takes paint1's colour**: the shader
  that stands in for them reads a combination's first colour, so a mod whose
  `carcols` puts its body colours second or third needs them moved first.
- **Physics wheels** take the donor's radii. A wheel's collision part is a
  curved geometry (a disc swept by the bound's margin); the template's curved
  edges, box and bounding sphere are resized to the donor's wheel (its box,
  which mods' exporters fit to the wheel). On the template's own wheels every
  add-on floated by the difference (the Dominator 6 cm on the Banshee's
  0.38 m).
- **Uncompressed textures** (A8R8G8B8, A8, L8) are compressed to DXT5
  (`dxt.py`), so they take the same verified path as every other texture.
- **Exporter quirks**: geometry counts left zero (taken from the buffers), the
  buffer's data pointer only at +24, and un-normalised normals (normalised;
  in-range ones are untouched).
- **Textures borrowed from another stock car's dictionary** get a black
  stand-in. The shared `vehicle_generic_*` pack resolves the rest.
- **Pivots** (`pivots` argument, see `batconv.parse_pivots`): donor geometry
  bound to a template bone no physics part drives and nothing hangs from,
  moved to its hinge, which the trainer turns in the car's live skeleton.
  Several pivots are separated by `;`, a hinge is given with `@x,y,z` (else
  the first donor bone's position), and a donor bone can be split between
  pivots by position (`boot:y>1.5`): its vertices are re-bound one by one.
  The Deimos's pop-up lamp pods swing on the Banshee's `extra_1`; the F-16's
  nose gear (the front of the mod's `boot`) and two main gears fold on the
  Cavalcade's `extra_1`, `extra_5` and `extra_6`; the Batmobile's canopy
  (the mod's parked `door_pside_f` and `windscreen`) on the FBI's
  `interiorlight`, which has no extras, hinged at its rear edge (the trainer
  finds that bone by name); the F-16's bubble canopy (the mod's `windscreen`)
  on the Cavalcade's `interiorlight` at (0, 2.3, 1.22), its rear tip. (v1.7.1-v1.7.4 emitted
  posed copies on three extras instead; hiding an extra drew their shared
  triangles to the world's origin.) The template needs the skeleton's
  rotation arrays (+8, +12): of the stock cars only the Banshee, Cavalcade,
  FBI, Infernus, NOOSE and Turismo carry them, and only the Cavalcade has
  three spare extras on its chassis.

## Format notes (what was mapped)

- RSC5 360: header big-endian; body = `0x0FF512EF`, u32be length, XMem LZX
  frames (`u16be csize` per 32 KiB frame, `0xFF u16be usize u16be csize` for a
  short one). `xlzx` decodes with the port's patched libmspack and encodes with
  LZX *uncompressed* blocks; every write is round-tripped before it is saved.
- Vertex elements (360): position f32×3 BE; normal/tangent/binormal `Dec3N`
  (10:10:10, /511, tangent sign in the top 2 bits); UVs half×2 (u high, v low);
  weights/indices/colour byte-reversed dwords. Declaration types word
  `0xAA1111111199A996`.
- Vertex/index buffers carry an embedded D3D resource header with the Xenos
  fetch constant (vertex: address|3, size in dwords, endian 8in32).
- Shaders: 0x60-byte header; parameter block (offsets | types | name hashes |
  values) replicated byte-for-byte from the 360 template; the type byte is the
  vec4 count (1, 4 = matrix, 15 = array); texture parameters reference textures
  by name. The shader group stores per-shader required vertex inputs.
- Textures: 0x40-byte info → D3D texture header (Xenos fetch constant: format,
  pitch, 4 KiB-page base address, mip address); tiled data padded to 32×32
  blocks per level. Dictionary hash = Jenkins one-at-a-time of the lowercase
  bare name; hash table sorted.
- Skeleton bones are 0xE0 bytes, identical layout on both platforms.

## Use

```
clang++ -std=c++20 -O2 -I<libmspack/mspack> xlzx.cpp lzxd.o -o xlzx   # port's patched libmspack
./xlzx unrsc fbi.xft work/fbi.xft.bin && ./xlzx unrsc fbi.xtd work/fbi.xtd.bin
python3 identity_test.py <export_dir> <stock PC fbi dir> work         # prove the rules
python3 run_vehicle.py <export_dir> work car.wft car.wtd out mycar [template] [pivots] [sources]   # template: fbi; "-" skips one
python3 validate_vehicle.py out mycar car.wft car.wtd [pivots]        # structural checks, rest pose included
python3 restpose.py out/mycar.xft out/mycar.xft.bin [pivot bone,...]  # the rest-pose check alone
python3 chunkcheck.py out/mycar.xft out/mycar.xft.bin out/mycar.xtd out/mycar.xtd.bin   # GPU buffers vs load chunks
python3 render_posed.py out/mycar mycar doors.png door_dside_f:z:-60 door_pside_f:z:60   # draw it with bones turned
./xlzx rsc 112 <flags> out/mycar.xft.bin mycar.xft   # flags from out/flags.txt
./xlzx rsc 7   <flags> out/mycar.xtd.bin mycar.xtd
```

`build_bundled.sh <export_dir> <work_dir> <sources_dir> <out_dir> [model ...]`
does the whole set for the bundled cars, or the ones named (sources:
`<sources>/<model>/<model>.wft/.wtd`).

A mod that ships its cars inside an IMG archive (most big packs do, with the
archive's table encrypted like the game's own):

```
python3 img3.py pack.img aes_key.bin list                     # the archive's items
python3 img3.py pack.img aes_key.bin extract out car.wft car.wtd
```

`aes_key.bin` is the game's 32-byte archive key (the one a LibertyRecomp
install keeps beside the game).

Big PC models: a geometry whose vertices would not fit the stock cars' 256
KiB load chunks is drawn as several (`split_geometry`: its triangles in
order, each part with the vertices it uses); `validate_vehicle.py` matches
the parts back to the donor's geometry corner by corner. A texture larger
than 1024 px starts from its first mip level that fits (`fit_texture`).

Then add an `addons/mycar/` folder (see `addons/README.md`) and the app
installs it as a new vehicle.

The stock 360 files come from your own console: the app exports the FBI and
a set of other stock cars (low sports cars, cars with extras, the airport
tug) to `/data/homebrew/PPSA94004/logs/export/` on launch. Any of them can be
the template (`run_vehicle.py ... mycar turismo`).
Converted files are not committed here — they contain data from the game.

## Known limits

- Collision stays the template car's (the Batmobile collides like a Buffalo);
  only the physics wheels take the donor's size.
- Bonnet, boot and glass are fixed to the body: they don't open or break.
  Doors open on the donor's hinges, with the template door's collision
  shape.
- Template bones the donor lacks (rear doors, sirens, …) keep their position;
  nothing of the donor is drawn there.
- Donor textures: DXT1/3/5 or A8R8G8B8. Mip chains beyond level 0 are not
  emitted.
- Behaviour that PC patches add (ZolikaPatch's pop-up lights or moving
  steering wheels) is not part of the model and does not carry over.
