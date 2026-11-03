# Map sets: places for the DeLorean's time travel

A PC GTA IV map mod becomes a destination the time machine can travel to:

1. `mapdraw.py` converts each model (`.wdr`) to a console drawable (`.xdr`),
   its shaders cloned from stock console ones (decompressed with
   `pc2x360/xlzx unrsc`); every model read back and checked.
2. `mapset.py <set.json> <source dir> <xdr dir> <template.xtd.bin:flags> <xlzx> <out dir>`
   builds the rest: texture dictionaries with mip chains (`maptex.py`), the
   collision as one bound dictionary (`bounds360.py`), the models' `.ide`
   lines, the placements (`place.txt`) and the destination (`map.txt`: label,
   date, shift, sea level, the arrival road found in the collision, the
   area). Models are renamed with the set's prefix.
3. The output goes in the app's `addons/maps/<name>/`; the installer
   (`gta4_addon_install.cpp`, `FindMaps`) puts the resources into
   `vehicles.img` and the lines into the shadow `common.rpf`, and the
   trainer places and deletes the pieces.

Load chunks (v1.7.35): the console loads a resource segment in chunks
(`pc2x360/batconv.load_chunks`: as many 16-page chunks as fit, the rest in
power-of-two chunks), each wherever its allocator finds room, pointers
translated chunk by chunk, so an array over a chunk's edge is read partly
from the memory after the chunk. The stock bound files cross pages freely but
never a chunk; `bounds360.Arena` keeps every object inside one, rounding the
page count up to whole 16-page chunks when the smaller tail chunks would cut
one (v1.7.34's island: a 17-node tree over the 64 KiB tail chunk's edge, its
last nodes read as zeros, and the game's node walk - a step of 0 for an inner
node of size 0 - never ended; the arrival froze). `xbdcheck.py` and
`boundheaders.py` check it.

