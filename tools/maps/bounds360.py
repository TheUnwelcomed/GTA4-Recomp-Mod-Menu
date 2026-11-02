"""GTA IV static collision (.wbn, PC) to a console bound dictionary (.xbd).

A map mod's collision is one .wbn per model, in world space: a 0x20-byte
container pointing at a composite (type 12) of BVH meshes (type 10). The
console's props get their collision from a bound dictionary named after
their .ide (rubbish.ide / rubbish.xbd), keyed by model-name hash (sorted),
each value a bound in the model's own space. So each piece here is moved
into its model's space (the model's instance position subtracted: the
pieces this is for are placed unrotated) and written big-endian.

Layouts (checked against nj_01_4.xbn, rubbish.xbd and nj_01.xbd):
  dictionary   0x20: vtable, block map ptr, parent 0, usage 1,
               keys (ptr, u16 count, u16 cap), values (ptr, u16, u16)
  phBound      0x80: vtable, type u8 + 3 bytes, radius around the centroid,
               distance from the origin to its farthest point, vec4 max,
               min, centroid, CG offset, +0x50 vec4, +0x60 vec4,
               vec3 margin + 1
  composite    +0x80 children ptr, current matrices, last matrices,
               child AABBs (min, max vec4 each), u16 count, u16 cap
  BVH          +0x8C polygons (32 bytes: normal, area, u16 x 8), +0x90
               vec4 vertex quantum, +0xA0 vec4 offset, +0xB0 vertices
               (s16 x 3), +0xB8 bytes 01 CD CD CD, -1, +0xC8 vertex and
               polygon counts, +0xD0 materials (u32), +0xD8 u8 count, +0xE0
               the tree; 0xF0 bytes
  tree         0x60: nodes ptr, count, cap, pad, vec4 AABB min, max,
               quantization, its inverse, subtrees ptr, u16 count, u16 cap;
               nodes 16 bytes (u16 x 7, u8 count, pad), subtrees 16 (u16 x 8)

usage: python3 -I bounds360.py <out.xbd.bin> <flags.txt> <name:x:y:z> <piece.wbn> [...]
  each piece: its model name and the position it is moved from (its
  instance), then the PC file. Writes the dictionary's body (for
  xlzx rsc 32 <flags> <body> <out>) and its flags.
"""
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
import rsc5  # noqa: E402

NAN_A, NAN_B = 0x7F800001, 0x7FC00001
NEG_MAX = 0xFF7FFFFF
VT_DICT, VT_COMPOSITE, VT_BVH = 0xE8826100, 0x44EB6100, 0xF4F06100


def joaat(s):
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


# ------------------------------------------------------------------ reading
def vec(r, off):
    return list(struct.unpack_from('<3f', r.virt, off))


def read_bound(r, off):
    t = r.u8(off + 4)
    b = dict(type=t, radius=r.f32(off + 8), max=vec(r, off + 0x10), min=vec(r, off + 0x20),
             centroid=vec(r, off + 0x30), cg=vec(r, off + 0x40), dist=vec(r, off + 0x50),
             margin=vec(r, off + 0x70))
    if t == 12:
        n = r.u16(off + 0x90)
        kids, cur, boxes = r.vptr(off + 0x80), r.vptr(off + 0x84), r.vptr(off + 0x8C)
        b['children'] = [read_bound(r, r.vptr(kids + 4 * i)) for i in range(n)]
        b['matrices'] = [bytes(r.virt[cur + 64 * i:cur + 64 * i + 64]) for i in range(n)]
        b['boxes'] = [(vec(r, boxes + 32 * i), vec(r, boxes + 32 * i + 16)) for i in range(n)]
    elif t == 10:
        nvert, npoly = r.u32(off + 0xC8), r.u32(off + 0xCC)  # (vertices first: gtrcomm's 2729 polygons fill the space to its tree exactly)
        polys, verts = r.vptr(off + 0x8C), r.vptr(off + 0xB0)
        b['polygons'] = bytes(r.virt[polys:polys + 32 * npoly])
        b['vertices'] = bytes(r.virt[verts:verts + 6 * nvert])
        b['quantum'], b['offset'] = vec(r, off + 0x90), vec(r, off + 0xA0)
        mats, nm = r.vptr(off + 0xD0), r.u8(off + 0xD8)
        b['materials'] = [r.u32(mats + 4 * i) for i in range(nm)]
        t_ = r.vptr(off + 0xE0)
        nodes, nn = r.vptr(t_), r.u32(t_ + 4)
        subs, ns = r.vptr(t_ + 0x50), r.u16(t_ + 0x54)
        b['tree'] = dict(nodes=bytes(r.virt[nodes:nodes + 16 * nn]), subtrees=bytes(r.virt[subs:subs + 16 * ns]),
                         min=vec(r, t_ + 0x10), max=vec(r, t_ + 0x20), quant=vec(r, t_ + 0x30),
                         inv=vec(r, t_ + 0x40), count=nn, cap=r.u32(t_ + 8))
    else:
        raise ValueError('bound type %d not handled' % t)
    return b


