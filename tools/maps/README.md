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

Names: the game registers an image's files by the first 24 characters of the
name, the extension among them (`sub_825127C0`), so `mapset.py` keeps every
model and texture dictionary name within 20 characters (a set can rename
them in its settings) and the installer skips a longer one.

`mipcheck.py` checks the mip layout (`pc2x360/xenos.py`) against stock
console textures: every level decoded where the layout says it is must look
like its parent scaled down. `sets/` holds each set's settings; the mods'
files themselves are not in this repository.
