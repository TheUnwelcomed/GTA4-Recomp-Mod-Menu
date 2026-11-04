"""A GTA IV map model (.wdr, PC) as a console drawable (.xdr).

The vehicle converter (pc2x360/batconv.py) does the work: shaders cloned
from stock console ones and filled from the mod's parameters by name hash,
geometry converted vertex by vertex, buffers laid out in load chunks. A map
model is a plain drawable at the root of its file (no fragment, no
skeleton, no wheels), so this builds a fresh virtual segment around it:

  root (0x90, a stock console map model's): vtable, block map, shader
  group, skeleton 0, centre, box min/max (w: the console's NaN marker),
  LOD collections x4, LOD distances x4, each LOD's render buckets (bit n:
  a shader of bucket n, the bucket at byte +9 of a shader's header; unused
  LODs -1), radius.

Shaders the stock samples lack fall back (MAP_FALLBACK) until stock ones
come in (the v1.7.29 mapsets export).

Tiles (v1.7.32): a model hundreds of metres across is cut into tiles (each
a drawable of its own, its origin the middle of what it holds), as the game
neither draws nor collides with the island's big terrain models placed as
objects (v1.7.31: every piece over ~200 m across was missing, buildings near
the player were there). plan_tiles() picks the tiles from the model's own
triangles (by their centres on a square grid); MapDrawable(tile=...) keeps a
tile's triangles, moved to its origin.

usage: python3 -I mapdraw.py <out_dir> <shader sources dir> <template.xdr.bin:flags> <model.wdr> [...]
  shader sources: decompressed console drawables (<name>.xdr.bin / .xdd.bin
  / .xft.bin, each with <name>.log from xlzx unrsc holding its flags).
  Writes <out_dir>/<model>.xdr.bin and flags.txt lines.
  --cutout-from=<source dir>: an opaque shader over a plant's cut-out mask drawn cut out
  --edits=<set.json>: the set's "edit" (per model, "drop_textures": the
    geometries drawn with those colour maps left out)
"""
import glob
import os
import re
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import rsc5  # noqa: E402

VP, PP = batconv.VP, batconv.PP
NAN_W = 0x7F800001
ROOT_SIZE = 0x90
XDR_TYPE = 109

# The game's presets that are a sampled effect in another draw bucket (read
# off the PC files, v1.7.37: gta_cutout.sps is gta_default.fx in bucket 3,
# gta_alpha.sps the same in bucket 1, gta_decal.sps in bucket 2, and so on):
# made from that effect's console sample, its bucket (+9) and preset name
# changed. Bucket 3 is the cut-out pass (every cut-out preset is in it, the
# trees' too), where the game draws with the texture's alpha as a mask.
MAP_PRESETS = {
    'gta_cutout.sps': ('gta_default.sps', 3),
    'gta_alpha.sps': ('gta_default.sps', 1),
    'gta_normal_cutout.sps': ('gta_normal.sps', 3),
    'gta_normal_alpha.sps': ('gta_normal.sps', 1),
    'gta_normal_spec_alpha.sps': ('gta_normal_spec.sps', 1),
    'gta_spec_decal.sps': ('gta_spec.sps', 2),
}
# Map shaders with no stock console sample yet: the nearest that draws. The
# trees and fences as gta_cutout (v1.7.36's gta_default drew their leaves as
# solid cards: the Gator Keys' palm fronds, its monstera black); glass as
# gta_alpha.
MAP_FALLBACK = {
    'gta_emissivenight.sps': 'gta_emissive.sps',
    'gta_emissivestrong.sps': 'gta_emissive.sps',
    'gta_normal_spec_cubemap_reflect.sps': 'gta_normal_spec.sps',
    'gta_normal_spec_reflect.sps': 'gta_normal_spec.sps',
    'gta_cutout_fence.sps': 'gta_cutout.sps',
    'gta_trees.sps': 'gta_cutout.sps',
    'gta_normal_reflect.sps': 'gta_normal_spec.sps',
    'gta_reflect.sps': 'gta_spec.sps',
    'gta_spec_reflect.sps': 'gta_spec.sps',
    'gta_spec_const.sps': 'gta_spec.sps',
    'gta_glass.sps': 'gta_alpha.sps',
    'gta_glass_reflect.sps': 'gta_alpha.sps',
    'gta_decal_glue.sps': 'gta_decal.sps',
    'gta_terrain_va_3lyr.sps': 'gta_default.sps',
    'gta_terrain_va_4lyr.sps': 'gta_default.sps',
}
# Effects that draw both sides of a triangle (the mods' tree and fence
# cards are one-sided: Wake Island's 1,000-odd gta_trees geometries have no
# back faces, its gta_cutout palms have them all) standing in as one that
# culls the back: their triangles get back faces (double_sided).
TWO_SIDED = {'gta_trees.sps', 'gta_cutout_fence.sps'}
# An opaque shader whose colour map is a plant's cut-out mask
# (alphamask.cutouts) drawn as its cut-out preset.
AUTO_CUTOUT = {'gta_default.sps': 'gta_cutout.sps', 'gta_normal.sps': 'gta_normal_cutout.sps',
               'gta_spec.sps': 'gta_cutout.sps'}