def read_wbn(path):
    raw = open(path, 'rb').read()
    magic, rtype, flags = struct.unpack_from('<III', raw, 0)
    r = rsc5.Resource(zlib.decompress(raw[12:]), flags, False, rtype, path)
    return read_bound(r, r.vptr(8))


# ------------------------------------------------------------------ moving
def translate(b, d):
    for k in ('max', 'min', 'centroid'):
        b[k] = [b[k][i] + d[i] for i in range(3)]
    if b['type'] == 12:
        for c in b['children']:
            translate(c, d)
        b['boxes'] = [([lo[i] + d[i] for i in range(3)], [hi[i] + d[i] for i in range(3)]) for lo, hi in b['boxes']]
    else:
        b['offset'] = [b['offset'][i] + d[i] for i in range(3)]
        t = b['tree']
        t['min'] = [t['min'][i] + d[i] for i in range(3)]
        t['max'] = [t['max'][i] + d[i] for i in range(3)]


def farthest(b):
    return max(math.sqrt(x * x + y * y + z * z) for x in (b['min'][0], b['max'][0])
               for y in (b['min'][1], b['max'][1]) for z in (b['min'][2], b['max'][2]))


# ------------------------------------------------------------------ writing
def swap_polygons(raw):
    out = bytearray(len(raw))
    for o in range(0, len(raw), 32):
        f = struct.unpack_from('<4f', raw, o)
        h = struct.unpack_from('<8H', raw, o + 16)
        struct.pack_into('>4f8H', out, o, *f, *h)
    return bytes(out)


def swap_u16(raw):
    n = len(raw) // 2
    return struct.pack('>%dH' % n, *struct.unpack('<%dH' % n, raw[:2 * n]))


def swap_nodes(raw):
    out = bytearray(len(raw))
    for o in range(0, len(raw), 16):
        h = struct.unpack_from('<7H', raw, o)
        struct.pack_into('>7H', out, o, *h)
        out[o + 14:o + 16] = raw[o + 14:o + 16]
    return bytes(out)


