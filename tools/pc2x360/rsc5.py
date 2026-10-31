"""RAGE RSC5 resource reader (GTA IV), PC and Xbox 360 flavours.

PC:   'RSC\\x05' little-endian header, zlib body, LE pointer graph.
360:  header stored big-endian, XMem LZX body (decode with xlzx unrsc first),
      BE pointer graph.
Pointers: 0x5xxxxxxx -> virtual (CPU) segment, 0x6xxxxxxx -> physical (GPU).
Drawable-side layouts follow SparkIV's RageLib (GPLv3).
"""
import struct
import zlib

RSC_MAGIC = 0x05435352


def seg_sizes(flags):
    virt = (flags & 0x7FF) << (((flags >> 11) & 0xF) + 8)
    phys = ((flags >> 15) & 0x7FF) << (((flags >> 26) & 0xF) + 8)
    return virt, phys


class Resource:
    def __init__(self, data, flags, big, rtype=0, name=''):
        self.big = big
        self.e = '>' if big else '<'
        self.flags = flags
        self.type = rtype
        self.name = name
        self.vsize, self.psize = seg_sizes(flags)
        self.data = data
        self.virt = data[:self.vsize]
        self.phys = data[self.vsize:self.vsize + self.psize]

    @classmethod
    def pc(cls, path):
        raw = open(path, 'rb').read()
        magic, rtype, flags = struct.unpack_from('<III', raw, 0)
        assert magic == RSC_MAGIC, path
        return cls(zlib.decompress(raw[12:]), flags, False, rtype, path)

    @classmethod
    def x360(cls, rsc_path, body_path):
        raw = open(rsc_path, 'rb').read()
        magic, rtype, flags = struct.unpack_from('>III', raw, 0)
        assert magic == RSC_MAGIC, rsc_path
        return cls(open(body_path, 'rb').read(), flags, True, rtype, rsc_path)

    def u32(self, off, seg=None):
        return struct.unpack_from(self.e + 'I', self.virt if seg is None else seg, off)[0]

    def u16(self, off, seg=None):
        return struct.unpack_from(self.e + 'H', self.virt if seg is None else seg, off)[0]

    def u8(self, off, seg=None):
        return (self.virt if seg is None else seg)[off]

    def f32(self, off, seg=None):
        return struct.unpack_from(self.e + 'f', self.virt if seg is None else seg, off)[0]

    def vec4(self, off):
        return struct.unpack_from(self.e + '4f', self.virt, off)

    @staticmethod
    def resolve(v):
        if v == 0:
            return None
        top = v >> 28
        if top == 5:
            return ('v', v & 0x0FFFFFFF)
        if top == 6:
            return ('p', v & 0x0FFFFFFF)
        return ('?', v)

    def ptr(self, off):
        return self.resolve(self.u32(off))

    def vptr(self, off):
        p = self.ptr(off)
        return p[1] if p and p[0] == 'v' and p[1] < self.vsize else None

    def cstr(self, off, limit=128):
        end = self.virt.find(b'\0', off, off + limit)
        return self.virt[off:end if end >= 0 else off + limit].decode('latin-1')

    def is_ptr(self, v):
        top = v >> 28
        return (top == 5 and (v & 0x0FFFFFFF) < self.vsize) or \
               (top == 6 and (v & 0x0FFFFFFF) < self.psize)


def ptr_collection(r, off):
    """PtrCollection<T>: ptr list, u16 count, u16 size -> list of item offsets."""
    lst = r.vptr(off)
    count = r.u16(off + 4)
    items = []
    if lst is not None:
        for i in range(count):
            items.append(r.vptr(lst + 4 * i))
    return items


def read_vertex_decl(r, off):
    return dict(usage=r.u32(off), stride=r.u16(off + 4), alt=r.u8(off + 6), type=r.u8(off + 7),
                types=struct.unpack_from(r.e + 'Q', r.virt, off + 8)[0])