COLOUR_SAMPLER = 0x2B5170FD
# The terrain shaders blend 3 or 4 texture layers by weights in the vertex:
# texcoord sets 0..N-1 are the layers' coordinates, then (0, w1) and
# (w2, w3) (the base layer's weight the rest). With no console sample of
# them, a terrain geometry is drawn as N plain ones: each triangle given to
# the layer weighing most at its corners, with that layer's texture and
# coordinates (Wake Island's ground: sand, dirt and scrub by area, the
# blends between them hard edges).
TERRAIN_SHADERS = {'gta_terrain_va_3lyr.sps': 3, 'gta_terrain_va_4lyr.sps': 4}
TERRAIN_LAYER_SAMPLERS = (0xD52B11DF, 0x2420AFD1, 0x31934AB6, 0x78B758FD)
TERRAIN_STAND_IN = 'gta_default.sps'
TINY_DEPTH = 60.0  # a carrier's one triangle this far under its origin


def load_console(path):
    """A decompressed console resource (with its xlzx log beside it)."""
    log = open(re.sub(r'\.bin$', '.log', path)).read()
    m = re.search(r'type=(\d+) flags=([0-9A-F]+)', log)
    return rsc5.Resource(open(path, 'rb').read(), int(m.group(2), 16), True, int(m.group(1)), path)


def drawables_of(r):
    """The drawables a console resource holds (xdr: one, xdd: a dictionary, xft: the fragment's)."""
    p = r.name
    if '.xdd' in p:
        vals, n = r.vptr(0x18), r.u16(0x1C)
        return [rsc5.read_drawable(r, r.vptr(vals + 4 * i)) for i in range(n) if r.vptr(vals + 4 * i) is not None]
    if '.xft' in p:
        d = rsc5.read_fragtype(r)['drawable']
        return [d] if d else []
    return [rsc5.read_drawable(r, 0)]


def pc_triangles(donor):
    """The drawable's first LOD as positions per triangle (n x 3 x 3), from
    the PC vertex and index buffers."""
    d = rsc5.read_drawable(donor, 0)
    tris = []
    for m in d['lods'][0]:
        for g in m['geometries']:
            at = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_PC)[0])[0]
            n = g['vertex_count'] or g['vb_count']
            base = g['vb_data'][1]
            pos = np.array([struct.unpack_from('<3f', donor.phys, base + v * g['vb_stride'] + at) for v in range(n)])
            idx = np.array(struct.unpack_from('<%dH' % g['ib_count'], donor.phys, g['ib_data'][1]), np.int64)
            tris.append(pos[idx.reshape(-1, 3)])
    return np.concatenate(tris) if tris else np.zeros((0, 3, 3))


def lifted(points, lift):
    """Points (... x 3) with the vertical moves a lift table gives them
    (plant.shifts: rounded position -> dz; a point not in it stays)."""
    if not lift:
        return points
    flat = points.reshape(-1, 3).astype(np.float32)
    out = flat.copy()
    for i, p in enumerate(flat):
        dz = lift.get((round(float(p[0]), 3), round(float(p[1]), 3), round(float(p[2]), 3)))
        if dz is not None:
            out[i, 2] = np.float32(float(p[2]) + dz)
    return out.reshape(points.shape).astype(points.dtype)


def lift_geometry(c, lift):
    """A converted geometry with its positions lifted (lifted())."""
    at = dict(batconv.vertex_layout(c['usage'], batconv.SIZE_360)[0])[0]
    stride, n = c['stride'], c['vcount']
    out = bytearray(c['vdata'])
    moved = 0
    for v in range(n):
        x, y, z = struct.unpack_from('>3f', out, v * stride + at)
        dz = lift.get((round(x, 3), round(y, 3), round(z, 3)))
        if dz is not None:
            struct.pack_into('>f', out, v * stride + at + 8, float(np.float32(z + dz)))
            moved += 1
    return dict(c, vdata=bytes(out)), moved


