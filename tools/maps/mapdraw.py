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
import batconv  # noqa: E402
import rsc5  # noqa: E402

VP, PP = batconv.VP, batconv.PP
NAN_W = 0x7F800001
ROOT_SIZE = 0x90
XDR_TYPE = 109

# Map shaders with no stock console sample yet: the nearest that draws.
MAP_FALLBACK = {
    'gta_emissivenight.sps': 'gta_emissive.sps',
    'gta_normal_spec_cubemap_reflect.sps': 'gta_normal_spec.sps',
    'gta_normal_cutout.sps': 'gta_normal.sps',
    'gta_cutout_fence.sps': 'gta_default.sps',
    'gta_trees.sps': 'gta_default.sps',
    'gta_cutout.sps': 'gta_default.sps',
    'gta_normal_reflect.sps': 'gta_normal_spec.sps',
    'gta_reflect.sps': 'gta_spec.sps',
}


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


def plan_tiles(donor, cell, zmin=None):
    """The model's tiles: a square grid of `cell` metres over its triangles'
    centres (from its box's corner); each cell holding some (the highest
    corner of each at or over `zmin`, model space, when given) is a tile:
    (ix, iy), the cell's box, its origin (the middle of its triangles' box)."""
    tri = pc_triangles(donor)
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


class MapDrawable(batconv.Converter):
    def __init__(self, tmpl, donor, sources, log=print, tile=None):
        self.tile = tile
        self.kept = []  # (a tile: its vertices' positions, for its bounds)
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
        self.t_texobj = None
        for s in d['shader_group']['shaders']:
            for p in batconv.shader_params(t, s['off']):
                if p['type'] == 0 and p['value'] is not None:
                    self.t_texobj = t.virt[p['value']:p['value'] + 32]
                    break
            if self.t_texobj:
                break
        assert struct.unpack_from('>I', self.t_vb, 28)[0] == VP | (g0['vb_off'] + 0x20)
        assert struct.unpack_from('>I', self.t_ib, 12)[0] == VP | (g0['ib_off'] + 0x10)

    def convert_drawable_lods(self, ddraw):
        """As the vehicle converter's, a tile keeping only its own triangles
        (a geometry, model or LOD left with none dropped)."""
        if not self.tile:
            return super().convert_drawable_lods(ddraw)
        out = []
        for coll in batconv.lod_collections(self.d, ddraw['off']):
            if coll is None:
                out.append(None)
                continue
            lod = []
            for m in batconv.model_collection(self.d, coll):
                gs = [g for g in rsc5.ptr_collection(self.d, m + 4) if g is not None]
                smap_p = self.d.vptr(m + 16)
                smap = [self.d.u16(smap_p + 2 * k) for k in range(len(gs))]
                geoms, shaders = [], []
                for g, si in zip(gs, smap):
                    whole = self.convert_geometry(rsc5.read_geometry(self.d, g), self.shader_usage[si])
                    whole = tile_geometry(whole, self.tile)
                    if whole is None:
                        continue
                    at = dict(batconv.vertex_layout(whole['usage'], batconv.SIZE_360)[0])[0]
                    self.kept += [struct.unpack_from('>3f', whole['vdata'], k * whole['stride'] + at)
                                  for k in range(whole['vcount'])]
                    parts = batconv.split_geometry(whole)
                    geoms += parts
                    shaders += [si] * len(parts)
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
        if self.tile:  # (a tile's own bounds)
            lo, hi, centre, radius = self.tile_bounds()
            for k, vec in ((0x10, centre), (0x20, lo), (0x30, hi)):
                struct.pack_into('>3fI', v.buf, k, *[float(x) for x in vec], NAN_W)
            struct.pack_into('>f', v.buf, 0x70, radius)
        flags, vtot, ptot = batconv.make_flags(self.t.flags, len(v.buf), self.p_count, self.p_shift)
        assert ptot == len(self.p.buf)
        virt = bytes(v.buf) + b'\0' * (vtot - len(v.buf))
        return flags, virt + bytes(self.p.buf)


def main():
    out_dir, src_dir, tmpl_arg = sys.argv[1:4]
    tmpl_path, tmpl_flags = tmpl_arg.rsplit(':', 1)
    tmpl = rsc5.Resource(open(tmpl_path, 'rb').read(), int(tmpl_flags, 16), True, XDR_TYPE, tmpl_path)
    sources = [load_console(p) for p in sorted(glob.glob(os.path.join(src_dir, '*.bin')))]
    for k, fb in MAP_FALLBACK.items():
        batconv.SPS_FALLBACK.setdefault(k, fb)
    os.makedirs(out_dir, exist_ok=True)
    flags_out = open(os.path.join(out_dir, 'flags.txt'), 'a')
    for path in sys.argv[4:]:
        name = os.path.basename(path)[:-4]
        donor = rsc5.Resource.pc(path)
        conv = MapDrawable(tmpl, donor, sources, log=lambda s, n=name: print('[%s] %s' % (n, s)))
        flags, body = conv.run()
        verdict = check(body, flags, donor)
        open(os.path.join(out_dir, name + '.xdr.bin'), 'wb').write(body)
        flags_out.write('%s.xdr %d 0x%08X\n' % (name, XDR_TYPE, flags))
        fallbacks = sorted({a for a, _ in conv.shader_report if a not in conv.t_shaders})
        print('%s: %d bytes, flags %08X, textures %d, %s%s' % (name, len(body), flags, len(conv.texture_names), verdict,
                                                          ('; shaders standing in: ' + ', '.join(fallbacks)) if fallbacks else ''))



def check(body, flags, donor):
    """The console drawable read back: per LOD the same triangles as the
    mod's (count), every index inside its buffer, every position decoding
    to one of the mod's (exactly: positions are stored as floats on both),
    every texture reference named."""
    x = rsc5.Resource(body, flags, True, XDR_TYPE, 'out')
    xd, dd = rsc5.read_drawable(x, 0), rsc5.read_drawable(donor, 0)
    assert len(xd['lods']) == len(dd['lods']), (len(xd['lods']), len(dd['lods']))
    report = []
    for xl, dl in zip(xd['lods'], dd['lods']):
        tri_x = sum(g['index_count'] // 3 for m in xl for g in m['geometries'])
        tri_d = sum(g['index_count'] // 3 for m in dl for g in m['geometries'])
        assert tri_x == tri_d, ('triangles', tri_x, tri_d)
        pc_pos = set()
        for m in dl:
            for g in m['geometries']:
                at = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_PC)[0])
                n = g['vertex_count'] or g['vb_count']
                base = g['vb_data'][1]
                for v in range(n):
                    pc_pos.add(struct.unpack_from('<3f', donor.phys, base + v * g['vb_stride'] + at[0]))
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
        report.append('%d triangles, %d positions checked' % (tri_x, checked))
    return '; '.join(report)


if __name__ == '__main__':
    main()
