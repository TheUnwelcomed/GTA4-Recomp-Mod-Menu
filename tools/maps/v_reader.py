"""Read GTA V (PC, gen8) map resources into plain Python data, for the map
converter: drawables (.ydr), texture dictionaries (.ytd), placements (.ymap)
and archetypes (.ytyp). Layouts as CodeWalker reads them (CodeWalker.Core/
GameFiles/Resources: Drawable.cs, Texture.cs, VertexType.cs; MetaTypes).

A resource file as rpf7.py writes it: the 16-byte RSC7 header (magic,
version, system flags, graphics flags), then the raw-deflated system pages
followed by the graphics pages. Pointers are 64-bit: 0x5xxxxxxx into the
system pages, 0x6xxxxxxx into the graphics pages.
"""
import struct
import zlib

import numpy as np


def page_size(flags):
    s = (((flags >> 27) & 0x1) << 0) + (((flags >> 26) & 0x1) << 1) + (((flags >> 25) & 0x1) << 2) \
        + (((flags >> 24) & 0x1) << 3) + (((flags >> 17) & 0x7F) << 4) + (((flags >> 11) & 0x3F) << 5) \
        + (((flags >> 7) & 0xF) << 6) + (((flags >> 5) & 0x3) << 7) + (((flags >> 4) & 0x1) << 8)
    return (0x200 << (flags & 0xF)) * s


def joaat(s):
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


class Resource:
    def __init__(self, blob):
        if blob[:4] != b'RSC7':
            raise ValueError('not an RSC7 resource')
        self.version, self.sysf, self.gfxf = struct.unpack_from('<III', blob, 4)
        d = zlib.decompressobj(-15)
        data = d.decompress(blob[16:]) + d.flush()
        n = page_size(self.sysf)
        self.sys, self.gfx = data[:n], data[n:]

    @classmethod
    def open(cls, path):
        return cls(open(path, 'rb').read())

    def at(self, ptr):
        """(buffer, offset) for a pointer, or (None, 0)."""
        if not ptr:
            return None, 0
        seg = ptr >> 28
        off = ptr & 0x0FFFFFFF
        if seg == 5:
            return self.sys, off
        if seg == 6:
            return self.gfx, off
        return None, 0

    def u8(self, ptr):
        b, o = self.at(ptr)
        return b[o]

    def u16(self, ptr):
        b, o = self.at(ptr)
        return struct.unpack_from('<H', b, o)[0]

    def u32(self, ptr):
        b, o = self.at(ptr)
        return struct.unpack_from('<I', b, o)[0]

    def u64(self, ptr):
        b, o = self.at(ptr)
        return struct.unpack_from('<Q', b, o)[0]

    def f32s(self, ptr, n):
        b, o = self.at(ptr)
        return struct.unpack_from('<%df' % n, b, o)

    def bytes(self, ptr, n):
        b, o = self.at(ptr)
        return bytes(b[o:o + n])

    def cstr(self, ptr, limit=256):
        b, o = self.at(ptr)
        if b is None:
            return None
        end = b.index(b'\0', o, min(len(b), o + limit))
        return b[o:end].decode('latin-1')


# ---------------------------------------------------------------- vertices
# Component types (VertexComponentType): their sizes and how they decode.
COMP_SIZE = {0: 0, 1: 4, 2: 4, 3: 8, 4: 4, 5: 8, 6: 12, 7: 16, 8: 4, 9: 4, 10: 4}
SEMANTICS = ['position', 'blendweights', 'blendindices', 'normal', 'colour0', 'colour1',
             'texcoord0', 'texcoord1', 'texcoord2', 'texcoord3', 'texcoord4', 'texcoord5', 'texcoord6',
             'texcoord7', 'tangent', 'binormal']