def plan_tiles(donor, cell, zmin=None, lift=None):
    """The model's tiles: a square grid of `cell` metres over its triangles'
    centres (from its box's corner); each cell holding some (the highest
    corner of each at or over `zmin`, model space, when given) is a tile:
    (ix, iy), the cell's box, its origin (the middle of its triangles' box).
    `lift`: the plants' moves (plant.shifts), applied first."""
    tri = lifted(pc_triangles(donor), lift)
    if zmin is not None:
        tri = tri[tri[:, :, 2].max(axis=1) >= zmin]
    cen = tri.mean(axis=1)
    x0, y0 = cen[:, 0].min(), cen[:, 1].min()
    ix = np.floor((cen[:, 0] - x0) / cell).astype(np.int64)
    iy = np.floor((cen[:, 1] - y0) / cell).astype(np.int64)
    tiles = []
    for key in sorted(set(zip(ix.tolist(), iy.tolist()))):
        sel = (ix == key[0]) & (iy == key[1])
        pts = tri[sel].reshape(-1, 3)
        lo, hi = pts.min(axis=0), pts.max(axis=0)
        tiles.append(dict(cell=key, lo=(x0 + key[0] * cell, y0 + key[1] * cell),
                          hi=(x0 + (key[0] + 1) * cell, y0 + (key[1] + 1) * cell),
                          origin=tuple(float(x) for x in (lo + hi) / 2), zmin=zmin, triangles=int(sel.sum())))
    return tiles


def tile_geometry(c, tile):
    """A converted geometry cut down to the tile's triangles (their centres in
    its cell, their highest corner over its zmin), moved to its origin; None
    when it has none there."""
    at = dict(batconv.vertex_layout(c['usage'], batconv.SIZE_360)[0])[0]
    stride, n = c['stride'], c['vcount']
    pos = np.array([struct.unpack_from('>3f', c['vdata'], v * stride + at) for v in range(n)])
    idx = np.array(struct.unpack('>%dH' % c['icount'], c['idata']), np.int64).reshape(-1, 3)
    cen = pos[idx].mean(axis=1)
    keep = ((cen[:, 0] >= tile['lo'][0]) & (cen[:, 0] < tile['hi'][0]) &
            (cen[:, 1] >= tile['lo'][1]) & (cen[:, 1] < tile['hi'][1]))
    if tile.get('zmin') is not None:
        keep &= pos[idx][:, :, 2].max(axis=1) >= tile['zmin']
    if not keep.any():
        return None
    tri = idx[keep]
    used = np.unique(tri)
    remap = np.full(n, -1, np.int64)
    remap[used] = np.arange(len(used))
    ox, oy, oz = tile['origin']
    out = bytearray()
    for v in used:
        rec = bytearray(c['vdata'][v * stride:(v + 1) * stride])
        x, y, z = struct.unpack_from('>3f', rec, at)
        struct.pack_into('>3f', rec, at, x - ox, y - oy, z - oz)
        out += rec
    new_idx = remap[tri].reshape(-1)
    idata = struct.pack('>%dH' % len(new_idx), *new_idx.tolist())
    return dict(c, vdata=bytes(out), vbytes=len(out), vcount=len(used), idata=idata, ibytes=len(idata),
                icount=len(new_idx), bones=list(c['bones']))


def half_floats(raw):
    """Little arrays of half floats (big-endian u16) as float32."""
    h = raw.astype(np.uint32)
    s, e, m = (h >> 15) & 1, (h >> 10) & 0x1F, h & 0x3FF
    v = np.where(e == 0, m / 1024.0 * 2.0 ** -14, (1 + m / 1024.0) * 2.0 ** (e.astype(np.int32) - 15))
    return np.where(s == 1, -v, v).astype(np.float32)


def terrain_layers(d, soff):
    """A PC terrain shader's layer textures, in order (None for a slot it does not name)."""
    by_hash = {p['hash']: p for p in batconv.shader_params(d, soff)}
    out = []
    for h in TERRAIN_LAYER_SAMPLERS:
        p = by_hash.get(h)
        name = None
        if p and p['type'] == 0 and p['value'] is not None and d.vptr(p['value'] + 20) is not None:
            name = d.cstr(d.vptr(p['value'] + 20))
        out.append(name)
    while out and out[-1] is None:
        out.pop()
    return out


