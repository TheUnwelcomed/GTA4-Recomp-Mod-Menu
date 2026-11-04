"""Writes GTA IV PC resources from plain data: drawables (.wdr, type 110)
and texture dictionaries (.wtd, type 8), laid out as the game's own PC files
are (SparkIV's layouts, checked against mods' files), so that everything that
reads a map mod's PC files (rsc5.py, batconv.py, maps/mapdraw.py, maptex.py,
alphamask.py) takes them unchanged. The way in for models from elsewhere: a
GTA V file read by maps/v_reader.py, or a mesh made here (a sign, a hut).

A drawable's shaders are cloned from PC samples of the same .sps (any mod's
.wdr with that shader: the parameter block kept whole, the textures and the
vector values given replaced), as the console converter clones the stock
console ones.

    lib = ShaderLibrary([paths of .wdr files])      # samples by .sps name
    d = Drawable(lib)
    s = d.shader('gta_default.sps', {SAMPLER_TEXTURE: 'mytex'})
    d.geometry(0, s, positions, normals, colours, [uvs], triangles)
    d.write('out/mymodel.wdr')
    Dictionary({'mytex': (w, h, 'DXT5', [level bytes...])}).write('out/mymodel.wtd')

Vertices: positions float3, normals float3, colours 4 bytes (B, G, R, A as
D3DCOLOR), texture coordinates float2; the PC vertex layout by usage bits
(batconv.vertex_layout(usage, SIZE_PC)); triangle lists of 16-bit indices.
"""
import os
import struct
import zlib

import numpy as np

import batconv
import rsc5

VP, PP = batconv.VP, batconv.PP
RSC_MAGIC = rsc5.RSC_MAGIC
TYPE_WDR, TYPE_WTD = 110, 8
NAN_W = 0xFFC00000
VFT = dict(drawable=0xB8AAF60D, shader_group=0x939CC343, shader=0xF44D580E, texture_ref=0x096FBA15,
           model=0x8BDEF787, geometry=0xEFE7D461, vertex_buffer=0x5CB234A6, index_buffer=0x8CA91DA9,
           dictionary=0x61D2BFEB, texture=0xB6F0EE97)
FOURCC = {'DXT1': b'DXT1', 'DXT3': b'DXT3', 'DXT5': b'DXT5'}
MAX_VERTICES = 65535


def flags_for(vsize, psize):
    """The resource flags word for segment sizes (each a count of pages of
    256 << shift bytes, 11 bits of count and 4 of shift)."""
    def part(size):
        for shift in range(16):
            unit = 256 << shift
            n = (size + unit - 1) // unit
            if n <= 0x7FF:
                return n, shift, n * unit
        raise ValueError('segment of %d bytes' % size)
    vn, vs, vfull = part(vsize)
    pn, ps, pfull = part(psize)
    return (vn | (vs << 11) | (pn << 15) | (ps << 26)), vfull, pfull


class Seg:
    """A segment under construction (16-byte aligned allocations)."""
    def __init__(self):
        self.buf = bytearray()

    def alloc(self, size, align=16):
        off = (len(self.buf) + align - 1) // align * align
        self.buf += b'\0' * (off + size - len(self.buf))
        return off

    def put(self, data, align=16):
        off = self.alloc(len(data), align)
        self.buf[off:off + len(data)] = data
        return off

    def cstr(self, s):
        return self.put(s.encode('latin-1') + b'\0', 4)

    def w32(self, off, v):
        struct.pack_into('<I', self.buf, off, v & 0xFFFFFFFF)

    def w16(self, off, v):
        struct.pack_into('<H', self.buf, off, v & 0xFFFF)

    def f32(self, off, *vals):
        struct.pack_into('<%df' % len(vals), self.buf, off, *vals)


def write_resource(path, rtype, virt, phys):
    flags, vfull, pfull = flags_for(len(virt), len(phys))
    body = bytes(virt) + b'\0' * (vfull - len(virt)) + bytes(phys) + b'\0' * (pfull - len(phys))
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', RSC_MAGIC, rtype, flags))
        f.write(zlib.compress(body, 6))
    return flags