def decode_component(raw, ctype, count, stride, offset):
    """An (count, n) float array for one vertex component."""
    view = np.frombuffer(raw, dtype=np.uint8).reshape(count, stride)[:, offset:offset + COMP_SIZE[ctype]]
    view = np.ascontiguousarray(view)
    if ctype == 1:  # half2
        return view.view('<f2').astype(np.float32).reshape(count, 2)
    if ctype == 3:  # half4
        return view.view('<f2').astype(np.float32).reshape(count, 4)
    if ctype in (2, 4):
        return view.view('<f4').reshape(count, 1)
    if ctype == 5:
        return view.view('<f4').reshape(count, 2)
    if ctype == 6:
        return view.view('<f4').reshape(count, 3)
    if ctype == 7:
        return view.view('<f4').reshape(count, 4)
    if ctype in (8, 9):  # ubyte4 / colour (0..255; colour normalised)
        a = view.reshape(count, 4).astype(np.float32)
        return a / 255.0 if ctype == 9 else a
    if ctype == 10:  # rgba8 snorm
        return view.view(np.int8).reshape(count, 4).astype(np.float32) / 127.0
    raise ValueError('vertex component type %d' % ctype)


# ---------------------------------------------------------------- drawables
def read_textures_dict(r, ptr):
    """A texture dictionary at `ptr`: {name: texture}."""
    if not ptr:
        return {}
    # TextureDictionary: ResourceFileBase (16), 4 x u32 unknowns, hashes list (ptr, count, cap),
    # textures pointer list (ptr, count, cap)
    hashes_ptr = r.u64(ptr + 0x20)
    count = r.u16(ptr + 0x28)
    tex_ptr = r.u64(ptr + 0x30)
    out = {}
    for i in range(count):
        t = r.u64(tex_ptr + 8 * i)
        tex = read_texture(r, t)
        if tex:
            out[tex['name'].lower()] = tex
    return out


# Texture formats (D3DFORMAT / FourCC as GTA V stores them)
TEX_FORMATS = {0x31545844: 'DXT1', 0x33545844: 'DXT3', 0x35545844: 'DXT5', 0x31495441: 'ATI1',
               0x32495441: 'ATI2', 0x20374342: 'BC7', 21: 'A8R8G8B8', 32: 'A8B8G8R8', 28: 'A8', 50: 'L8',
               25: 'A1R5G5B5', 22: 'X8R8G8B8'}


def level_size(fmt, w, h):
    if fmt in ('DXT1', 'ATI1'):
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
    if fmt in ('DXT3', 'DXT5', 'ATI2', 'BC7'):
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
    if fmt in ('A8R8G8B8', 'A8B8G8R8', 'X8R8G8B8'):
        return w * h * 4
    if fmt in ('A8', 'L8'):
        return w * h
    if fmt == 'A1R5G5B5':
        return w * h * 2
    return 0


