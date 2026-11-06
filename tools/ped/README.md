# Ped tools

A GTA V ped mod as the console's player outfit (v1.7.46: the Beyond suit).

The player's clothes on the console are one drawable per slot in
`playerped.rpf` (`<slot>_NNN_<u|r>.xdr`, its colour map in
`<slot>_diff_NNN_a_<uni|whi>.xtd`, the normal and specular maps embedded in
the drawable), skinned to the player's 90-bone skeleton; the game counts a
slot's drawables by the files it finds. A V ped is a `.ydd` of drawables on
V's 98-bone skeleton (`.yft`) with its textures in the `.ytd` or embedded.

## Layout

A work folder (`<root>`) holding, as `specs/beyond.json` names them:

- `mod/` - the V mod's `.ydd`, `.yft`, `.ytd`;
- `iv/src/` - the console's player files from the trainer's export
  (`logs/export/playerped/files`: the exporter writes their headers
  little-endian) with `xlzx unrsc` bodies beside them as `<file>.bin`: the
  template component (`feet_000_u.xdr`, `gta_ped.sps`), the shader source
  (`uppr_000_u.xdr`, `gta_ped_reflect.sps`), the texture template
  (`uppr_diff_000_a_uni.xtd`);
- `samples/` - any IV PC `.wdr` files whose shaders carry the diffuse, normal
  and specular samplers (`gta_normal_spec.sps`, `gta_normal_spec_reflect.sps`),
  for the PC drawable the converter writes first;
- `build/suit.wtd` - the touched-up textures (`pedtex.py`).

## Steps

    python3 -I pedtex.py <mod.ydd> <mod.ytd> <root>/build/suit.wtd <png dir> <diffuse> <normal> <spec>
    python3 -I pedconv.py specs/beyond.json <root> <out>
    python3 -I pedcheck.py <out> uppr_017_u uppr.png <xlzx>
    python3 -I ../pc2x360/rpf2.py playerped.rpf aes_key.bin add test.rpf <out>/*.xdr <out>/*.xtd

The components go to `addons/beyondsuit/playerped/` in the release; the
installer puts them where the game reads the player's clothes.

## What the conversion does

- the mesh turned 180 degrees about z (V peds face -y, left at +x; IV's
  face +y, left at -x);
- every vertex's bones mapped to the IV bone of the same joint
  (`V_TO_IV`; the helper bones - IK, PH, MH, FB, the rolls - ride on the
  nearest joint), its position moved by that joint's offset between the two
  bind poses (both the same A-pose, the joints a few centimetres apart), so
  the IV skeleton's rest matrices draw it where the modder put it;
- the triangles cut into geometries of at most 25 distinct bones (the
  console's player components carry 25), taken in the order of their
  heaviest bone;
- a skinned PC drawable (`pcwrite`, four weights a vertex) with one
  `gta_normal_spec` shader naming the textures, converted on the stock
  component: the stock file's virtual segment kept whole (its skeleton, the
  drawable header), the stock `gta_ped` shader cloned with the donor's
  texture names, the vertices in the console's layout, the normal and
  specular maps embedded (tiled, with mips), the bounds the donor's;
- the colour map as the slot's `.xtd` (one texture variation, "a");
- placeholders (a 2 mm triangle in the pelvis) for the slots the suit covers;
- every file stored as `playerped.rpf` stores it: a little-endian RSC
  header, then the LZX body (the `.img` archives' files carry the header
  big-endian).

The vertex colour: the player's hands carry (A, R, G, B) = (255, 216, 136,
216) everywhere (red and blue an occlusion term, green a mask), so a suit
with no baked occlusion gets that.