# ---------------------------------------------------------------- shader samples
class ShaderLibrary:
    """PC shader samples by .sps name, from the given .wdr files (the first
    shader of each .sps wins)."""
    def __init__(self, paths):
        self.samples = {}
        for p in paths:
            try:
                r = rsc5.Resource.pc(p)
                # (a vehicle's drawable sits in its fragment, at +0xB4; v1.7.43)
                d = rsc5.read_fragtype(r)['drawable'] if p.lower().endswith('.wft') else rsc5.read_drawable(r, 0)
            except Exception:
                continue
            if not d:
                continue
            if not d['shader_group']:
                continue
            for s in d['shader_group']['shaders']:
                if s['sps'] and s['sps'] not in self.samples:
                    self.samples[s['sps']] = (r, s['off'])

    def __contains__(self, sps):
        return sps in self.samples

    def __getitem__(self, sps):
        return self.samples[sps]


# ---------------------------------------------------------------- drawables
class Drawable:
    def __init__(self, library):
        self.lib = library
        self.shaders = []     # (sps, textures {hash: name}, vectors {hash: floats})
        self.lods = [[], [], [], []]  # geometries per LOD: dict(shader, pos, nrm, col, uvs, tris)

    def shader(self, sps, textures, vectors=None, fallback=None):
        """A shader cloned from the library's sample of `sps` (or of
        `fallback` when there is none); returns its index."""
        if sps not in self.lib:
            assert fallback in self.lib, 'no PC sample of %s (nor of %s)' % (sps, fallback)
            sps = fallback
        self.shaders.append((sps, dict(textures), dict(vectors or {})))
        return len(self.shaders) - 1

    def geometry(self, lod, shader, pos, nrm, col, uvs, tris, tan=None, bones=None):
        """pos (n, 3), nrm (n, 3) or None, col (n, 4) uint8 or None, uvs a
        list of (n, 2) arrays, tris (m, 3) int, tan (n, 4) or None (the
        tangent and its handedness, for normal-mapped shaders). More than
        65535 vertices are split into several geometries. `bones`: a skinned
        geometry (a vehicle's, v1.7.43): the bone table (skeleton bone
        indexes) and every vertex bound wholly to its first entry."""
        pos = np.asarray(pos, np.float32)
        tris = np.asarray(tris, np.int64).reshape(-1, 3)
        n = len(pos)
        if n <= MAX_VERTICES:
            self.lods[lod].append(dict(shader=shader, pos=pos, nrm=None if nrm is None else np.asarray(nrm, np.float32),
                                       col=None if col is None else np.asarray(col, np.uint8),
                                       uvs=[np.asarray(u, np.float32) for u in uvs], tris=tris,
                                       tan=None if tan is None else np.asarray(tan, np.float32),
                                       bones=None if bones is None else [int(b) for b in bones]))
            return 1
        # split by triangle runs, re-indexing each part's vertices
        parts = 0
        start = 0
        while start < len(tris):
            used = {}
            end = start
            while end < len(tris):
                new = [v for v in tris[end] if v not in used]
                if len(used) + len(new) > MAX_VERTICES:
                    break
                for v in new:
                    used[v] = len(used)
                end += 1
            order = np.array(sorted(used, key=used.get), np.int64)
            remap = np.vectorize(used.get)(tris[start:end])
            self.geometry(lod, shader, pos[order], None if nrm is None else np.asarray(nrm)[order],
                          None if col is None else np.asarray(col)[order], [np.asarray(u)[order] for u in uvs], remap,
                          None if tan is None else np.asarray(tan)[order], bones)
            parts += 1
            start = end
        return parts

    # -- writing
    def _write_shader(self, v, sps, textures, vectors):
        src, so = self.lib[sps]
        out = v.put(src.virt[so:so + 0x60])
        v.w32(out + 4, 0)
        # the effect and preset names
        for k in (0x44, 0x48):
            p = src.vptr(so + k)
            v.w32(out + k, (VP | v.cstr(src.cstr(p))) if p is not None else 0)
        # the parameter block: offsets | types | names | values, copied whole
        b = so + 8
        po, size = src.vptr(b + 12), src.u32(so + 0x20)
        pt, pn = src.vptr(b + 28), src.vptr(b + 44)
        params = batconv.shader_params(src, so)
        block = v.put(src.virt[po:po + size])
        v.w32(out + 0x14, VP | block)
        v.w32(out + 0x24, VP | (block + (pt - po)))
        v.w32(out + 0x34, VP | (block + (pn - po)))
        for i, p in enumerate(params):
            if p['type'] == 0:
                name = textures.get(p['hash'])
                if name is None and p['value'] is not None:
                    np_ = src.vptr(p['value'] + 20)
                    name = src.cstr(np_) if np_ is not None else None
                if name is None:
                    v.w32(block + 4 * i, 0)
                    continue
                t = v.alloc(0x20)
                v.w32(t, VFT['texture_ref'])
                v.w32(t + 8, 0x00010002)
                v.w32(t + 20, VP | v.cstr(name))
                v.w32(block + 4 * i, VP | t)
            else:
                assert p['value'] is not None and po <= p['value'] < po + size
                o = block + (p['value'] - po)
                vals = vectors.get(p['hash'])
                if vals is not None:
                    vals = list(vals) + [0.0] * (4 * p['type'] - len(vals))
                    v.f32(o, *vals[:4 * p['type']])
                v.w32(block + 4 * i, VP | o)
        return out

    def _write_geometry(self, v, p, g):
        n = len(g['pos'])
        usage = 1
        if g.get('bones') is not None:
            usage |= (1 << 1) | (1 << 2)  # blend weights and indices
        if g['nrm'] is not None:
            usage |= 1 << 3
        if g['col'] is not None:
            usage |= 1 << 4
        for k in range(len(g['uvs'])):
            usage |= 1 << (6 + k)
        if g.get('tan') is not None:
            usage |= 1 << 14
        elems, stride = batconv.vertex_layout(usage, batconv.SIZE_PC)
        at = dict(elems)
        vdata = np.zeros((n, stride), np.uint8)
        vdata[:, at[0]:at[0] + 12] = g['pos'].astype('<f4').view(np.uint8)
        if g.get('bones') is not None:  # (wholly on the table's first bone: weight 255 in the first slot)
            vdata[:, at[1]] = 255
        if g['nrm'] is not None:
            vdata[:, at[3]:at[3] + 12] = g['nrm'].astype('<f4').view(np.uint8)
        if g['col'] is not None:
            vdata[:, at[4]:at[4] + 4] = g['col']
        for k, uv in enumerate(g['uvs']):
            o = at[6 + k]
            vdata[:, o:o + 8] = uv.astype('<f4').view(np.uint8)
        if g.get('tan') is not None:
            vdata[:, at[14]:at[14] + 16] = g['tan'].astype('<f4').view(np.uint8)
        idx = g['tris'].astype('<u2').tobytes()
        vdata = vdata.tobytes()
        vb_data = p.put(vdata)
        ib_data = p.put(idx)
        decl = v.alloc(16)
        v.w32(decl, usage)
        v.w16(decl + 4, stride)
        v.buf[decl + 7] = 4
        struct.pack_into('<Q', v.buf, decl + 8, batconv.PC_TYPES)
        vb = v.alloc(0x40)
        v.w32(vb, VFT['vertex_buffer'])
        v.w32(vb + 4, n)
        v.w32(vb + 12, stride)
        v.w32(vb + 16, VP | decl)
        v.w32(vb + 24, PP | vb_data)
        ib = v.alloc(0x30)
        v.w32(ib, VFT['index_buffer'])
        v.w32(ib + 4, len(g['tris']) * 3)
        v.w32(ib + 8, PP | ib_data)
        geo = v.alloc(0x50)
        v.w32(geo, VFT['geometry'])
        v.w32(geo + 12, VP | vb)
        v.w32(geo + 28, VP | ib)
        v.w32(geo + 44, len(g['tris']) * 3)
        v.w32(geo + 48, len(g['tris']))
        v.w16(geo + 52, n)
        v.w16(geo + 54, 3)
        v.w16(geo + 60, stride)
        if g.get('bones') is not None:  # the bone table (+56 its entries, +62 their count, after the stride's u16)
            tbl = v.alloc(2 * len(g['bones']))
            for i, b in enumerate(g['bones']):
                v.w16(tbl + 2 * i, b)
            v.w32(geo + 56, VP | tbl)
            v.w16(geo + 62, len(g['bones']))
        return geo

    @staticmethod
    def _sphere(points):
        lo, hi = points.min(0), points.max(0)
        c = (lo + hi) / 2
        return c, float(np.sqrt(((points - c) ** 2).sum(1)).max()), lo, hi

    def write(self, path):
        v, p = Seg(), Seg()
        self.emit(v, p)
        flags = write_resource(path, TYPE_WDR, v.buf, p.buf)
        return dict(flags=flags, vsize=len(v.buf), psize=len(p.buf), radius=self.radius, bbox=self.bbox)

    def emit(self, v, p, skeleton=None):
        """The drawable into the segments; returns its root's offset. A
        vehicle's `skeleton` (v1.7.43): [(name, parent index, (x, y, z))],
        parents first, each bone's position in the car - written as the PC
        files have it (bones of 0xE0 bytes, the three matrix arrays)."""
        assert self.lods[0], 'no geometry'
        root = v.alloc(0x90)
        blockmap = v.alloc(0x10)
        sg = v.alloc(0x40)
        v.w32(sg, VFT['shader_group'])
        shader_offs = [self._write_shader(v, *s) for s in self.shaders]
        lst = v.alloc(4 * len(shader_offs))
        for i, so in enumerate(shader_offs):
            v.w32(lst + 4 * i, VP | so)
        v.w32(sg + 8, VP | lst)
        v.w16(sg + 12, len(shader_offs))
        v.w16(sg + 14, len(shader_offs))
        all_points = []
        lod_colls = []
        masks = []
        for lod in self.lods:
            if not lod:
                lod_colls.append(0)
                masks.append(0xFFFFFFFF)
                continue
            geos, spheres = [], []
            for g in lod:
                geos.append(self._write_geometry(v, p, g))
                c, r, lo, hi = self._sphere(g['pos'])
                spheres.append((c, r))
                all_points.append(np.stack([lo, hi]))
            mask = 0
            for g in lod:
                src, so = self.lib[self.shaders[g['shader']][0]]
                mask |= 1 << src.virt[so + 9]
            masks.append(mask)
            m = v.alloc(0x20)
            v.w32(m, VFT['model'])
            glist = v.alloc(4 * len(geos))
            for i, go in enumerate(geos):
                v.w32(glist + 4 * i, VP | go)
            v.w32(m + 4, VP | glist)
            v.w16(m + 8, len(geos))
            v.w16(m + 10, len(geos))
            pts = np.concatenate([g['pos'] for g in lod])
            whole = self._sphere(pts)
            entries = ([(whole[0], whole[1])] if len(geos) > 1 else []) + spheres
            b = v.alloc(16 * len(entries))
            for i, (c, r) in enumerate(entries):
                v.f32(b + 16 * i, float(c[0]), float(c[1]), float(c[2]), r)
            v.w32(m + 12, VP | b)
            smap = v.alloc(2 * len(geos))
            for i, g in enumerate(lod):
                v.w16(smap + 2 * i, g['shader'])
            v.w32(m + 16, VP | smap)
            v.w16(m + 26, len(geos))
            mlist = v.alloc(4)
            v.w32(mlist, VP | m)
            coll = v.alloc(8)
            v.w32(coll, VP | mlist)
            v.w16(coll + 4, 1)
            v.w16(coll + 6, 1)
            lod_colls.append(coll)
        pts = np.concatenate(all_points)
        c, r, lo, hi = self._sphere(pts)
        v.w32(root, VFT['drawable'])
        v.w32(root + 4, VP | blockmap)
        v.w32(root + 8, VP | sg)
        v.f32(root + 16, float(c[0]), float(c[1]), float(c[2]))
        v.w32(root + 28, NAN_W)
        v.f32(root + 32, float(lo[0]), float(lo[1]), float(lo[2]))
        v.w32(root + 44, NAN_W)
        v.f32(root + 48, float(hi[0]), float(hi[1]), float(hi[2]))
        v.w32(root + 60, NAN_W)
        for i, coll in enumerate(lod_colls):
            v.w32(root + 64 + 4 * i, (VP | coll) if coll else 0)
            v.f32(root + 80 + 4 * i, 9999.0)
            v.w32(root + 96 + 4 * i, masks[i])
        v.f32(root + 112, r)
        if skeleton:
            v.w32(root + 12, VP | write_skeleton(v, skeleton))
        self.radius, self.bbox = r, (lo.tolist(), hi.tolist())
        return root