def read_geometry(r, off):
    g = {'off': off}
    vb = r.vptr(off + 12)
    ib = r.vptr(off + 28)
    g['index_count'] = r.u32(off + 44)
    g['face_count'] = r.u32(off + 48)
    g['vertex_count'] = r.u16(off + 52)
    g['prim'] = r.u16(off + 54)
    g['stride'] = r.u16(off + 60)
    if vb is not None:
        g['vb_off'] = vb
        g['vb_count'] = r.u16(vb + 4)
        # +8 is the locked-data pointer, +24 the buffer's own data pointer;
        # some exporters fill only the latter.
        g['vb_data'] = r.resolve(r.u32(vb + 8)) or r.resolve(r.u32(vb + 24))
        g['vb_stride'] = r.u32(vb + 12)
        d = r.vptr(vb + 16)
        g['decl'] = read_vertex_decl(r, d) if d is not None else None
    if ib is not None:
        g['ib_off'] = ib
        g['ib_count'] = r.u32(ib + 4)
        g['ib_data'] = r.resolve(r.u32(ib + 8))
    return g


def read_model(r, off):
    m = {'off': off}
    m['geometries'] = [read_geometry(r, g) for g in ptr_collection(r, off + 4) if g is not None]
    return m


def read_shader_group(r, off):
    sg = {'off': off, 'texdict': r.ptr(off + 4)}
    shaders = []
    for s in ptr_collection(r, off + 8):
        if s is None:
            continue
        base = s + 8
        name_ptr = r.vptr(s + 0x44)
        sps_ptr = r.vptr(s + 0x48)
        shaders.append(dict(off=s, param_count=r.u32(base + 0x14),
                            name=r.cstr(name_ptr) if name_ptr is not None else None,
                            sps=r.cstr(sps_ptr) if sps_ptr is not None else None))
    sg['shaders'] = shaders
    return sg


def read_drawable(r, off):
    d = {'off': off}
    sg = r.vptr(off + 8)
    sk = r.vptr(off + 12)
    d['center'] = r.vec4(off + 16)
    d['bmin'] = r.vec4(off + 32)
    d['bmax'] = r.vec4(off + 48)
    d['lods'] = []
    for i in range(4):
        p = r.vptr(off + 64 + 4 * i)
        if p is not None:
            d['lods'].append([read_model(r, m) for m in ptr_collection(r, p) if m is not None])
    d['shader_group'] = read_shader_group(r, sg) if sg is not None else None
    d['skeleton_off'] = sk
    return d


def read_fragtype(r):
    f = {}
    dp = r.vptr(0xB4)
    f['drawable'] = read_drawable(r, dp) if dp is not None else None
    f['child_count'] = r.u8(0x1F3)
    lst = r.vptr(0xD4)
    children = []
    if lst is not None:
        for i in range(f['child_count']):
            c = r.vptr(lst + 4 * i)
            if c is None:
                continue
            cd = r.vptr(c + 0x90)
            children.append(dict(off=c, drawable=read_drawable(r, cd) if cd is not None else None))
    f['children'] = children
    return f


def summarize(r):
    print(f"{r.name}: big={r.big} type={r.type} virt={r.vsize} phys={r.psize}")
    f = read_fragtype(r)
    d = f['drawable']
    if d is None:
        print("  no drawable at 0xB4"); return r, f
    sg = d['shader_group']
    print(f"  drawable@{d['off']:X} center={tuple(round(x,3) for x in d['center'])}")
    print(f"  shaders: {len(sg['shaders']) if sg else None}")
    for s in (sg['shaders'] if sg else []):
        print(f"    {s['name']!s:32} params={s['param_count']}")
    for li, lod in enumerate(d['lods']):
        nv = sum(g['vertex_count'] for m in lod for g in m['geometries'])
        ng = sum(len(m['geometries']) for m in lod)
        print(f"  lod{li}: models={len(lod)} geometries={ng} vertices={nv}")
        decls = sorted({(g['decl']['usage'], g['decl']['stride'], g['decl']['types'])
                        for m in lod for g in m['geometries'] if g.get('decl')})
        for u, st, t in decls:
            print(f"    decl usage={u:08X} stride={st} types={t:016X}")
    print(f"  children: {f['child_count']}, with drawables: "
          f"{sum(1 for c in f['children'] if c['drawable'])}")
    return r, f