def read_texture(r, ptr):
    if not ptr:
        return None
    name = r.cstr(r.u64(ptr + 0x28))
    out = dict(name=name or '')
    # A full texture (in a dictionary) carries its size, format and data;
    # a bare reference (TextureBase) only its name.
    w, h, depth, stride = struct.unpack_from('<HHHH', *_slice(r, ptr + 0x50, 8))
    fmt_code = r.u32(ptr + 0x58)
    levels = r.u8(ptr + 0x5D)
    data_ptr = r.u64(ptr + 0x70)
    fmt = TEX_FORMATS.get(fmt_code)
    if w and h and fmt and data_ptr and (data_ptr >> 28) == 6:
        total, lw, lh = 0, w, h
        for _ in range(max(1, levels)):
            total += level_size(fmt, lw, lh)
            lw, lh = max(1, lw // 2), max(1, lh // 2)
        out.update(w=w, h=h, fmt=fmt, levels=levels, stride=stride, data=r.bytes(data_ptr, total))
    return out


def _slice(r, ptr, n):
    b, o = r.at(ptr)
    return b, o


def read_shader(r, ptr):
    params_ptr = r.u64(ptr)
    name_hash = r.u32(ptr + 8)
    count = r.u8(ptr + 0x10)
    file_hash = r.u32(ptr + 0x18)
    params = []
    offset = 0
    for i in range(count):
        p = params_ptr + 16 * i
        dtype = r.u8(p)
        dptr = r.u64(p + 8)
        if dtype == 0:
            params.append(('texture', read_texture(r, dptr) if dptr else None))
        else:
            params.append(('vector', r.f32s(dptr, 4 * dtype) if dptr else None))
            offset += 16 * dtype
    hashes_at = params_ptr + 16 * count + offset
    names = [r.u32(hashes_at + 4 * i) for i in range(count)]
    return dict(name_hash=name_hash, file_hash=file_hash,
                params={n: v for n, v in zip(names, params)})


def read_geometry(r, ptr):
    vb_ptr = r.u64(ptr + 0x18)
    ib_ptr = r.u64(ptr + 0x38)
    # VertexBuffer (gen8): stride, flags, data pointer, vertex count, ..., info (declaration)
    stride = r.u16(vb_ptr + 8)
    data_ptr = r.u64(vb_ptr + 0x10)
    vcount = r.u32(vb_ptr + 0x18)
    info_ptr = r.u64(vb_ptr + 0x30)
    flags = r.u32(info_ptr)
    decl_stride = r.u16(info_ptr + 4)
    types = r.u64(info_ptr + 8)
    raw = r.bytes(data_ptr, vcount * stride)
    comps, off = {}, 0
    for sem in range(16):
        if not (flags >> sem) & 1:
            continue
        ctype = (types >> (4 * sem)) & 0xF
        comps[SEMANTICS[sem]] = decode_component(raw, ctype, vcount, stride, off)
        off += COMP_SIZE[ctype]
    icount = r.u32(ib_ptr + 8)
    ind_ptr = r.u64(ib_ptr + 0x10)
    indices = np.frombuffer(r.bytes(ind_ptr, 2 * icount), dtype='<u2').copy()
    return dict(vertex_count=vcount, stride=stride, decl_stride=decl_stride, decl_flags=flags, decl_types=types,
                components=comps, indices=indices)


def read_model(r, ptr):
    geoms_ptr = r.u64(ptr + 8)
    n = r.u16(ptr + 0x10)
    mapping_ptr = r.u64(ptr + 0x20)
    skel = r.u32(ptr + 0x28)
    mask = r.u16(ptr + 0x2C)
    geoms = []
    for i in range(n):
        g = read_geometry(r, r.u64(geoms_ptr + 8 * i))
        g['shader'] = r.u16(mapping_ptr + 2 * i)
        geoms.append(g)
    return dict(geometries=geoms, render_mask=mask, skeleton_binding=skel)


def read_pointer_list(r, ptr):
    lst = r.u64(ptr)
    count = r.u16(ptr + 8)
    return [r.u64(lst + 8 * i) for i in range(count)]


def read_drawable(r, ptr=0x50000000, with_name=True):
    sg = r.u64(ptr + 0x10)
    center = r.f32s(ptr + 0x20, 4)
    bmin = r.f32s(ptr + 0x30, 3)
    bmax = r.f32s(ptr + 0x40, 3)
    lods = []
    for k in range(4):
        lp = r.u64(ptr + 0x50 + 8 * k)
        lods.append([read_model(r, m) for m in read_pointer_list(r, lp)] if lp else [])
    lod_dist = r.f32s(ptr + 0x70, 4)
    shaders, embedded = [], {}
    if sg:
        embedded = read_textures_dict(r, r.u64(sg + 8))
        sh_ptr = r.u64(sg + 0x10)
        sh_count = r.u16(sg + 0x18)
        shaders = [read_shader(r, r.u64(sh_ptr + 8 * i)) for i in range(sh_count)]
    name = r.cstr(r.u64(ptr + 0xA8)) if with_name else None
    return dict(name=name, center=center[:3], radius=center[3], bbox=(bmin, bmax), lods=lods, lod_dist=lod_dist,
                shaders=shaders, embedded_textures=embedded)


def read_ydr(path):
    return read_drawable(Resource.open(path))


def read_ytd(path):
    r = Resource.open(path)
    return read_textures_dict(r, 0x50000000)


# ---------------------------------------------------------------- meta (ymap, ytyp)
def meta_blocks(r):
    """{structure name hash: [(length, offset in the system pages)]} and the root block."""
    root = struct.unpack_from('<i', r.sys, 0x1C)[0]
    blocks_ptr = struct.unpack_from('<Q', r.sys, 0x30)[0] & 0x0FFFFFFF
    n = struct.unpack_from('<h', r.sys, 0x4C)[0]
    blocks = [struct.unpack_from('<IiQ', r.sys, blocks_ptr + 16 * i) for i in range(n)]
    return blocks, blocks[root - 1]


def meta_ptr(r, blocks, ptr):
    """A meta pointer (block index in the low 12 bits, 1-based; offset in
    the rest) to an offset in the system pages."""
    block = (ptr & 0xFFF) - 1
    offset = (ptr >> 12) & 0xFFFFF
    if block < 0 or block >= len(blocks):
        return None
    return (blocks[block][2] & 0x0FFFFFFF) + offset


def read_ymap(path):
    r = Resource.open(path)
    blocks, root = meta_blocks(r)
    base = root[2] & 0x0FFFFFFF
    # CMapData: entities array at +96 (pointer, count)
    ents_ptr, ents_count = struct.unpack_from('<IxxxxH', r.sys, base + 96)
    start = meta_ptr(r, blocks, ents_ptr)
    out = []
    for i in range(ents_count):
        p = struct.unpack_from('<Q', r.sys, start + 8 * i)[0] & 0xFFFFFFFF
        e = meta_ptr(r, blocks, p)
        if e is None:
            continue
        # CEntityDef (128 bytes): archetypeName +8, flags +12, guid +16, position +32 (vec3), rotation
        # +48 (vec4), scaleXY +64, scaleZ +68, parentIndex +72, lodDist +76, childLodDist +80,
        # lodLevel +84, numChildren +88, priorityLevel +92
        name, flags, guid = struct.unpack_from('<III', r.sys, e + 8)
        pos = struct.unpack_from('<3f', r.sys, e + 32)
        rot = struct.unpack_from('<4f', r.sys, e + 48)
        sxy, sz, parent, lod_dist, child_lod = struct.unpack_from('<ffiff', r.sys, e + 64)
        lod_level, num_children = struct.unpack_from('<II', r.sys, e + 84)
        out.append(dict(archetype=name, flags=flags, pos=pos, rot=rot, scale=(sxy, sz), parent=parent,
                        lod_dist=lod_dist, child_lod_dist=child_lod, lod_level=lod_level, children=num_children))
    return out


def read_ytyp(path):
    r = Resource.open(path)
    blocks, root = meta_blocks(r)
    base = root[2] & 0x0FFFFFFF
    # CMapTypes: extensions +8, archetypes array at +24 (pointer, count)
    arch_ptr, arch_count = struct.unpack_from('<IxxxxH', r.sys, base + 24)
    start = meta_ptr(r, blocks, arch_ptr)
    out = []
    for i in range(arch_count):
        p = struct.unpack_from('<Q', r.sys, start + 8 * i)[0] & 0xFFFFFFFF
        a = meta_ptr(r, blocks, p)
        if a is None:
            continue
        # CBaseArchetypeDef: lodDist +8, flags +12, specialAttribute +16, bbMin +32, bbMax +48,
        # bsCentre +64, bsRadius +80, hdTextureDist +84, name +88, textureDictionary +92,
        # clipDictionary +96, drawableDictionary +100, physicsDictionary +104, assetType +108,
        # assetName +112
        lod_dist, flags, special = struct.unpack_from('<fII', r.sys, a + 8)
        bmin = struct.unpack_from('<3f', r.sys, a + 32)
        bmax = struct.unpack_from('<3f', r.sys, a + 48)
        bsc = struct.unpack_from('<3f', r.sys, a + 64)
        bsr, hd = struct.unpack_from('<ff', r.sys, a + 80)
        name, txd, clip, drawable_dict, physics, asset_type, asset_name = struct.unpack_from('<7I', r.sys, a + 88)
        out.append(dict(name=name, txd=txd, drawable_dict=drawable_dict, physics=physics, asset_type=asset_type,
                        asset_name=asset_name, lod_dist=lod_dist, flags=flags, bbox=(bmin, bmax),
                        sphere=(bsc, bsr)))
    return out