def write_skeleton(v, bones):
    """A PC skeleton: bones of 0xE0 bytes (name, flags, parent, local
    translation, an identity rotation, the stored position, limits of +-pi),
    the global, inverse global and local matrix arrays (64 bytes each,
    identity rotations with the translation in the fourth row), the header
    the PC files carry (gauntletc.wft checked)."""
    n = len(bones)
    sk = v.alloc(0x40)
    bo = v.alloc(0xE0 * n)
    for i, (name, parent, pos) in enumerate(bones):
        b = bo + 0xE0 * i
        v.w32(b, VP | v.cstr(name))
        v.w32(b + 4, 0x0008038E)
        v.w32(b + 0x10, (VP | (bo + 0xE0 * parent)) if parent >= 0 else 0)
        v.buf[b + 0x18] = i & 0xFF
        v.buf[b + 0x19] = i & 0xFF
        ppos = bones[parent][2] if parent >= 0 else (0.0, 0.0, 0.0)
        v.f32(b + 0x20, pos[0] - ppos[0], pos[1] - ppos[1], pos[2] - ppos[2])
        v.w32(b + 0x2C, NAN_W)
        v.w32(b + 0x3C, NAN_W)
        v.f32(b + 0x40, 0.0, 0.0, 0.0, 1.0)
        v.w32(b + 0x5C, NAN_W)
        v.f32(b + 0x60, pos[0], pos[1], pos[2])
        for o in (0x6C, 0x7C, 0x8C, 0x9C, 0xAC, 0xBC, 0xCC):
            v.w32(b + o, NAN_W)
        v.f32(b + 0xB0, -3.14159274, -3.14159274, -3.14159274)
        v.f32(b + 0xC0, 3.14159274, 3.14159274, 3.14159274)
    arrays = []
    for kind in ('global', 'inverse', 'local'):
        a = v.alloc(64 * n)
        for i, (name, parent, pos) in enumerate(bones):
            m = a + 64 * i
            v.f32(m, 1.0, 0.0, 0.0, 0.0)
            v.f32(m + 16, 0.0, 1.0, 0.0, 0.0)
            v.f32(m + 32, 0.0, 0.0, 1.0, 0.0)
            if kind == 'local':
                ppos = bones[parent][2] if parent >= 0 else (0.0, 0.0, 0.0)
                v.f32(m + 48, pos[0] - ppos[0], pos[1] - ppos[1], pos[2] - ppos[2], 1.0)
            elif kind == 'global':
                v.f32(m + 48, pos[0], pos[1], pos[2], 1.0)
            else:
                v.f32(m + 48, -pos[0], -pos[1], -pos[2], 1.0)
        arrays.append(a)
    parents = v.alloc(2 * n)
    for i, (name, parent, pos) in enumerate(bones):
        v.w16(parents + 2 * i, parent & 0xFFFF)
    v.w32(sk, VP | bo)
    v.w32(sk + 4, VP | parents)
    v.w32(sk + 8, VP | arrays[0])
    v.w32(sk + 12, VP | arrays[1])
    v.w32(sk + 16, VP | arrays[2])
    v.w16(sk + 20, n)
    v.w16(sk + 22, 3)
    v.w32(sk + 24, 0xC0)
    v.w32(sk + 28, 0xF)
    v.w32(sk + 32, VP | parents)
    v.w16(sk + 36, n)
    v.w16(sk + 38, n)
    v.w32(sk + 40, 3)
    return sk