def split_terrain(c, layers):
    """A converted terrain geometry as one plain geometry per layer: the
    triangles whose corners weigh that layer most, with its coordinates in
    texcoord 0; (layer index, geometry) for each layer that got some."""
    at = dict(batconv.vertex_layout(c['usage'], batconv.SIZE_360)[0])
    stride, n, k = c['stride'], c['vcount'], len(layers)
    raw = np.frombuffer(c['vdata'], np.uint8).reshape(n, stride)

    def uv(set_):
        e = 6 + set_
        assert e in at, 'terrain geometry without texcoord set %d' % set_
        return half_floats(raw[:, at[e]:at[e] + 4].copy().view('>u2'))
    w = [uv(k)[:, 1]]  # (0, w1)
    for i in range(2, k):
        w.append(uv(k + 1)[:, (i - 2) % 2])  # (w2, w3)
    w = np.stack(w, 1)[:, :k - 1]
    w = np.concatenate([np.clip(1.0 - w.sum(1, keepdims=True), 0.0, 1.0), w], 1)
    idx = np.array(struct.unpack('>%dH' % c['icount'], c['idata']), np.int64).reshape(-1, 3)
    choice = w[idx].mean(axis=1).argmax(axis=1)
    out = []
    for layer in range(k):
        tri = idx[choice == layer]
        if not len(tri) or layers[layer] is None:
            continue
        used = np.unique(tri)
        remap = np.full(n, -1, np.int64)
        remap[used] = np.arange(len(used))
        v = raw[used].copy()
        v[:, at[6]:at[6] + 4] = raw[used][:, at[6 + layer]:at[6 + layer] + 4]
        new_idx = remap[tri].reshape(-1)
        idata = struct.pack('>%dH' % len(new_idx), *new_idx.tolist())
        out.append((layer, dict(c, vdata=v.tobytes(), vbytes=int(v.size), vcount=len(used), idata=idata,
                                ibytes=len(idata), icount=len(new_idx), bones=list(c['bones']))))
    return out


def tiny_geometry(c, at_z=TINY_DEPTH):
    """A converted geometry cut down to one triangle a millimetre across,
    `at_z` below the origin: a carrier's drawable (mapset.py), there only
    because an object needs a model with one."""
    at = dict(batconv.vertex_layout(c['usage'], batconv.SIZE_360)[0])[0]
    stride = c['stride']
    idx = struct.unpack_from('>3H', c['idata'], 0)
    out = bytearray()
    for k, v in enumerate(idx):
        rec = bytearray(c['vdata'][v * stride:(v + 1) * stride])
        struct.pack_into('>3f', rec, at, 0.001 if k == 1 else 0.0, 0.001 if k == 2 else 0.0, -at_z)
        out += rec
    idata = struct.pack('>3H', 0, 1, 2)
    return dict(c, vdata=bytes(out), vbytes=len(out), vcount=3, idata=idata, ibytes=len(idata), icount=3,
                bones=list(c['bones']))


def flip_dec3n(w):
    """A packed 10:10:10 vector turned round (each part negated, -512 kept at -511)."""
    out = w & 0xC0000000
    for sh in (0, 10, 20):
        q = (w >> sh) & 0x3FF
        q = q - 1024 if q >= 512 else q
        q = max(-511, min(511, -q))
        out |= (q & 0x3FF) << sh
    return out


def back_faces(c):
    """A converted geometry's back: its vertices with the normal and the
    binormal turned round, its triangles wound the other way."""
    at = dict(batconv.vertex_layout(c['usage'], batconv.SIZE_360)[0])
    stride, n = c['stride'], c['vcount']
    back = bytearray(c['vdata'])
    for e in (3, 15):  # (the normal, the binormal; the tangent runs along the texture as before)
        if e not in at:
            continue
        for v in range(n):
            o = v * stride + at[e]
            struct.pack_into('>I', back, o, flip_dec3n(struct.unpack_from('>I', back, o)[0]))
    idx = np.array(struct.unpack('>%dH' % c['icount'], c['idata']), np.int64).reshape(-1, 3)[:, [0, 2, 1]]
    idata = struct.pack('>%dH' % idx.size, *idx.reshape(-1).tolist())
    return dict(c, vdata=bytes(back), idata=idata, bones=list(c['bones']))


