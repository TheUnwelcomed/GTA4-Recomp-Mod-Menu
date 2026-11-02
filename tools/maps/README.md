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

`mipcheck.py` checks the mip layout (`pc2x360/xenos.py`) against stock
console textures: every level decoded where the layout says it is must look
like its parent scaled down. `sets/` holds each set's settings; the mods'
files themselves are not in this repository.