# ---------------------------------------------------------------- vehicles
TYPE_WFT = 112


class Fragment:
    """A vehicle's fragment (.wft) around a Drawable (v1.7.43): the fragType
    header as the PC files have it (the first 0x200 bytes of a PC vehicle
    file given as the pattern, its pointers cleared), the drawable with its
    skeleton at +0xB4, the bounding sphere at +0x10 - no physics children
    and no bound: the console converter (batconv.Converter) takes those from
    its stock template, and a car with no wheel meshes hides the template's.

        Fragment(drawable, skeleton).write('out/car.wft', pattern='some.wft')
    """
    def __init__(self, drawable, skeleton):
        self.drawable = drawable
        self.skeleton = skeleton

    def write(self, path, pattern):
        pat = rsc5.Resource.pc(pattern)
        head = bytearray(pat.virt[:0x200])
        for o in range(0, 0x200, 4):  # every pointer cleared (the drawable's set below)
            w = struct.unpack_from('<I', head, o)[0]
            if w >> 28 in (5, 6):
                struct.pack_into('<I', head, o, 0)
        head[0x1F3] = 0  # no children
        v, p = Seg(), Seg()
        root = v.put(bytes(head))
        assert root == 0
        d = self.drawable.emit(v, p, self.skeleton)
        v.w32(0xB4, VP | d)
        blockmap = v.alloc(0x10)
        v.w32(4, VP | blockmap)
        lo, hi = (np.array(x, np.float32) for x in self.drawable.bbox)
        c = (lo + hi) / 2
        r = float(np.sqrt(((hi - c) ** 2).sum()))
        v.f32(0x10, float(c[0]), float(c[1]), float(c[2]), r)
        flags = write_resource(path, TYPE_WFT, v.buf, p.buf)
        return dict(flags=flags, vsize=len(v.buf), psize=len(p.buf), radius=r)