class MapDrawable(batconv.Converter):
    def __init__(self, tmpl, donor, sources, log=print, tile=None, tiny=False, bounds=None, lift=None, cutout=None,
                 drop=None):
        """`tile`: one of plan_tiles' (its triangles only, moved to its
        origin). `tiny`: the donor's first geometry as one tiny triangle
        (tiny_geometry). `bounds`: (lo, hi, centre, radius) written as the
        drawable's instead of the donor's. `lift`: the plants' vertical moves
        (plant.shifts), applied to the positions before anything else.
        `cutout`: the colour maps that are plants' cut-out masks
        (alphamask.cutouts): an opaque shader using one drawn cut out.
        `drop`: colour-map names (lower case) whose geometries are left out
        (a set's edits: Benny's one-sided sign, v1.7.38)."""
        self.tile = tile
        self.drop = {n.lower() for n in (drop or ())}
        self.dropped = []         # triangles left out per LOD (drop)
        self.tiny = tiny
        self.bounds = bounds
        self.lift = lift
        self.lifted = 0
        self.cutout = {n.lower() for n in (cutout or ())}
        self.auto_cutouts = []    # (the donor's shaders drawn cut out for their texture)
        self.two_sided = set()    # (the donor's shaders given back faces)
        self.added = []           # triangles added per LOD (back faces)
        self.kept = []  # (a tile: its vertices' positions, for its bounds)
        self.terrain = {}  # a terrain shader's index: the plain shaders standing in for its layers
        self.terrain_split = 0
        self.t = tmpl
        self.d = donor
        self.log = log
        self.tf = {'drawable': rsc5.read_drawable(tmpl, 0), 'children': []}
        self.df = {'drawable': rsc5.read_drawable(donor, 0), 'children': []}
        root = bytearray(tmpl.virt[:ROOT_SIZE])
        struct.pack_into('>I', root, 4, VP | ROOT_SIZE)
        self.v = batconv.Seg(bytes(root) + b'\x00\x00\x00\x00' + b'\xCD' * 0x0C, batconv.VIRTUAL_PAGE)
        self.p = batconv.Seg()
        self.decls = {}
        self.uv_filled = 0
        self.zero_vectors = 0
        self.texture_names = set()
        self.options = {}
        self.bone_map = {}
        self.face_rules, self.split_rules = [], []
        self.donor_bones = []
        self.pivots = {}
        # every source foreign (its strings copied in): the fresh segment holds none of the template's
        self.shader_sources = [load_console(s.name) for s in sources]
        self._templates()

    def _templates(self):
        t = self.t
        d = self.tf['drawable']
        g0 = d['lods'][0][0]['geometries'][0]
        self.t_geom = t.virt[g0['off']:g0['off'] + 0x50]
        self.t_vb = t.virt[g0['vb_off']:g0['vb_off'] + 0x40]
        self.t_ib = t.virt[g0['ib_off']:g0['ib_off'] + 0x30]
        decl = t.vptr(g0['vb_off'] + 16)
        self.t_decl = t.virt[decl:decl + 16]
        m0 = d['lods'][0][0]['off']
        self.t_model_main = t.virt[m0:m0 + 0x20]
        coll = batconv.lod_collections(t, d['off'])[0]
        self.t_coll = t.virt[coll:coll + 8]
        sg = d['shader_group']['off']
        self.t_sg = t.virt[sg:sg + 0x50]
        self.t_shaders = {}
        for res in self.shader_sources:
            for rd in drawables_of(res):
                if not rd or not rd['shader_group']:
                    continue
                rsg = rd['shader_group']['off']
                usage_arr, extra_arr = res.vptr(rsg + 64), res.vptr(rsg + 72)
                for i, s in enumerate(rd['shader_group']['shaders']):
                    sps = batconv.shader_sps(res, s['off'])
                    self.t_shaders.setdefault(sps, dict(res=res, off=s['off'], usage=res.u32(usage_arr + 4 * i),
                                                        extra=res.u32(extra_arr + 4 * i)))
        for preset, (base, bucket) in MAP_PRESETS.items():  # (the effect's sample in another bucket)
            if base in self.t_shaders and preset not in self.t_shaders:
                self.t_shaders[preset] = dict(self.t_shaders[base], bucket=bucket, sps_name=preset)
        self.t_texobj = None
        for s in d['shader_group']['shaders']:
            for p in batconv.shader_params(t, s['off']):
                if p['type'] == 0 and p['value'] is not None and \
                        struct.unpack_from('>I', t.virt, p['value'])[0] == batconv.TEXTURE_REFERENCE_ID:
                    self.t_texobj = t.virt[p['value']:p['value'] + 32]
                    break
            if self.t_texobj:
                break
        assert self.t_texobj, 'the template names no texture by reference'
        assert struct.unpack_from('>I', self.t_vb, 28)[0] == VP | (g0['vb_off'] + 0x20)
        assert struct.unpack_from('>I', self.t_ib, 12)[0] == VP | (g0['ib_off'] + 0x10)

    def colour_map(self, soff):
        """The donor shader's colour map's name (lower case, no pack:/ or .dds), or None."""
        for p in batconv.shader_params(self.d, soff):
            if p['hash'] == COLOUR_SAMPLER and p['type'] == 0 and p['value'] is not None:
                np_ = self.d.vptr(p['value'] + 20)
                if np_ is None:
                    return None
                name = self.d.cstr(np_).split('/')[-1].lower()
                return name[:-4] if name.endswith('.dds') else name
        return None

    def build_shader_group(self):
        """The vehicle converter's, without its per-car reordering, and a
        terrain shader cloned once per layer (split_terrain). Geometries'
        shader indices are the new group's from here on (shader_slot empty)."""
        d = self.d
        dsg = self.df['drawable']['shader_group']
        new_shaders, usages, extras = [], [], []
        self.shader_report = []
        self.shader_slot = {}  # the donor's index -> the first of its own in the new group
        for si, s in enumerate(dsg['shaders']):
            sps = batconv.shader_sps(d, s['off'])
            self.shader_slot[si] = len(new_shaders)
            if sps in TERRAIN_SHADERS and TERRAIN_STAND_IN in self.t_shaders:
                layers = terrain_layers(d, s['off'])
                assert len(layers) == TERRAIN_SHADERS[sps], (sps, layers)
                ts = self.t_shaders[TERRAIN_STAND_IN]
                self.terrain[si] = []
                for name in layers:
                    self.terrain[si].append(len(new_shaders))
                    new_shaders.append(self._clone_shader(ts, s['off'], {batconv.SAMPLER_TEXTURE: name}))
                    usages.append(ts['usage'])
                    extras.append(ts['extra'])
                self.shader_report.append((sps, '%s x%d (its layers: %s)' % (TERRAIN_STAND_IN, len(layers),
                                                                              ', '.join(layers))))
                continue
            tsps = sps if sps in self.t_shaders else batconv.SPS_FALLBACK.get(sps)
            assert tsps in self.t_shaders, 'no template shader for %s' % sps
            colour = self.colour_map(s['off'])
            if tsps in AUTO_CUTOUT and colour in self.cutout and AUTO_CUTOUT[tsps] in self.t_shaders:
                self.auto_cutouts.append('%s (%s)' % (colour, sps))
                tsps = AUTO_CUTOUT[tsps]
            if sps in TWO_SIDED and sps not in self.t_shaders:
                self.two_sided.add(si)
            ts = self.t_shaders[tsps]
            new_shaders.append(self._clone_shader(ts, s['off']))
            usages.append(ts['usage'])
            extras.append(ts['extra'])
            self.shader_report.append((sps, tsps if ts['res'] is self.t else '%s (from %s)'
                                       % (tsps, ts['res'].name.split('/')[-1])))
        self.donor_slot = dict(self.shader_slot)
        self.shader_slot = {}
        self.shader_usage = list(usages)
        n = len(new_shaders)
        v = self.v
        sg = v.put(self.t_sg)
        arr = v.alloc(4 * n)
        for i, so in enumerate(new_shaders):
            v.w32(arr + 4 * i, VP | so)
        v.w32(sg + 8, VP | arr)
        v.w16(sg + 12, n)
        v.w16(sg + 14, n)
        for k, values in ((64, usages), (72, extras)):
            a = v.alloc(4 * n)
            for i, val in enumerate(values):
                v.w32(a + 4 * i, val)
            v.w32(sg + k, VP | a)
            v.w16(sg + k + 4, n)
            v.w16(sg + k + 6, n)
        return sg

    def convert_drawable_lods(self, ddraw):
        """As the vehicle converter's, a terrain geometry split by layer, a
        tile keeping only its own triangles (a geometry, model or LOD left
        with none dropped)."""
        out = []
        for coll in batconv.lod_collections(self.d, ddraw['off']):
            if coll is None or (self.tiny and out):
                out.append(None)
                continue
            lod = []
            for m in batconv.model_collection(self.d, coll):
                gs = [g for g in rsc5.ptr_collection(self.d, m + 4) if g is not None]
                smap_p = self.d.vptr(m + 16)
                smap = [self.d.u16(smap_p + 2 * k) for k in range(len(gs))]
                geoms, shaders = [], []
                for g, si in zip(gs, smap):
                    if self.drop and self.colour_map(self.df['drawable']['shader_group']['shaders'][si]['off']) in self.drop:
                        while len(self.dropped) <= len(out):
                            self.dropped.append(0)
                        self.dropped[len(out)] += self.d.u32(g + 44) // 3
                        continue
                    first = self.donor_slot[si]
                    whole = self.convert_geometry(rsc5.read_geometry(self.d, g), self.shader_usage[first])
                    if self.tiny:
                        out.append([([tiny_geometry(whole)], [first])])
                        return out
                    if self.lift:
                        whole, moved = lift_geometry(whole, self.lift)
                        self.lifted += moved
                    if si in self.terrain:
                        pieces = [(self.terrain[si][layer], part, False)
                                  for layer, part in split_terrain(whole, terrain_layers(self.d, self.df['drawable']['shader_group']['shaders'][si]['off']))]
                        self.terrain_split += 1
                    else:
                        pieces = [(first, whole, False)]
                        if si in self.two_sided:  # (its back faces, a geometry of their own)
                            pieces.append((first, back_faces(whole), True))
                    for slot, piece, back in pieces:
                        if self.tile:
                            piece = tile_geometry(piece, self.tile)
                            if piece is None:
                                continue
                            at = dict(batconv.vertex_layout(piece['usage'], batconv.SIZE_360)[0])[0]
                            self.kept += [struct.unpack_from('>3f', piece['vdata'], k * piece['stride'] + at)
                                          for k in range(piece['vcount'])]
                        if back:
                            while len(self.added) <= len(out):
                                self.added.append(0)
                            self.added[len(out)] += piece['icount'] // 3
                        parts = batconv.split_geometry(piece)
                        if len(parts) > 1:
                            self.log('geometry of %d vertices (%d KiB) drawn as %d parts of up to %d vertices'
                                     % (piece['vcount'], piece['vbytes'] >> 10, len(parts),
                                        max(p['vcount'] for p in parts)))
                        geoms += parts
                        shaders += [slot] * len(parts)
                if geoms:
                    lod.append((geoms, shaders))
            out.append(lod or None)
        return out

    def tile_bounds(self):
        """The tile's box, centre and radius (its own space)."""
        p = np.array(self.kept)
        lo, hi = p.min(axis=0), p.max(axis=0)
        centre = (lo + hi) / 2
        return lo, hi, centre, float(np.linalg.norm(p - centre, axis=1).max())

    def run(self):
        v = self.v
        sg = self.build_shader_group()
        arr = struct.unpack_from('>I', v.buf, sg + 8)[0] & 0x0FFFFFFF
        n_shaders = struct.unpack_from('>H', v.buf, sg + 12)[0]
        shader_offs = [struct.unpack_from('>I', v.buf, arr + 4 * i)[0] & 0x0FFFFFFF for i in range(n_shaders)]
        buckets = [v.buf[o + 9] for o in shader_offs]  # (in the slots' order)
        lods = self.convert_drawable_lods(self.df['drawable'])
        self.check_shader_inputs(lods)
        self.place_buffers(lods)
        struct.pack_into('>II', v.buf, 8, VP | sg, 0)
        masks = []
        for slot in range(4):
            lod = lods[slot] if slot < len(lods) else None
            if lod:
                struct.pack_into('>I', v.buf, 0x40 + 4 * slot, VP | self.emit_lod(lod, self.t_model_main))
                mask = 0
                for _, smap in lod:
                    for si in smap:
                        mask |= 1 << buckets[self.shader_slot.get(si, si)]
                masks.append(mask)
            else:
                struct.pack_into('>I', v.buf, 0x40 + 4 * slot, 0)
                masks.append(0xFFFFFFFF)
        struct.pack_into('>4I', v.buf, 0x60, *masks)
        d = self.d
        for k in (0x10, 0x20, 0x30):
            x, y, z = struct.unpack_from('<3f', d.virt, k)
            struct.pack_into('>3fI', v.buf, k, x, y, z, NAN_W)
        struct.pack_into('>4f', v.buf, 0x50, *struct.unpack_from('<4f', d.virt, 0x50))
        struct.pack_into('>f', v.buf, 0x70, struct.unpack_from('<f', d.virt, 0x70)[0])
        if self.tile or self.bounds:  # (a tile's own bounds, or the given ones)
            lo, hi, centre, radius = self.bounds or self.tile_bounds()
            for k, vec in ((0x10, centre), (0x20, lo), (0x30, hi)):
                struct.pack_into('>3fI', v.buf, k, *[float(x) for x in vec], NAN_W)
            struct.pack_into('>f', v.buf, 0x70, float(radius))
        flags, vtot, ptot = batconv.make_flags(self.t.flags, len(v.buf), self.p_count, self.p_shift)
        assert ptot == len(self.p.buf)
        virt = bytes(v.buf) + b'\0' * (vtot - len(v.buf))
        return flags, virt + bytes(self.p.buf)