`collide.py` makes the collision (v1.7.31): a map mod's collision is often
its render meshes (The Gator Keys: 293,000 triangles, 15 MiB, which the game
never loaded), so each piece is welded, cut below the shallows, decimated
(quadric edge collapse within the set's error, open borders sliding only
along themselves) and split into BVH meshes of up to 6,000 polygons,
coplanar triangle pairs made quads; each mesh gets its own tree, built by the
rules `treerules.py` checks on the mod's PC files and stock console `.xbn`
(depth-first nodes with escape counts, four polygons a leaf, inner boxes the
union of their children, Bullet's 128-node subtree headers). The dictionary
uses 16 KiB pages, as the stock static collision does. `xbdcheck.py` reads a
built dictionary back as the game does and walks rays through every tree
against a brute-force test of every polygon.

How a piece gets its collision (v1.7.34): the game links a model to its bound
only when a bound dictionary holding it loads. The bounds store's set
callback (`sub_82288E90`) walks the dictionary and, for each key that is a
model's name hash, makes an archetype and calls the model's SetPhysics
(`sub_82360060`: model info `+0x04` the archetype, `+0x52` the dictionary's
slot); an object gets collision only from a model whose `+0x52` is set. At
boot the game loads every dictionary once for this, unless its cache is good
(`sub_82289958`), when it loads only those in images outside the cache, so a
set's dictionary in `vehicles.img` is never loaded by the game itself: the
trainer asks for it (`sub_821D00F0`, kept in with flag 2) with the models and
places the pieces once it is in. Its name does not matter, so each set's is
`<prefix>col.xbd` (`map.txt`'s `bounds`). Header values follow the stock
dictionaries (props, objects with mass), not the static bounds: a lone
mesh's volume `-FLT_MAX`, a composite's centre of gravity the
volume-weighted mean of its children's.

Tiles: the game seemingly neither draws nor collides with an object several
hundred metres across (v1.7.31: the island's terrain, roads and rocks
missing, its buildings there), so `mapset.py` cuts every model over the set's
`tiles.radius` into tiles of `tiles.cell` metres (`mapdraw.plan_tiles`,
`MapDrawable(tile=...)`), each a model of its own with its own collision; it
needs `--draw` (the template and shader sources `mapdraw.py` used).

A mod of several archives (v1.7.37, Wake Island: five `.img` files, their
`.ide` and `.wpl` files, the stream `.wpl` files inside the archives, and the
GTA V trees it adds to the game's `ext_veg.img`) is one source directory
(links into the extracted archives will do); the set names all its `.ide`
and `.wpl` files, and only the models the `.wpl` files place are made, a
model placed many times one file. Three more things such a mod needs:

- The game's own props: a mod placing the game's models (Wake Island: 745
  of its 1,290 placements are the game's landing lights, crates, buoys and
  bushes) gets no file for them. `stocknames.py` lists the game's model
  names from the mapsets export's `.ide` files (`stock_models.txt`); a set
  naming that list (`stock`) writes such a piece with the game's model name
  and collision `2` in `place.txt`, and the trainer makes the object from the
  game's model, with the game's own collision.
- Collision by area (`collision.mode` `"world"`): the mod's `.wbn` files
  cover regions (the ground, rock lines, the ship's rooms), not models, so
  they are cut into cells of `collision.cell` metres, each cell's bound
  carried by a piece of its own (`<prefix>c<n>`: a drawable of one triangle
  60 m under the ground, its `.ide` box the collision's) placed unrotated at
  the cell's centre. A big set's collision goes into several dictionaries of
  about 3 MiB (`<prefix>col0.xbd`...; `map.txt`'s `bounds` names them all,
  and the trainer waits for every one).
- Shaders: the terrain shaders (`gta_terrain_va_3lyr` / `4lyr`) blend 3 or 4
  texture layers by weights in the vertex (texcoord sets after the layers'
  coordinates); with no console sample of them, `mapdraw.py` draws such a
  geometry as one plain geometry per layer, each triangle given to the layer
  weighing most at its corners (`split_terrain`): sand, dirt and scrub by
  area, hard edges between. Glass, alpha and decal shaders stand in as the
  one blended sample (`gta_decal`), until the export brings the real ones.

The object pool: the game makes room for 1300 objects at boot
(`sub_82326E10`); Wake Island is some 1,600 pieces, so the trainer makes the
pool with room for 3400 (its override of that function).

Shaders (v1.7.37): the game's cut-out presets are its plain effects in another
draw bucket (read off the PC files: `gta_cutout.sps` is `gta_default.fx` in
bucket 3, `gta_alpha.sps` the same in bucket 1, `gta_decal.sps` in bucket 2),
so `mapdraw.py` makes them from the effect's console sample with the bucket
and preset name changed (`MAP_PRESETS`); the trees' and fences' leaves are
cut out of their cards at last, with back faces added for the one-sided
cards (`TWO_SIDED`). An opaque shader whose colour map is a plant's cut-out
mask (`alphamask.py`, `--cutout-from=<source dir>`) is drawn cut out as well.

Planting (`plant.py`, a set's `plant` patterns): a vegetation cluster's
plants are found as connected pieces of its triangles gathered by base (a
palm's trunk, the fronds starting up in its crown, the tufts at its foot),
and each is moved so its base sits just under the ground the set's
collision has there - the surface its own trunk passes through, or one it
stands just above; nothing is moved to a roof over its crown or a seabed
far under it, and pieces that are not a plant of their own go with the
plant they touch or stay. Each planted trunk gets a box of collision. Only
clusters placed once and unrotated are planted; Wake Island's trees stand
right as the mod placed them and are left alone.

Life (`life` in a set's settings): the vehicles the mod parks (its `.wpl`
car generators, `vehicles.txt`: model, position, heading; the game's own
vehicles by name, `stock_vehicles.txt`, another game's swapped by
`vehicle_subs`), the game's ped models that belong there (`peds`, `wander`
of them kept about the player), the weather (the game's id) and the
trainer's ambience by name (`keys`, `atoll`: `src/gta4_island_sound.h`).
`ground.bin` is the collision's walkable surface on a 4 m grid for the
trainer's own ground probe (the game's does not see the pieces).

Streamed by distance (v1.7.38, a set's `stream`, map.txt's): the trainer
makes the pieces within that of the player (the arrival first), deletes
those gone a sixth farther and lets their models go, so a set takes only
the streaming memory of what is near. Wake Island at once (1,600 pieces,
160 MiB) filled the game's 268 MB and the world stopped streaming (log
104); it is streamed within 700 m now, the Keys with the mansion within 1 km.
The pieces with collision are made first.

Edits and additions (v1.7.38): `edit` leaves out of a model the geometries
drawn with the named colour maps (`mapdraw.py --edits`) and the collision
triangles in the named boxes (the model's space); `extra` names text files
of placements beyond the mod's (`model x y z heading`, or with a quaternion:
the game's props by name, or models made here: `props.py` writes Benny's
new sign and tiki bar as `.wdr`/`.wtd` with their collision in
`<model>.colmesh.npz` and an `.ide`, all linked into the source directory).
`tex_of` caps a dictionary's texture size by name.

GTA V (v1.7.38, `vimport.py`): the Scarface mansion (a FiveM MLO) joined the
Keys as PC pieces: `v_reader.py` reads the V resources (RSC7: drawables with
their shaders, vertex formats and embedded textures; texture dictionaries;
`.ymap` entities through the `.ytyp` archetypes), `pc2x360/pcwrite.py`
writes IV PC files (the shaders cloned from the mods' own PC samples of the
nearest preset, `V_SHADERS`), and the set takes them like a mod's. A V
entity's rotation is stored inverted as IV's are; the whole scene can be
turned and anchored (`--anchor=vx,vy,vz:tx,ty,tz --turn=deg`); a model
placed once is written turned as placed (its placement unrotated, so it can
be tiled and its render mesh taken as collision). What the pack lacks (the
game's own props it places) is listed and left out.

Creatures (v1.7.39, `creature.py`, a set's `life.creatures`): a GTA V animal
cut into rigid parts by the bones that drive each vertex (a triangle goes
with most of its corners), each part a PC drawable in the creature's space
with a pivot (the midpoint between the part and the body along the
creature's length, or given) and an axis; `creature.txt` goes to the set,
which lists the part models as its own and writes `creatures.txt` with how
many of each kind. The trainer places the body and turns the parts about
their pivots as the creature swims or walks.

Names: the game registers an image's files by the first 24 characters of the
name, the extension among them (`sub_825127C0`), so `mapset.py` keeps every
model and texture dictionary name within 20 characters (a set can rename
them in its settings) and the installer skips a longer one.

`mipcheck.py` checks the mip layout (`pc2x360/xenos.py`) against stock
console textures: every level decoded where the layout says it is must look
like its parent scaled down. `sets/` holds each set's settings; the mods'
files themselves are not in this repository.