# ---------------------------------------------------------------- texture dictionaries
def dxt_level_bytes(fmt, w, h):
    blocks = max(1, (w + 3) // 4) * max(1, (h + 3) // 4)
    return blocks * (8 if fmt == 'DXT1' else 16)


class Dictionary:
    def __init__(self, textures):
        """textures: {name: (w, h, fmt, [level data...])} with fmt one of
        DXT1 / DXT3 / DXT5 (each level's bytes as D3D lays them out)."""
        self.textures = textures

    def write(self, path):
        v, p = Seg(), Seg()
        names = sorted(self.textures, key=lambda n: batconv.joaat(n))
        root = v.alloc(0x20)
        blockmap = v.alloc(0x10)
        v.w32(root, VFT['dictionary'])
        v.w32(root + 4, VP | blockmap)
        v.w32(root + 12, 1)
        hashes = v.alloc(4 * len(names))
        tlist = v.alloc(4 * len(names))
        for i, name in enumerate(names):
            w, h, fmt, levels = self.textures[name]
            assert fmt in FOURCC, fmt
            for k, lv in enumerate(levels):
                need = dxt_level_bytes(fmt, max(1, w >> k), max(1, h >> k))
                assert len(lv) == need, (name, k, len(lv), need)
            data = p.put(b''.join(levels), 128)
            t = v.alloc(0x50)
            v.w32(t, VFT['texture'])
            v.w32(t + 8, 0x00010000)
            v.w32(t + 20, VP | v.cstr('pack:/%s.dds' % name))
            v.w16(t + 28, w)
            v.w16(t + 30, h)
            v.buf[t + 32:t + 36] = FOURCC[fmt]
            v.w16(t + 36, w if fmt != 'DXT1' else w // 2)
            v.buf[t + 39] = len(levels)
            v.f32(t + 40, 1.0, 1.0, 1.0)
            v.w32(t + 72, PP | data)
            v.w32(hashes + 4 * i, batconv.joaat(name))
            v.w32(tlist + 4 * i, VP | t)
        v.w32(root + 16, VP | hashes)
        v.w16(root + 20, len(names))
        v.w16(root + 22, len(names))
        v.w32(root + 24, VP | tlist)
        v.w16(root + 28, len(names))
        v.w16(root + 30, len(names))
        return write_resource(path, TYPE_WTD, v.buf, p.buf)


def glob_wdr(dirs):
    out = []
    for d in dirs:
        for f in sorted(os.listdir(d)):
            if f.lower().endswith('.wdr'):
                out.append(os.path.join(d, f))
    return out