def set_edits(path):
    """A set's edits ({model: {colour maps whose geometries are left out}})."""
    if not path:
        return {}
    import json
    return {m.lower(): {t.lower() for t in e.get('drop_textures', [])}
            for m, e in json.load(open(path)).get('edit', {}).items()}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    cutout_dir = next((a[len('--cutout-from='):] for a in sys.argv[1:] if a.startswith('--cutout-from=')), None)
    edits_path = next((a[len('--edits='):] for a in sys.argv[1:] if a.startswith('--edits=')), None)
    out_dir, src_dir, tmpl_arg = args[:3]
    tmpl_path, tmpl_flags = tmpl_arg.rsplit(':', 1)
    tmpl = rsc5.Resource(open(tmpl_path, 'rb').read(), int(tmpl_flags, 16), True, XDR_TYPE, tmpl_path)
    sources = [load_console(p) for p in sorted(glob.glob(os.path.join(src_dir, '*.bin')))]
    for k, fb in MAP_FALLBACK.items():
        batconv.SPS_FALLBACK.setdefault(k, fb)
    cutout = set()
    if cutout_dir:
        import alphamask
        cutout = alphamask.cutouts(cutout_dir)
        print('cut-out masks of plants (alphamask.py): %d textures' % len(cutout))
    edits = set_edits(edits_path)
    os.makedirs(out_dir, exist_ok=True)
    flags_out = open(os.path.join(out_dir, 'flags.txt'), 'a')
    for path in args[3:]:
        name = os.path.basename(path)[:-4]
        donor = rsc5.Resource.pc(path)
        conv = MapDrawable(tmpl, donor, sources, log=lambda s, n=name: print('[%s] %s' % (n, s)), cutout=cutout,
                           drop=edits.get(name.lower()))
        flags, body = conv.run()
        verdict = check(body, flags, donor, added=conv.added, dropped=conv.dropped)
        open(os.path.join(out_dir, name + '.xdr.bin'), 'wb').write(body)
        flags_out.write('%s.xdr %d 0x%08X\n' % (name, XDR_TYPE, flags))
        fallbacks = sorted({'%s as %s' % (a, b.split(' ')[0]) for a, b in conv.shader_report if a not in conv.t_shaders})
        embedded = getattr(conv, 'embedded_replaced', 0)
        print('%s: %d bytes, flags %08X, textures %d, %s%s%s%s' % (
            name, len(body), flags, len(conv.texture_names), verdict,
            ('; shaders standing in: ' + ', '.join(fallbacks)) if fallbacks else '',
            ('; cut out for their texture: ' + ', '.join(conv.auto_cutouts)) if conv.auto_cutouts else '',
            ('; %d sample textures embedded, named by reference instead' % embedded) if embedded else ''))