class Arena:
    """The virtual segment, laid out front to back, 16-byte aligned; an
    object that would cross a load chunk (16 pages) starts the next one."""

    def __init__(self, chunk):
        self.buf = bytearray()
        self.chunk = chunk

    def alloc(self, size, align=16):
        o = (len(self.buf) + align - 1) & ~(align - 1)
        if self.chunk and size <= self.chunk and o // self.chunk != (o + size - 1) // self.chunk:
            o = (o // self.chunk + 1) * self.chunk
        self.buf += b'\xCD' * (o + size - len(self.buf))
        return o

    def put(self, data, align=16):
        o = self.alloc(len(data), align)
        self.buf[o:o + len(data)] = data
        return o


def ptr(o):
    return 0x50000000 | o


def vec4(v, w):
    return struct.pack('>3fI', v[0], v[1], v[2], w)


def header(b, vtable, type_bytes, w_max, w_min, w60):
    h = bytearray(0x80)
    struct.pack_into('>I', h, 0, vtable)
    h[4:8] = type_bytes
    struct.pack_into('>ff', h, 8, b['radius'], farthest(b))
    h[0x10:0x20] = vec4(b['max'], w_max)
    h[0x20:0x30] = vec4(b['min'], w_min)
    h[0x30:0x40] = vec4(b['centroid'], NAN_A)
    h[0x40:0x50] = vec4(b['cg'], NAN_A)
    h[0x50:0x60] = vec4(b['dist'], NAN_A)
    if w60 is None:
        struct.pack_into('>4I', h, 0x60, NEG_MAX, NEG_MAX, NEG_MAX, NEG_MAX)
    else:
        struct.pack_into('>3If', h, 0x60, NEG_MAX, NEG_MAX, NEG_MAX, w60)
    struct.pack_into('>3fI', h, 0x70, *b['margin'], 1)
    return h


def write_bound(a, b):
    """The bound and what it points at; returns its offset."""
    if b['type'] == 10:
        t = b['tree']
        polys = a.put(swap_polygons(b['polygons']))
        verts = a.put(swap_u16(b['vertices']))
        nodes = a.put(swap_nodes(t['nodes']))
        subs = a.put(swap_u16(t['subtrees']))
        mats = a.put(struct.pack('>%dI' % len(b['materials']), *b['materials']))
        th = bytearray(0x60)
        struct.pack_into('>III', th, 0, ptr(nodes), t['count'], t['count'])
        th[0x0C:0x10] = b'\xCD' * 4
        th[0x10:0x20] = vec4(t['min'], NAN_A)
        th[0x20:0x30] = vec4(t['max'], NAN_A)
        th[0x30:0x40] = vec4(t['quant'], NAN_B)
        th[0x40:0x50] = vec4(t['inv'], NAN_B)
        n_sub = len(t['subtrees']) // 16
        struct.pack_into('>IHH', th, 0x50, ptr(subs), n_sub, n_sub)
        th[0x58:0x60] = b'\xCD' * 8
        tree = a.put(bytes(th))
        size = [b['max'][i] - b['min'][i] for i in range(3)]
        # (+0x6C: the mesh's volume where it is known - collide.py works out the
        # signed volume the stock files carry - else its box's)
        h = header(b, VT_BVH, b'\x0A\x01\x01\x00', NAN_A, NAN_A, b.get('volume', size[0] * size[1] * size[2]))
        body = bytearray(0x70)
        struct.pack_into('>12x I', body, 0, ptr(polys))
        body[0x10:0x20] = vec4(b['quantum'], NAN_A)
        body[0x20:0x30] = vec4(b['offset'], NAN_A)
        struct.pack_into('>II', body, 0x30, ptr(verts), 0)
        body[0x38:0x3C] = b'\x01\xCD\xCD\xCD'
        struct.pack_into('>IIIIIII', body, 0x3C, 0xFFFFFFFF, 0, 0, len(b['vertices']) // 6,
                         len(b['polygons']) // 32, ptr(mats), 0)
        body[0x58] = len(b['materials'])
        struct.pack_into('>I', body, 0x60, ptr(tree))
        return a.put(bytes(h) + bytes(body))
    kids = [write_bound(a, c) for c in b['children']]
    n = len(kids)
    kp = a.put(struct.pack('>%dI' % n, *[ptr(k) for k in kids]))
    mats = a.put(b''.join(m[:12] + struct.pack('>I', NAN_A) + _swap_matrix(m)[16:] if False else _swap_matrix(m)
                          for m in b['matrices']))
    boxes = a.put(b''.join(vec4(lo, NAN_B) + vec4(hi, NAN_B) for lo, hi in b['boxes']))
    h = header(b, VT_COMPOSITE, b'\x0C\x01\x01\x00', NAN_B, NAN_B, None)
    body = struct.pack('>IIIIHH', ptr(kp), ptr(mats), ptr(mats), ptr(boxes), n, n) + b'\x00' * 12
    return a.put(bytes(h) + body)


def _swap_matrix(m):
    """A child's 4x4 matrix (rows of x, y, z, w), the w words kept as the
    console's NaN markers."""
    out = bytearray(64)
    for row in range(4):
        x, y, z = struct.unpack_from('<3f', m, 16 * row)
        struct.pack_into('>3fI', out, 16 * row, x, y, z, NAN_A)
    return bytes(out)


def build_dictionary(entries):
    """entries: [(name, bound)]; returns (body bytes, flags)."""
    entries = sorted(entries, key=lambda e: joaat(e[0]))
    total_guess = sum(len(b.get('polygons', b'')) + len(b.get('vertices', b'')) for _, b in entries)
    # pages: the smallest size with at most 63 of them (as the stock tools pack)
    shift = 4
    while (max(total_guess * 2, 4096) + (256 << shift) - 1) // (256 << shift) > 48:
        shift += 1
    page = 256 << shift
    a = Arena(16 * page)
    a.put(b'\x00' * 0x20)  # (the root, filled in last)
    offs = [write_bound(a, b) for _, b in entries]
    n = len(entries)
    keys = a.put(struct.pack('>%dI' % n, *[joaat(name) for name, _ in entries]))
    vals = a.put(struct.pack('>%dI' % n, *[ptr(o) for o in offs]))
    blockmap = a.put(b'\x00\x00\x00\x00' + b'\xCD' * 0x2C)
    root = struct.pack('>IIIIIHHIHH', VT_DICT, ptr(blockmap), 0, 1, ptr(keys), n, n, ptr(vals), n, n)
    a.buf[0:0x20] = root
    size = len(a.buf)
    count = (size + page - 1) // page
    assert count <= 0x3F, (size, page)
    body = bytes(a.buf) + b'\xCD' * (count * page - size)
    flags = 0xC0000000 | count | (shift << 11)
    assert rsc5.seg_sizes(flags) == (count * page, 0)
    return body, flags


def check(body, flags, entries):
    """Read the dictionary back (big-endian) and compare with what went in."""
    r = rsc5.Resource(body, flags, True, 32, 'xbd')
    n = r.u16(0x14)
    keys = [r.u32(r.vptr(0x10) + 4 * i) for i in range(n)]
    assert keys == sorted(keys) and len(set(keys)) == n
    by_key = {joaat(name): b for name, b in entries}
    for i, k in enumerate(keys):
        off = r.vptr(r.vptr(0x18) + 4 * i)
        b = by_key[k]

        def cmp(off, b):
            t = r.u8(off + 4)
            assert t == b['type']
            mx = struct.unpack_from('>3f', r.virt, off + 0x10)
            assert all(abs(mx[j] - b['max'][j]) < 1e-3 for j in range(3))
            if t == 12:
                kids = r.vptr(off + 0x80)
                for j, c in enumerate(b['children']):
                    cmp(r.vptr(kids + 4 * j), c)
            else:
                nvert, npoly = r.u32(off + 0xC8), r.u32(off + 0xCC)
                assert npoly * 32 == len(b['polygons']) and nvert * 6 == len(b['vertices'])
                vp = r.vptr(off + 0xB0)
                q = struct.unpack_from('>3f', r.virt, off + 0x90)
                o = struct.unpack_from('>3f', r.virt, off + 0xA0)
                # vertices: back to positions, against the source moved
                for v in range(0, nvert, max(1, nvert // 50)):
                    s_new = struct.unpack_from('>3h', r.virt, vp + 6 * v)
                    s_old = struct.unpack_from('<3h', b['vertices'], 6 * v)
                    assert s_new == s_old
                    p = [s_new[j] * q[j] + o[j] for j in range(3)]
                    assert all(b['min'][j] - 1.0 <= p[j] <= b['max'][j] + 1.0 for j in range(3)), (p, b['min'], b['max'])
                pp = r.vptr(off + 0x8C)
                for pi in range(0, npoly, max(1, npoly // 50)):
                    idx = struct.unpack_from('>3H', r.virt, pp + 32 * pi + 16)
                    assert all(x < nvert for x in idx), idx
                tr = r.vptr(off + 0xE0)
                assert r.u32(tr + 4) * 16 == len(b['tree']['nodes'])
        cmp(off, b)
    return n


def main():
    out, flags_out = sys.argv[1], sys.argv[2]
    entries = []
    for i in range(3, len(sys.argv), 2):
        name, x, y, z = sys.argv[i].split(':')
        b = read_wbn(sys.argv[i + 1])
        if b['type'] == 12 and len(b['children']) == 1:
            b = b['children'][0]  # (one mesh: the mesh itself, as the console's props have it)
        translate(b, [-float(x), -float(y), -float(z)])
        entries.append((name, b))
    body, flags = build_dictionary(entries)
    n = check(body, flags, entries)
    open(out, 'wb').write(body)
    open(flags_out, 'w').write('0x%08X\n' % flags)
    print('%s: %d bounds, %d bytes, flags %08X' % (out, n, len(body), flags))


if __name__ == '__main__':
    main()