def check(body, flags, donor, lift=None, added=(), dropped=()):
    """The console drawable read back: per LOD the same triangles as the
    mod's (count), every index inside its buffer, every position decoding
    to one of the mod's (exactly: positions are stored as floats on both;
    with a lift, one of the mod's moved as the lift says), every texture
    reference named."""
    x = rsc5.Resource(body, flags, True, XDR_TYPE, 'out')
    xd, dd = rsc5.read_drawable(x, 0), rsc5.read_drawable(donor, 0)
    assert len(xd['lods']) == len(dd['lods']), (len(xd['lods']), len(dd['lods']))
    report = []
    for k, (xl, dl) in enumerate(zip(xd['lods'], dd['lods'])):
        tri_x = sum(g['index_count'] // 3 for m in xl for g in m['geometries'])
        tri_d = sum(g['index_count'] // 3 for m in dl for g in m['geometries'])
        extra = added[k] if k < len(added) else 0  # (back faces given to two-sided shaders' triangles)
        gone = dropped[k] if k < len(dropped) else 0  # (a set's edits: geometries left out)
        assert tri_x == tri_d + extra - gone, ('triangles', tri_x, tri_d, extra, gone)
        pc_pos = set()
        for m in dl:
            for g in m['geometries']:
                at = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_PC)[0])
                n = g['vertex_count'] or g['vb_count']
                base = g['vb_data'][1]
                pts = np.array([struct.unpack_from('<3f', donor.phys, base + v * g['vb_stride'] + at[0])
                                for v in range(n)], np.float64)
                for p in lifted(pts, lift) if len(pts) else pts:
                    pc_pos.add((float(p[0]), float(p[1]), float(p[2])))
        checked = 0
        for m in xl:
            for g in m['geometries']:
                n = g['vertex_count'] or g['vb_count']
                ib = g['ib_data'][1]
                idx = struct.unpack_from('>%dH' % g['index_count'], x.phys, ib)
                assert max(idx) < n, ('index out of range', max(idx), n)
                at = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_360)[0])
                base = g['vb_data'][1]
                for v in range(0, n, max(1, n // 200)):
                    p = struct.unpack_from('>3f', x.phys, base + v * g['vb_stride'] + at[0])
                    assert p in pc_pos, ('position not in the mod', p)
                    checked += 1
                # (every normal, tangent and binormal a direction: a zero one
                # is drawn black by the normal-mapped shaders - v1.7.35)
                for e in (3, 14, 15):
                    if e not in at:
                        continue
                    for v in range(n):
                        w = struct.unpack_from('>I', x.phys, base + v * g['vb_stride'] + at[e])[0]
                        parts = [((w >> sh) & 0x3FF) for sh in (0, 10, 20)]
                        parts = [q - 1024 if q >= 512 else q for q in parts]
                        assert sum(q * q for q in parts) > 450 * 450, ('zero or short vector', e, v, parts)
        report.append('%d triangles%s%s, %d positions checked' % (tri_x, (' (%d back faces)' % extra) if extra else '',
                                                                  (' (%d left out)' % gone) if gone else '', checked))
    return '; '.join(report)


if __name__ == '__main__':
    main()
