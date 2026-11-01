"""PC GTA IV vehicle -> Xbox 360 vehicle, by transplant into a stock 360 file.

Inputs:  stock 360 template (decoded fbi.xft/.xtd bodies + their RSC headers)
         PC donor vehicle (.wft/.wtd)
Output:  360 bodies + RSC flags for fbi.xft and fbi.xtd (pack with xlzx rsc).

Everything structural that this does was verified against the stock FBI,
which exists on both platforms (see the verification log in the repo):
  vertex element encodings, texture tiling/endian, skeleton/matrix parity,
  shader parameter subsetting, VB/IB/D3D header layouts.

The 360 template keeps: physics, fragment groups/children, skeleton topology
and rotations, all other children. The donor provides: main drawable LODs,
the wheel child's LODs, shaders (cloned from same-named 360 shaders - the
template's, or another stock car's - values by name hash), textures (merged
into the .xtd), bounds, and the positions of the template's point bones
(wheels and suspension, seats, lamps, exhausts), moved consistently in every
copy of the skeleton's pose (see "the transplant" below).
"""
import math
import os
import re
import struct
import sys

import rsc5
import dxt  # noqa: E402
import xenos

VP = 0x50000000
PP = 0x60000000

# donor shader (.sps) -> template shader (.sps) when the template lacks it
SPS_FALLBACK = {
    'gta_vehicle_interior.sps': 'gta_vehicle_interior2.sps',
    'gta_default.sps': 'gta_vehicle_interior2.sps',  # needs 0059, as the donor part supplies
    # Two- and three-colour paints: the template car only carries the
    # one-colour paint; values shared by name carry over, the body takes the
    # vehicle's primary colour.
    'gta_vehicle_paint2.sps': 'gta_vehicle_paint1.sps',
    'gta_vehicle_paint3.sps': 'gta_vehicle_paint1.sps',
    'gta_vehicle_generic.sps': 'gta_vehicle_mesh.sps',
    # (the Batwing's cockpit glass: a mod's own name for a glass shader)
    'gta_vehicle_glass_shades_lights.sps': 'gta_vehicle_vehglass.sps',
}
# Texture slots of the vehicle shaders, by their name hashes: the colour map,
# and the bump, specular and dirt maps a mod may fill with it.
SAMPLER_TEXTURE = 0x2B5170FD                       # TextureSampler
SAMPLER_PLACEHOLDER = {0x46B7C64F: 'bump',         # BumpSampler
                       0x608799C6: 'spec',         # SpecSampler
                       0x7E9A27FE: 'dirt'}         # DirtSampler (always the template's)
BONE_ALIAS = {'chasiss': 'chassis'}  # a typo in the Batmobile's skeleton
# Shaders the game drives per car (the body colours, lights on and off, the
# glass, the tyres), in the order they are moved to the front of the shader
# group when one would sit past slot 11. Stock cars keep them in the first 12
# slots (paint at 0, or at 10-11 on the police car and taxi); the game left
# the Dominator 645's paint white at slot 14 while the Deimos's at slot 7
# took its colour.
EFFECT_SHADERS = ('gta_vehicle_paint', 'gta_vehicle_lightsemissive', 'gta_vehicle_lights',
                  'gta_vehicle_vehglass', 'gta_vehicle_tire')
STOCK_SHADER_SLOTS = 12
# A donor bone the template has no bone for is drawn rigidly on the chassis
# (vertices are stored in model space, so a part keeps its place; it only
# loses its own motion).
BONE_FALLBACK = 'chassis'
# A template wheel the donor lacks takes the place of the donor's opposite
# wheel - a tricycle layout's nose wheel, say, fills both front slots.
WHEEL_STAND_IN = {'wheel_rf': 'wheel_lf', 'wheel_lf': 'wheel_rf',
                  'wheel_rr': 'wheel_lr', 'wheel_lr': 'wheel_rr'}

TYPES_360 = 0xAA1111111199A996  # pos f3, bw/bi/col c4, normal dec3n, uv half2, tangent/binormal dec3n
SIZE_360 = {0: 12, 1: 4, 2: 4, 3: 4, 4: 4, 5: 4, 14: 4, 15: 4}
SIZE_PC = {0: 12, 1: 4, 2: 4, 3: 12, 4: 4, 5: 4, 14: 16, 15: 12}
for _i in range(6, 14):
    SIZE_360[_i] = 4
    SIZE_PC[_i] = 8
PC_TYPES = 0x6755555555996996


def joaat(s):
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def align(v, a):
    return (v + a - 1) // a * a


def swap_words(b):
    """LE 32-bit words -> BE 32-bit words."""
    n = len(b) // 4
    return struct.pack('>%dI' % n, *struct.unpack('<%dI' % n, b[:n * 4]))


def encode_flags(template_flags, vsize, psize):
    """RSC5 size flags with the 6-bit counts the stock tools emit."""
    def enc(size):
        size = align(max(size, 4096), 4096)  # whole pages, as every stock file has
        b = 0
        while -(-size // (1 << (b + 8))) > 0x3F:
            b += 1
        a = -(-size // (1 << (b + 8)))
        return a, b, a << (b + 8)
    va, vb, vtot = enc(vsize)
    pa, pb, ptot = enc(psize)
    flags = (template_flags & 0xC0000000) | va | (vb << 11) | (pa << 15) | (pb << 26)
    assert rsc5.seg_sizes(flags) == (vtot, ptot)
    return flags, vtot, ptot


# The console loads a resource's virtual segment page by page (4 KiB pages
# for these files: 256 << the flags' shift), each page wherever the
# allocator finds room, and translates every pointer page by page. So no
# object reached through a pointer may straddle a page boundary - the rule
# Rockstar's packer keeps (checked on 15 stock 360 cars). An object that
# would cross starts on the next page instead.
VIRTUAL_PAGE = 4096


# The physical (GPU) segment is loaded in chunks, not as one block: the
# resource map (sub_8285C790) cuts each segment into as many 16-page chunks as
# it holds, then the rest into power-of-two chunks, largest first, and each
# chunk goes wherever the allocator finds room. Pointers are translated chunk
# by chunk, so a buffer running past its chunk's end reads whatever lies after
# that chunk in memory: v1.7.5's huge stretched polygons, vertex data whose
# tail was another allocation (the PS5 log's "changed without an unlock"
# buffers each crossed exactly such an edge). Rockstar's packer keeps every
# buffer and texture inside one chunk (15 stock cars: none crosses); so does
# pack_physical.
def load_chunks(count, page):
    """(start, end) of each chunk a segment of `count` pages is loaded in."""
    total, big = count * page, 16 * page
    out, off = [], 0
    for _ in range(total // big):
        out.append((off, off + big))
        off += big
    rest = total - off
    while rest:
        size = 1 << (rest.bit_length() - 1)
        out.append((off, off + size))
        off += size
        rest -= size
    return out


def segment_chunks(flags, physical=True):
    shift = (flags >> 26) & 0xF if physical else (flags >> 11) & 0xF
    count = (flags >> 15) & 0x7FF if physical else flags & 0x7FF
    return load_chunks(count, 256 << shift)


# Chunks as small as the biggest object allows. Stock cars load in 128 and
# 256 KiB chunks; v1.7.7's add-ons were packed for the smallest file and came
# out in chunks of up to 2 MiB, each one allocation the console's streaming
# heap must find whole. Fresh after a launch it could; once the world had been
# streaming a while, often not: a second spawn of an add-on "did not load
# within 20 s" (log 053), the first Batmobile took 17 s. The resource map
# (sub_8285C790's callers keep it on the stack) holds 128 chunks, virtual ones
# included, and the page count has 11 bits (Rockstar's own files go past 63).
MIN_PHYSICAL_PAGE = 16384      # 256 KiB chunks, the stock cars' size
MAX_PHYSICAL_CHUNKS = 100
# Biggest vertex buffer one converted geometry keeps. A PC mod's single
# geometry can hold far more (the Lazar 3's hull: 29,795 vertices, 1.9 MiB,
# which alone forced 2 MiB chunks); a bigger one is drawn as several
# geometries, so the segment still loads in the stock cars' chunk sizes.
MAX_VERTEX_BYTES = 224 << 10


def split_geometry(c, limit=MAX_VERTEX_BYTES):
    """A converted geometry (Converter.convert_geometry) as parts whose vertex
    data fit `limit` bytes: its triangles in order, each part holding the
    vertices its triangles use (in first-use order) with the same layout,
    declaration and bone table. One part when it already fits."""
    if c['vbytes'] <= limit:
        return [c]
    stride, most = c['stride'], limit // c['stride']
    idx = struct.unpack('>%dH' % c['icount'], c['idata'])
    parts, cur, remap = [], [], {}

    def close():
        verts = sorted(remap, key=remap.get)
        vdata = b''.join(c['vdata'][v * stride:(v + 1) * stride] for v in verts)
        idata = struct.pack('>%dH' % len(cur), *cur)
        parts.append(dict(c, vcount=len(verts), vdata=vdata, vbytes=len(vdata), icount=len(cur),
                          idata=idata, ibytes=len(idata), bones=list(c['bones'])))

    for t in range(0, len(idx), 3):
        tri = idx[t:t + 3]
        if len(remap) + len(set(v for v in tri if v not in remap)) > most:
            close()
            cur, remap = [], {}
        for v in tri:
            cur.append(remap.setdefault(v, len(remap)))
    close()
    return parts


def pack_physical(objects, fixed_end=0, fixed_pieces=(), max_chunks=MAX_PHYSICAL_CHUNKS):
    """Offsets for `objects` [(size, alignment)] so that each lies inside one
    load chunk, in the smallest chunks that hold the biggest object (pages of
    at least MIN_PHYSICAL_PAGE), then the fewest pages. `fixed_end`: bytes
    [0, fixed_end) already taken (a template's data kept in place), made of
    `fixed_pieces` [(start, end)] that must each stay inside one chunk too.
    Returns (offsets, count, shift, total)."""
    order = sorted(range(len(objects)), key=lambda i: (-objects[i][0], i))
    need = fixed_end + sum(s for s, _ in objects)
    biggest = max([s for s, _ in objects] + [e - s for s, e in fixed_pieces] + [1])
    for shift in range(4, 16):
        page = 256 << shift
        if 16 * page < biggest or page < MIN_PHYSICAL_PAGE:
            continue
        for count in range(max(1, -(-need // page)), 0x7FF + 1):
            chunks = load_chunks(count, page)
            if len(chunks) > max_chunks:
                break
            if any(not any(cs <= s and e <= ce for cs, ce in chunks) for s, e in fixed_pieces):
                continue
            fill = [max(cs, min(ce, fixed_end)) for cs, ce in chunks]
            offs = [None] * len(objects)
            for i in order:
                size, a = objects[i]
                for k, (cs, ce) in enumerate(chunks):
                    o = align(fill[k], a)
                    if o + size <= ce:
                        offs[i] = o
                        fill[k] = o + size
                        break
                else:
                    break
            else:
                return offs, count, shift, count * page
    raise AssertionError('no physical layout keeps every buffer inside one load chunk')


def place_physical(objects, fixed=b'', fixed_pieces=()):
    """Pack `objects` [(bytes, alignment)] after `fixed`; returns (segment
    bytes, offsets, count, shift)."""
    offs, count, shift, total = pack_physical([(len(b), a) for b, a in objects], len(fixed), fixed_pieces)
    buf = bytearray(total)
    buf[:len(fixed)] = fixed
    for (b, _), o in zip(objects, offs):
        buf[o:o + len(b)] = b
    return bytes(buf), offs, count, shift


def make_flags(template_flags, vsize, pcount, pshift):
    """RSC5 flags: the virtual size as encode_flags packs it, the physical
    page count and shift as pack_physical chose them."""
    flags, vtot, _ = encode_flags(template_flags, vsize, 4096)
    flags = (flags & ~((0x7FF << 15) | (0xF << 26))) | (pcount << 15) | (pshift << 26)
    return flags, vtot, pcount * (256 << pshift)


class Seg:
    def __init__(self, data=b'', page=0):
        self.buf = bytearray(data)
        self.page = page  # 0: no paging rule (physical data is laid out by pack_physical)

    def alloc(self, size, alignment=16, phase=0):
        """`phase`: the offset must equal this modulo `alignment`."""
        off = len(self.buf)
        off += (phase - off) % alignment
        if self.page and size <= self.page and off // self.page != (off + size - 1) // self.page:
            off = (off // self.page + 1) * self.page
            off += (phase - off) % alignment
        self.buf += b'\0' * (off + size - len(self.buf))
        return off

    def put(self, data, alignment=16):
        off = self.alloc(len(data), alignment)
        self.buf[off:off + len(data)] = data
        return off

    def w32(self, off, v):
        struct.pack_into('>I', self.buf, off, v & 0xFFFFFFFF)

    def w16(self, off, v):
        struct.pack_into('>H', self.buf, off, v & 0xFFFF)

    def cstr(self, s):
        return self.put(s.encode('latin-1') + b'\0', 4)


# ---------------------------------------------------------------- vertices
def _half(v):
    """float -> half, truncating toward zero, as Rockstar's 360 tools do
    (identity test: every UV differing from theirs was one ulp further out)."""
    b = struct.unpack('<I', struct.pack('<f', v))[0]
    s = (b >> 16) & 0x8000
    e = ((b >> 23) & 0xFF) - 112
    m = b & 0x7FFFFF
    if ((b >> 23) & 0xFF) == 0xFF or e >= 31:
        return s | 0x7BFF
    if e <= 0:
        if e < -10:
            return s
        return s | (((m | 0x800000) >> (1 - e)) >> 13)
    return s | (e << 10) | (m >> 13)


def _dec3n(x, y, z, w=0):
    def q(v):  # truncation toward zero, matching Rockstar's packing
        return max(-511, min(511, int(v * 511.0))) & 0x3FF
    return q(x) | (q(y) << 10) | (q(z) << 20) | ((int(w) & 3) << 30)


def vertex_layout(usage, sizes):
    out, off = [], 0
    for i in range(16):
        if usage & (1 << i):
            out.append((i, off))
            off += sizes[i]
    return out, off


UV_BITS = 0x3FC0  # texcoord sets 0-7 (usage bits 6-13)


def unit(x, y, z):
    """A direction for 10:10:10 packing: some exporters leave the odd normal
    unnormalised (lengths of 100 seen), which would wrap the 10-bit fields;
    those are normalised, everything already in range is left exactly as is."""
    if max(abs(x), abs(y), abs(z)) <= 1.0:
        return x, y, z
    n = (x * x + y * y + z * z) ** 0.5
    return (x / n, y / n, z / n) if n > 0 else (0.0, 0.0, 1.0)


def convert_vertices(src, usage, count, pc_stride, fill_uv=0):
    """PC vertices -> 360 vertices. `fill_uv`: texcoord bits the target
    shader reads but the donor lacks, written as copies of texcoord 0."""
    pc_elems, pc_size = vertex_layout(usage, SIZE_PC)
    x_elems, x_size = vertex_layout(usage | fill_uv, SIZE_360)
    assert pc_size == pc_stride, (hex(usage), pc_size, pc_stride)
    assert not fill_uv or (usage & 0x40 and not fill_uv & ~UV_BITS & 0xFFFF)
    x_at = dict(x_elems)
    pc_at = dict(pc_elems)
    out = bytearray(x_size * count)
    for v in range(count):
        base = v * pc_stride
        obase = v * x_size
        for i in range(16):
            if fill_uv & (1 << i):
                u, t = struct.unpack_from('<2f', src, base + pc_at[6])
                struct.pack_into('>HH', out, obase + x_at[i], _half(u), _half(t))
        for (i, po) in pc_elems:
            xo = x_at[i]
            p = base + po
            o = obase + xo
            if i == 0:
                out[o:o + 12] = struct.pack('>3f', *struct.unpack_from('<3f', src, p))
            elif i in (1, 2, 4, 5):
                out[o:o + 4] = src[p:p + 4][::-1]
            elif i in (3, 15):
                x, y, z = unit(*struct.unpack_from('<3f', src, p))
                struct.pack_into('>I', out, o, _dec3n(x, y, z, 0))
            elif i == 14:
                x, y, z, w = struct.unpack_from('<4f', src, p)
                x, y, z = unit(x, y, z)
                struct.pack_into('>I', out, o, _dec3n(x, y, z, -1 if w < 0 else 1))
            else:
                u, t = struct.unpack_from('<2f', src, p)
                struct.pack_into('>HH', out, o, _half(u), _half(t))
    return bytes(out), x_size


# ---------------------------------------------------------------- reading
def model_collection(r, coll_off):
    return [m for m in rsc5.ptr_collection(r, coll_off) if m is not None]


def lod_collections(r, drawable_off):
    return [r.vptr(drawable_off + 64 + 4 * i) for i in range(4)]


def shader_params(r, s):
    b = s + 8
    n = r.u32(b + 0x14)
    po, pt, pn = r.vptr(b + 12), r.vptr(b + 28), r.vptr(b + 44)
    out = []
    for i in range(n):
        out.append(dict(hash=r.u32(pn + 4 * i), type=r.u8(pt + i),
                        value=r.vptr(po + 4 * i)))
    return out


def shader_sps(r, s):
    p = r.vptr(s + 0x48)
    return r.cstr(p) if p is not None else None


def bones(r, skel):
    bo = r.vptr(skel)
    n = r.u16(skel + 20)
    return bo, [r.cstr(r.vptr(bo + 0xE0 * i)) for i in range(n)]


# ---------------------------------------------------------------- the transplant
#
# How the game poses a car (worked out on the console files and the game code):
#  - The skeleton data holds each bone's local translation (+0x20), its
#    position in the car (+0x60), and three matrix arrays: global rotations
#    (+8), their inverses (+12) and the local matrices (+16).
#  - The fragment also carries two ready-made poses of the whole skeleton
#    (fragType +0x1DC and +0x1E0: locals, 0x50 apart, and globals, 0x40 apart).
#    A spawned car starts from those, and the physics parts (fragment
#    children: doors, bonnet, glass, bumpers, wheels) are placed from the
#    collision composite's part matrices.
#  - Skinning draws a vertex with (the bone's current matrix) x (the inverse
#    of its rest pose). Every copy of a bone's rest pose must therefore agree,
#    or the parts on that bone are drawn displaced: v1.7.4 moved bones in the
#    skeleton data only, and a part could land metres away (the Batmobile's
#    door bones sit 13 m under the car in the mod).
#  - The game hides a bone (an extra switched off, a part broken off, a
#    smashed window or lamp) by overwriting its local matrix with a constant
#    that sends it, and every bone under it, to the world's origin. A
#    triangle with one corner there is a long black sliver across the map.
#  - Doors, the bonnet and the boot hang on joints, built from the skeleton
#    when they swing: the bone's position is the hinge, its rotation limits
#    (+0xB0 min, +0xC0 max, radians per axis; a free bone has -pi..pi) the
#    axis and travel. Each physics part's fragment child keeps the inverse of
#    its bone's rest matrix (+0x10) and its link's centre of gravity (+0x50:
#    the body's, or a door's own), and fragType +0xF8 holds each part's
#    offset from that centre; every stock car keeps these consistent with the
#    skeleton (checked on the template before anything moves).
#
# So the donor's geometry rides only on bones the game never hides or poses
# from physics parts of a different car: the chassis, the body shell, the
# wheel assembly (moved, consistently, to the donor's wheels), a pivot bone
# the trainer turns (pop-up lamps), and a door: the template's door joint
# moved to the donor's hinge, so it swings the donor's door the way the mod
# hangs it. A door is hidden only when it breaks off, and then the game draws
# it (from the same drawable) on its own. Everything else is drawn rigid on
# the chassis: it stays where the modder put it.

# Template bones moved to the donor's position (where the game reads a point
# from the skeleton: wheels and suspension, seats, lamps, exhausts, smoke).
POINT_PREFIXES = ('wheel_', 'hub_', 'suspension_', 'seat_', 'headlight_', 'taillight_',
                  'brakelight_', 'indicator_', 'reversinglight_', 'exhaust', 'overheat', 'siren')
POINT_NAMES = {'engine', 'interiorlight', 'petrolcap', 'steeringwheel'}
# Bones the donor's geometry may stay on: never hidden, never posed by a
# collision part of the template's own shape.
CARRY_PREFIXES = ('wheel_', 'hub_', 'suspension_')
CARRY_NAMES = {'chassis', 'bodyshell'}
# A donor bone further than this outside its own model is a parking spot
# (the Batmobile keeps its unused door bones 13 m below the car).
POINT_MARGIN = 0.6
# Doors the donor's own door (and what hangs under it: its windows) swings
# on. The bonnet and boot stay rigid: mods hinge them at either end (the
# Deimos's engine cover, the Dominator's bonnet at the windscreen), where the
# template's lid, collision and all, would hang the wrong way round.
DOOR_NAMES = ('door_dside_f', 'door_pside_f', 'door_dside_r', 'door_pside_r')


def is_point(name):
    return name.startswith(POINT_PREFIXES) or name in POINT_NAMES


def carries(name):
    return name.startswith(CARRY_PREFIXES) or name in CARRY_NAMES


def v_mat(v, m):
    """Row vector times 3x3 (rows are the basis vectors, as RAGE stores them)."""
    return tuple(v[0] * m[0][j] + v[1] * m[1][j] + v[2] * m[2][j] for j in range(3))


def m_mul(a, b):
    return [v_mat(a[i], b) for i in range(3)]


def m_t(m):
    return [tuple(m[j][i] for j in range(3)) for i in range(3)]


def v_sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def v_add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def close(a, b, tol=2e-3):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def is_identity(m, tol=1e-3):
    return all(abs(m[k][j] - (k == j)) <= tol for k in range(3) for j in range(3))


def bone_limits(r, rec):
    """A bone's rotation limits: (min xyz, max xyz) in radians."""
    return (struct.unpack_from(r.e + '3f', r.virt, rec + 0xB0),
            struct.unpack_from(r.e + '3f', r.virt, rec + 0xC0))


def is_joint(limits):
    """A bone whose rotation is limited on some axis hangs a part on a joint
    (door, bonnet, boot); a free bone has -pi..pi on all three."""
    mn, mx = limits
    return any(mx[k] - mn[k] < 6.28 for k in range(3))


def joint_axes(limits):
    """The axes a joint turns about (a door: just z)."""
    mn, mx = limits
    return tuple(k for k in range(3) if 1e-4 < mx[k] - mn[k] < 6.28)


def rows(r, off, k=4):
    """A matrix as rows (rotation rows, then the translation)."""
    return [struct.unpack_from(r.e + '3f', r.virt, off + 16 * i) for i in range(k)]


def read_skeleton(r, sk):
    """Bones of a skeleton: name, parent, local translation and rotation,
    stored position and global rotation."""
    e = r.e
    bo, n = r.vptr(sk), r.u16(sk + 20)
    garr, larr = r.vptr(sk + 8), r.vptr(sk + 16)
    out = []
    for i in range(n):
        b = bo + 0xE0 * i
        par = r.u32(b + 0x10)
        pi = ((par & 0x0FFFFFFF) - bo) // 0xE0 if par else -1
        out.append(dict(
            name=r.cstr(r.vptr(b)), parent=pi, rec=b,
            t=struct.unpack_from(e + '3f', r.virt, b + 0x20),
            pos=struct.unpack_from(e + '3f', r.virt, b + 0x60),
            L=[struct.unpack_from(e + '3f', r.virt, larr + 64 * i + 16 * k) for k in range(3)],
            G=[struct.unpack_from(e + '3f', r.virt, garr + 64 * i + 16 * k) for k in range(3)]))
    return bo, out


def pose(bones, translations):
    """Global positions and rotations from local translations (and the
    bones' own local rotations), parents first."""
    pos, rot = [None] * len(bones), [None] * len(bones)
    for i, b in enumerate(bones):
        p = b['parent']
        assert p < i, 'bone %s comes before its parent' % b['name']
        if p < 0:
            rot[i], pos[i] = b['L'], translations[i]
        else:
            rot[i] = m_mul(b['L'], rot[p])
            pos[i] = v_add(v_mat(translations[i], rot[p]), pos[p])
    return pos, rot


class Cond:
    """A source's condition: half-spaces of the car's space, all to be met.
    `faces`: it picks whole triangles (by their centre) rather than vertices,
    and a vertex a picked triangle shares with one left behind is copied, so
    the part comes away cleanly when it turns (a canopy cut out of a body
    modelled in one piece)."""

    def __init__(self, terms, faces=False):
        self.terms = terms  # [((a, b, c), op, value)]: a x + b y + c z  op  value
        self.faces = faces

    def __call__(self, pos):
        ops = {'>': float.__gt__, '>=': float.__ge__, '<': float.__lt__, '<=': float.__le__}
        return all(ops[op](float(k[0] * pos[0] + k[1] * pos[1] + k[2] * pos[2]), float(v))
                   for k, op, v in self.terms)

    def __repr__(self):
        return ('faces:' if self.faces else '') + '&'.join(
            '%s%s%g' % ('+'.join('%g%s' % (c, 'xyz'[i]) for i, c in enumerate(k) if c), op, v)
            for k, op, v in self.terms)


def _linear(text):
    """'y+0.62z' -> (0, 1, 0.62); '-x', '2*y - 0.5 z' likewise."""
    k = [0.0, 0.0, 0.0]
    t = text.replace(' ', '').replace('*', '')
    if not t:
        raise ValueError('empty condition')
    if t[0] not in '+-':
        t = '+' + t
    term, terms = '', []
    for ch in t:
        if ch in '+-' and term:
            terms.append(term)
            term = ''
        term += ch
    terms.append(term)
    for term in terms:
        axis = term[-1]
        if axis not in 'xyz':
            raise ValueError('bad term %r in %r' % (term, text))
        num = term[:-1]
        coef = 1.0 if num in ('+', '') else -1.0 if num == '-' else float(num)
        k['xyz'.index(axis)] += coef
    return tuple(k)


def parse_cond(text):
    faces = text.startswith('faces:')
    if faces:
        text = text[len('faces:'):]
    terms = []
    for part in text.split('&'):
        for op in ('>=', '<=', '>', '<'):
            if op in part:
                lhs, value = part.split(op)
                terms.append((_linear(lhs), op, float(value)))
                break
        else:
            raise ValueError('bad condition %r' % part)
    return Cond(terms, faces)


def parse_pivots(spec):
    """Pivot spec -> {template bone: {'hinge': (x, y, z) or None,
    'sources': [(donor bone, Cond or None)]}}.

    spec   := entry (';' entry)*
    entry  := template_bone ['@' x ',' y ',' z] '=' source (',' source)*
    source := donor_bone [':' ['faces:'] cond ('&' cond)*]
    cond   := linear op value     (linear: terms in x y z, e.g. y, -x, y+0.62z;
                                   op > >= < <=)

    A plain source binds all of the donor bone's geometry to the pivot; one
    with conditions only the vertices (in the car's space) that meet them all,
    so one donor bone can be split between pivots (the F-16's nose gear and
    left main gear share the mod's `boot`); with `faces:`, the triangles whose
    centres meet them, their shared corners copied (the Batwing's canopy, cut
    out of its one-piece body). The pivot bone goes to the hinge when one is
    given, else to the first source bone's position."""
    out = {}
    for entry in filter(None, (spec or '').split(';')):
        head, srcs = entry.split('=', 1)
        hinge = None
        if '@' in head:
            head, at = head.split('@')
            hinge = tuple(float(c) for c in at.split(','))
            assert len(hinge) == 3, entry
        sources = []
        for src in srcs.split(','):
            if ':' not in src:
                sources.append((src, None))
                continue
            name, cond = src.split(':', 1)
            sources.append((name, parse_cond(cond)))
        out[head] = dict(hinge=hinge, sources=sources)
    return out


def meets(pos, cond):
    if isinstance(cond, Cond):
        return cond(pos)
    axis, op, value = cond
    x = pos['xyz'.index(axis)]
    return {'>': x > value, '>=': x >= value, '<': x < value, '<=': x <= value}[op]


def main_influence(vsrc, at, v, stride):
    """The bone-table slot of a vertex's strongest influence."""
    o = v * stride
    if 2 not in at:
        return 0
    if 1 not in at:
        return vsrc[o + at[2]]
    w = vsrc[o + at[1]:o + at[1] + 4]
    k = max(range(4), key=lambda q: w[q])
    return vsrc[o + at[2] + k]


def face_cut(pos, bone_of, idx, rules):
    """Triangles moved to pivots whole. pos: vertex positions; bone_of: each
    vertex's donor bone; idx: the index list; rules: [(donor bone, Cond, key)].
    A triangle whose three corners ride the rule's donor bone and whose centre
    meets its condition goes to `key` (the first rule that takes it). Returns
    (new index list, dup, assign): `dup` lists, for each vertex appended,
    the vertex it copies; `assign` maps vertices (original or appended) to
    the key they now ride. A vertex used only by moved triangles moves
    itself; one shared with triangles staying behind (or moving elsewhere)
    is copied for the moved ones, so no triangle is stretched."""
    n = len(pos)
    tri_key = []
    for t in range(0, len(idx), 3):
        a, b, c = idx[t:t + 3]
        key = None
        for donor_bone, cond, k in rules:
            if bone_of[a] == bone_of[b] == bone_of[c] == donor_bone:
                centre = [(pos[a][i] + pos[b][i] + pos[c][i]) / 3.0 for i in range(3)]
                if cond(centre):
                    key = k
                    break
        tri_key.append(key)
    users = {}
    for t, key in enumerate(tri_key):
        for v in idx[3 * t:3 * t + 3]:
            users.setdefault(v, [])
            if key not in users[v]:
                users[v].append(key)
    home, assign, dup = {}, {}, []  # (vertex, key) -> the vertex carrying it
    for v in sorted(users):
        keys = users[v]
        stay = None if None in keys else keys[0]  # the original keeps this one
        for key in keys:
            if key == stay:
                home[(v, key)] = v
                if key is not None:
                    assign[v] = key
            else:
                home[(v, key)] = n + len(dup)
                assign[n + len(dup)] = key
                dup.append(v)
    out = list(idx)
    for t, key in enumerate(tri_key):
        if key is not None:
            for j in range(3):
                out[3 * t + j] = home[(idx[3 * t + j], key)]
    return out, dup, assign


class Converter:
    def __init__(self, tmpl, donor, log=print, pivots=None, shader_sources=(), options=None):
        """`pivots`: parse_pivots() output (or {template bone: [donor bones]}):
        the donor bones' geometry is bound to that template bone, moved to the
        hinge; the trainer turns it (pop-up lamps, landing gear).
        `shader_sources`: other stock 360 cars to take a shader from when the
        template lacks the donor's (paint2/paint3 live on the airtug, police,
        taxi...)."""
        self.pivots = {k: (v if isinstance(v, dict) else dict(hinge=None, sources=[(b, None) for b in v]))
                       for k, v in (pivots or {}).items()}
        self.t = tmpl     # rsc5.Resource (360, big-endian)
        self.d = donor    # rsc5.Resource (PC)
        self.log = log
        self.v = Seg(tmpl.virt, VIRTUAL_PAGE)  # template virt kept whole; new structs appended
        self.p = Seg()            # physical rebuilt: donor geometry only
        self.tf = rsc5.read_fragtype(tmpl)
        self.df = rsc5.read_fragtype(donor)
        self.decls = {}
        self.uv_filled = 0
        self.texture_names = set()
        self.options = options or {}
        self.shader_sources = list(shader_sources)
        self._templates()

    # Raw template structures to clone (vtables and constant fields).
    def _templates(self):
        t = self.t
        d = self.tf['drawable']
        g0 = d['lods'][0][0]['geometries'][0]
        self.t_geom = t.virt[g0['off']:g0['off'] + 0x50]
        self.t_vb = t.virt[g0['vb_off']:g0['vb_off'] + 0x40]
        self.t_ib = t.virt[g0['ib_off']:g0['ib_off'] + 0x30]
        decl = t.vptr(g0['vb_off'] + 16)
        self.t_decl = t.virt[decl:decl + 16]
        main_model = d['lods'][0][0]['off']
        self.t_model_main = t.virt[main_model:main_model + 0x20]
        wheel = self._template_wheel()
        wm = wheel['lods'][0][0]['off']
        self.t_model_wheel = t.virt[wm:wm + 0x20]
        self.t_coll = t.virt[lod_collections(t, d['off'])[0]:lod_collections(t, d['off'])[0] + 8]
        sg = d['shader_group']['off']
        self.t_sg = t.virt[sg:sg + 0x50]
        # shaders by .sps: the template's first, then the other stock cars'
        self.t_shaders = {}
        for res in [t] + self.shader_sources:
            rd = rsc5.read_fragtype(res)['drawable'] if res is not t else d
            rsg = rd['shader_group']['off']
            usage_arr, extra_arr = res.vptr(rsg + 64), res.vptr(rsg + 72)
            for i, s in enumerate(rd['shader_group']['shaders']):
                sps = shader_sps(res, s['off'])
                self.t_shaders.setdefault(sps, dict(res=res, off=s['off'], usage=res.u32(usage_arr + 4 * i),
                                                    extra=res.u32(extra_arr + 4 * i)))
        # a template texture-parameter object to clone when the template slot is empty
        self.t_texobj = None
        for s in d['shader_group']['shaders']:
            for p in shader_params(t, s['off']):
                if p['type'] == 0 and p['value'] is not None:
                    self.t_texobj = t.virt[p['value']:p['value'] + 32]
                    break
            if self.t_texobj:
                break
        # sanity: geometry, VB and IB templates look like what was mapped
        assert struct.unpack_from('>I', self.t_vb, 28)[0] == VP | (g0['vb_off'] + 0x20)
        assert struct.unpack_from('>I', self.t_ib, 12)[0] == VP | (g0['ib_off'] + 0x10)

    def _template_wheel(self):
        for c in self.tf['children']:
            if c['drawable'] and c['drawable']['lods']:
                return c['drawable']
        raise ValueError('template has no wheel child')

    def _donor_wheel(self):
        """The donor's wheel mesh: the child on wheel_lf, else on another wheel."""
        found = None
        for c in self.df['children']:
            if not (c['drawable'] and c['drawable']['lods']):
                continue
            bone = self.d.u16(c['off'] + 0x0E)
            name = self.donor_bones[bone]['name'] if bone < len(self.donor_bones) else ''
            if name == 'wheel_lf':
                return c['drawable']
            if name.startswith('wheel_') and found is None:
                found = c['drawable']
        return found  # None: wheels modelled into the body (or none): template wheels hidden

    # ---------------- skeleton
    def _instances(self):
        """The fragment's two ready-made skeleton poses: (locals, globals)."""
        out = []
        sk = self.tf['drawable']['skeleton_off']
        for hdr in (0x1DC, 0x1E0):
            inst = self.t.vptr(hdr)
            if inst is None:
                continue
            assert self.t.vptr(inst + 4) == sk and self.t.u32(inst + 0x10) == len(self.tmpl_bones), \
                'fragment pose at +%X does not belong to the skeleton' % hdr
            out.append((self.t.vptr(inst + 0x0C), self.t.vptr(inst + 0x14)))
        return out

    def _phys_children(self):
        """{template bone index: child index} for the fragment's physics parts."""
        t = self.t
        lst = t.vptr(0xD4)
        out = {}
        for i in range(self.tf['child_count']):
            c = t.vptr(lst + 4 * i)
            if c is not None:
                out[t.u16(c + 0x0E)] = i
        return out

    def transplant_skeleton(self):
        t, d = self.t, self.d
        tsk = self.tf['drawable']['skeleton_off']
        dsk = self.df['drawable']['skeleton_off']
        tbo, tb = read_skeleton(t, tsk)
        _, db = read_skeleton(d, dsk)
        self.tmpl_bones, self.donor_bones = tb, db
        tnames = [b['name'] for b in tb]
        dnames = [b['name'] for b in db]
        tindex = {n: i for i, n in enumerate(tnames)}
        dindex = {BONE_ALIAS.get(n, n): i for i, n in enumerate(dnames)}
        self.template_bone_index = tindex
        self.donor_bone_names = dnames
        phys = self._phys_children()

        # The template's own data must chain to its stored poses, or the rules
        # above don't describe this file.
        tpos, trot = pose(tb, [b['t'] for b in tb])
        for i, b in enumerate(tb):
            assert close(tpos[i], b['pos']), 'template bone %s: chained %s, stored %s' % (b['name'], tpos[i], b['pos'])
            assert all(close(trot[i][k], b['G'][k]) for k in range(3)), 'template bone %s rotation' % b['name']
        instances = self._instances()
        for loc, glo in instances:
            for i in range(len(tb)):
                assert close(struct.unpack_from('>3f', t.virt, loc + 0x50 * i + 0x30), tb[i]['t'])
                assert close(struct.unpack_from('>3f', t.virt, glo + 0x40 * i + 0x30), tpos[i])

        # where the donor's own bones are (its file chains the same way)
        dmin = [x - POINT_MARGIN for x in self.df['drawable']['bmin'][:3]]
        dmax = [x + POINT_MARGIN for x in self.df['drawable']['bmax'][:3]]

        def sane(p):
            return all(lo <= x <= hi for x, lo, hi in zip(p, dmin, dmax))

        # targets: template bone -> new position in the car
        target, why = {}, {}
        self.stand_ins, self.parked = [], []
        for ti, b in enumerate(tb):
            n = b['name']
            if not is_point(n) or (ti in phys and not n.startswith('wheel_')):
                continue
            src = n if n in dindex else (WHEEL_STAND_IN.get(n) if WHEEL_STAND_IN.get(n) in dindex else None)
            if src is None:
                continue
            p = db[dindex[src]]['pos']
            if not sane(p):
                self.parked.append('%s (%.1f, %.1f, %.1f)' % (src, *p))
                continue
            if close(p, b['pos'], 1e-5):
                continue  # already there (a car on itself, say): left byte for byte
            if src != n:
                self.stand_ins.append('%s<-%s' % (n, src))
            target[ti] = p
        self.pivot_of = {}     # donor bone index -> template pivot bone (all its geometry)
        self.split_rules = []  # (donor bone index, condition, template pivot bone): some of it
        self.face_rules = []   # (likewise, by whole triangles: Cond.faces)
        for tname, spec in self.pivots.items():
            assert tname in tindex, 'pivot %s not in the template' % tname
            ti = tindex[tname]
            assert ti not in phys, 'pivot %s is a physics part of the template' % tname
            assert not any(b['parent'] == ti for b in tb), 'pivot %s has bones under it' % tname
            for s, cond in spec['sources']:
                assert s in dindex, 'pivot source %s not in the donor' % s
                if cond is None:
                    self.pivot_of[dindex[s]] = tname
                elif getattr(cond, 'faces', False):
                    self.face_rules.append((dindex[s], cond, tname))
                else:
                    self.split_rules.append((dindex[s], cond, tname))
            target[ti] = spec['hinge'] or db[dindex[spec['sources'][0][0]]]['pos']
        for bname, at in self.options.get('points', {}).items():
            assert bname in tindex, 'points: %s not in the template' % bname
            target[tindex[bname]] = at
        self.doors = self._doors(tb, db, tindex, dindex, phys, trot, sane, target)
        self.moved = set(target)

        # new local translations, parents first (rotations stay the template's)
        new_t = [b['t'] for b in tb]
        npos = [None] * len(tb)
        for i, b in enumerate(tb):
            p = b['parent']
            if i in target:
                npos[i] = target[i]
                new_t[i] = target[i] if p < 0 else v_mat(v_sub(target[i], npos[p]), m_t(trot[p]))
            else:
                npos[i] = new_t[i] if p < 0 else v_add(v_mat(new_t[i], trot[p]), npos[p])
        check_pos, _ = pose(tb, new_t)
        assert all(close(a, b_, 1e-4) for a, b_ in zip(check_pos, npos))

        def put3(off, v3):
            struct.pack_into('>3f', self.v.buf, off, *v3)

        larr = t.vptr(tsk + 16)
        for i, b in enumerate(tb):
            if close(new_t[i], b['t'], 0) and close(npos[i], b['pos'], 0):
                continue
            put3(b['rec'] + 0x20, new_t[i])
            put3(b['rec'] + 0x60, npos[i])
            put3(larr + 64 * i + 0x30, new_t[i])
            for loc, glo in instances:
                put3(loc + 0x50 * i + 0x30, new_t[i])
                put3(glo + 0x40 * i + 0x30, npos[i])
        # a door takes the donor's travel too (same axis, checked in _doors)
        for ti, di in self.doors.items():
            for k in (0xB0, 0xC0):
                self.v.buf[tb[ti]['rec'] + k:tb[ti]['rec'] + k + 12] = \
                    swap_words(d.virt[db[di]['rec'] + k:db[di]['rec'] + k + 12])
        self.bone_pos = npos
        self._move_composite_parts(phys, tpos, npos)
        self._move_children(phys, tpos, npos)
        self._resize_wheels(phys, dindex)

        # which template bone each donor bone's geometry is drawn with
        door_of = {di: tnames[ti] for ti, di in self.doors.items()}
        self.bone_map, self.rigid = {}, []
        for di, b in enumerate(db):
            n = BONE_ALIAS.get(b['name'], b['name'])
            if di in self.pivot_of:
                tn = self.pivot_of[di]
            elif n in tindex and carries(n) and (n in CARRY_NAMES or tindex[n] in self.moved or
                                                  close(b['pos'], npos[tindex[n]], 1e-5)):
                tn = n
            else:
                # a door and what hangs under it (windows) swing with the door;
                # parts hung under a pivot source (the pop-up pods) turn with it
                p, tn = di, BONE_FALLBACK
                while p >= 0:
                    if p in self.pivot_of:
                        tn = self.pivot_of[p]
                        break
                    if p in door_of:
                        tn = door_of[p]
                        break
                    p = db[p]['parent']
                if tn == BONE_FALLBACK and n != BONE_FALLBACK:
                    self.rigid.append(b['name'])
            self.bone_map[di] = tindex[tn]
        self.log('skeleton: %d template bones moved to the donor\'s points%s%s; donor parts drawn rigid on %s: %d'
                 % (len(self.moved), ('; stand-ins ' + ', '.join(self.stand_ins)) if self.stand_ins else '',
                    ('; parked donor bones ignored: ' + ', '.join(self.parked)) if self.parked else '',
                    BONE_FALLBACK, len(self.rigid)))

    def _donor_geometry_bones(self):
        """Donor bones some geometry of the main drawable is bound to."""
        d, out = self.d, set()
        for lod in self.df['drawable']['lods']:
            for m in lod:
                for g in m['geometries']:
                    bp, bc = d.vptr(g['off'] + 56), d.u16(g['off'] + 62)
                    if bp is not None:
                        out.update(d.u16(bp + 2 * k) for k in range(bc))
        return out

    @staticmethod
    def _under(bones, i, top):
        """Is bone i `top` or hung somewhere under it?"""
        while i >= 0:
            if i == top:
                return True
            i = bones[i]['parent']
        return False

    def _doors(self, tb, db, tindex, dindex, phys, trot, sane, target):
        """{template door bone: donor door bone} for the doors the car swings:
        a template door joint (a physics part on a limited bone) whose donor
        door is on the car (not parked), in the same frame as the template's
        (neither rotated), turning about the same single axis, with geometry
        on it or under it, and not taken by a pivot. The template's door goes
        to the donor's hinge (`target`); the caller copies the donor's travel.
        The F-16's are control surfaces in rotated frames, the Batmobile's are
        parked under the car: both stay rigid."""
        geo = self._donor_geometry_bones()
        sources = {dindex[s] for spec in self.pivots.values() for s, _ in spec['sources'] if s in dindex}
        out, notes = {}, []
        for n in DOOR_NAMES:
            if n not in tindex or n not in dindex:
                continue
            ti, di = tindex[n], dindex[n]
            tl, dl = bone_limits(self.t, tb[ti]['rec']), bone_limits(self.d, db[di]['rec'])
            if ti not in phys or not is_joint(tl) or n in self.pivots:
                continue
            if not sane(db[di]['pos']):
                why = 'parked'
            elif not (is_identity(db[di]['G']) and is_identity(trot[ti])):
                why = 'rotated'
            elif len(joint_axes(tl)) != 1 or joint_axes(dl) != joint_axes(tl):
                why = 'turns about another axis'
            elif any(self._under(db, s, di) for s in sources):
                why = 'a pivot source'
            elif not any(self._under(db, g, di) for g in geo):
                why = 'no geometry'
            else:
                why = None
            if why:
                notes.append('%s %s' % (n, why))
                continue
            target[ti] = db[di]['pos']
            out[ti] = di
            notes.append('%s hinge %.2f m from the template\'s' % (
                n, sum((a - b) ** 2 for a, b in zip(db[di]['pos'], tb[ti]['pos'])) ** 0.5))
        if notes:
            self.log('doors: ' + '; '.join(notes))
        return out

    def _move_composite_parts(self, phys, old_pos, new_pos):
        """Collision parts (wheels, doors) follow their bones; the composite's box grows to hold them."""
        t, v = self.t, self.v
        bnd = t.vptr(0xEC)
        if bnd is None:
            return
        assert t.u8(bnd + 4) == 0x0C, 'fragment bound is not a composite'
        n = t.u16(bnd + 0x90)
        mats = {t.vptr(bnd + 0x84), t.vptr(bnd + 0x88)}
        boxes = t.vptr(bnd + 0x8C)
        mx = list(struct.unpack_from('>3f', t.virt, bnd + 0x10))
        mn = list(struct.unpack_from('>3f', t.virt, bnd + 0x20))
        grew = False
        for bone, child in phys.items():
            if close(old_pos[bone], new_pos[bone], 0) or child >= n:
                continue
            for m in mats:
                cur = struct.unpack_from('>3f', t.virt, m + 0x40 * child + 0x30)
                assert close(cur, old_pos[bone]), 'collision part %d is not at its bone' % child
                struct.pack_into('>3f', v.buf, m + 0x40 * child + 0x30, *new_pos[bone])
            rot = [struct.unpack_from('>3f', t.virt, next(iter(mats)) + 0x40 * child + 16 * k) for k in range(3)]
            lo = struct.unpack_from('>3f', t.virt, boxes + 0x20 * child)
            hi = struct.unpack_from('>3f', t.virt, boxes + 0x20 * child + 0x10)
            for cx in (lo[0], hi[0]):
                for cy in (lo[1], hi[1]):
                    for cz in (lo[2], hi[2]):
                        w = v_add(v_mat((cx, cy, cz), rot), new_pos[bone])
                        for k in range(3):
                            if w[k] > mx[k]:
                                mx[k], grew = w[k], True
                            if w[k] < mn[k]:
                                mn[k], grew = w[k], True
        if grew:
            r08, r0c = t.f32(bnd + 0x08), t.f32(bnd + 0x0C)
            half = [(a - b) / 2 for a, b in zip(mx, mn)]
            radius = sum(h * h for h in half) ** 0.5
            struct.pack_into('>3f', v.buf, bnd + 0x10, *mx)
            struct.pack_into('>3f', v.buf, bnd + 0x20, *mn)
            struct.pack_into('>3f', v.buf, bnd + 0x30, *[(a + b) / 2 for a, b in zip(mx, mn)])
            struct.pack_into('>2f', v.buf, bnd + 0x08, max(r08, radius), max(r0c, radius + (r0c - r08)))

    def _move_children(self, phys, old_pos, new_pos):
        """A moved physics part's fragment child data follow its bone: the
        inverse of the bone's rest matrix (child +0x10), its link's centre of
        gravity (child +0x50: the body's for a part fixed to the body, a
        door's own, which moves with its hinge) and its offset from that
        centre (fragType +0xF8). Stale, they put a part back where the
        template had it once a door swings and the car is articulated (v1.7.7
        and before left the wheels' that way). Every relation is checked on
        the template first."""
        t, v = self.t, self.v
        tb = self.tmpl_bones
        lst, att = t.vptr(0xD4), t.vptr(0xF8)
        bnd = t.vptr(0xEC)
        cur = t.vptr(bnd + 0x84)
        root_cg = rows(t, t.vptr(lst) + 0x50)[3]

        def link_of(i):  # the joint bone whose link carries bone i (-1: the body)
            while i >= 0 and not is_joint(bone_limits(t, tb[i]['rec'])):
                i = tb[i]['parent']
            return i if i > 0 else -1

        moved = []
        for bone, c in sorted(phys.items(), key=lambda kv: kv[1]):
            co = t.vptr(lst + 4 * c)
            link = link_of(bone)
            d_link = v_sub(new_pos[link], old_pos[link]) if link >= 0 else (0.0, 0.0, 0.0)
            if close(old_pos[bone], new_pos[bone], 0) and close(d_link, (0, 0, 0), 0):
                continue
            inv, cg, off = rows(t, co + 0x10), rows(t, co + 0x50), rows(t, att + 64 * c)
            part = rows(t, cur + 64 * c)
            name = tb[bone]['name']
            # the template's own data keep the relations this rewrites
            assert close(inv[3], tuple(-x for x in v_mat(old_pos[bone], inv[:3]))), \
                '%s: child +0x10 is not the inverse of its rest matrix' % name
            assert close(cg[3], root_cg) == (link < 0), '%s: link centre %s' % (name, cg[3])
            assert close(off[3], v_mat(v_sub(part[3], cg[3]), m_t(cg[:3]))), \
                '%s: part offset %s is not part - centre' % (name, off[3])
            new_cg = v_add(cg[3], d_link)
            struct.pack_into('>3f', v.buf, co + 0x10 + 0x30, *(-x for x in v_mat(new_pos[bone], inv[:3])))
            struct.pack_into('>3f', v.buf, co + 0x50 + 0x30, *new_cg)
            struct.pack_into('>3f', v.buf, att + 64 * c + 0x30, *v_mat(v_sub(new_pos[bone], new_cg), m_t(cg[:3])))
            moved.append(name)
        self.children_moved = moved

    @staticmethod
    def _wheel_bounds(res, frag, names):
        """{wheel bone name: (bound, child, tyre radius)}. A wheel's collision
        part is a curved geometry: a disc (its curved edges' radius) swept by
        the bound's margin; Rockstar's wheels keep the two adding up to their
        box's half height (FBI: 0.2592 + 0.1412 = 0.4004). Mods' exporters
        don't always (the Batmobile's add up to 0.51 in a 0.35 box), so the
        radius is read from the box, which they fit to the wheel itself."""
        bnd = res.vptr(0xEC)
        parts, lst = res.vptr(bnd + 0x80), res.vptr(0xD4)
        out = {}
        for i in range(frag['child_count']):
            c = res.vptr(lst + 4 * i)
            if c is None:
                continue
            name = names[res.u16(c + 0x0E)]
            p = res.vptr(parts + 4 * i)
            if not name.startswith('wheel_') or p is None or res.u8(p + 4) != 5:
                continue
            edges, n = res.vptr(p + 0xE4), res.u32(p + 0xEC)
            if edges is None or not n:
                continue
            out[name] = (p, i, res.f32(p + 0x14))
        return out

    def _resize_wheels(self, phys, dindex):
        """The template's physics wheels take the donor's radii. A stock
        wheel larger than the mod's held the car up by the difference (all the
        add-ons floated: the Dominator 6 cm on the Banshee's 0.38 m wheels)."""
        t, v = self.t, self.v
        tw = self._wheel_bounds(t, self.tf, [b['name'] for b in self.tmpl_bones])
        dw = self._wheel_bounds(self.d, self.df, [BONE_ALIAS.get(b['name'], b['name']) for b in self.donor_bones])
        bnd = t.vptr(0xEC)
        boxes = t.vptr(bnd + 0x8C)
        done = []
        radii = self.options.get('radii', {})
        for name, (p, child, radius) in sorted(tw.items()):
            src = name if name in dw else WHEEL_STAND_IN.get(name)
            if src not in dw and name not in radii:
                continue
            want = radii.get(name, dw[src][2] if src in dw else radius)
            margin = t.f32(p + 0x70)
            edge_r = max(want - margin, 0.02)
            edges, n = t.vptr(p + 0xE4), t.u32(p + 0xEC)
            for e in range(n):
                struct.pack_into('>f', v.buf, edges + 0x30 * e + 0x20, edge_r)
            r = edge_r + margin
            hx = t.f32(p + 0x10)
            struct.pack_into('>2f', v.buf, p + 0x14, r, r)
            struct.pack_into('>2f', v.buf, p + 0x24, -r, -r)
            struct.pack_into('>f', v.buf, p + 0x08, (hx * hx + 2 * r * r) ** 0.5)
            struct.pack_into('>2f', v.buf, boxes + 0x20 * child + 0x04, -r, -r)
            struct.pack_into('>2f', v.buf, boxes + 0x20 * child + 0x14, r, r)
            done.append('%s %.3f->%.3f' % (name[6:], radius, r))
        self.wheel_report = done
        if done:
            self.log('wheels: physics radii from the donor: ' + ', '.join(done))

    # ---------------- shaders
    def build_shader_group(self):
        d = self.d
        dsg = self.df['drawable']['shader_group']
        n = len(dsg['shaders'])
        new_shaders, usages, extras, kinds = [], [], [], []
        self.shader_report = []
        for s in dsg['shaders']:
            sps = shader_sps(d, s['off'])
            tsps = sps if sps in self.t_shaders else SPS_FALLBACK.get(sps)
            assert tsps in self.t_shaders, 'no template shader for %s' % sps
            ts = self.t_shaders[tsps]
            new_shaders.append(self._clone_shader(ts, s['off']))
            usages.append(ts['usage'])
            extras.append(ts['extra'])
            kinds.append(tsps)
            self.shader_report.append((sps, tsps if ts['res'] is self.t else '%s (from %s)'
                                       % (tsps, ts['res'].name.split('/')[-1])))
        # The shaders the game drives per car to the front when one would sit
        # past the stock slots; geometries' shader maps follow (emit_lod).
        # shader_usage stays in the donor's order, as the donor maps read it.
        def rank(i):
            return next((k for k, prefix in enumerate(EFFECT_SHADERS) if kinds[i].startswith(prefix)),
                        len(EFFECT_SHADERS))
        order = list(range(n))
        if any(rank(i) < len(EFFECT_SHADERS) and i >= STOCK_SHADER_SLOTS for i in order):
            order.sort(key=lambda i: (rank(i), i))
            self.log('shaders: per-car shaders moved to the front (%s)' % ', '.join(
                '%s %d->%d' % (kinds[i].replace('gta_vehicle_', '').replace('.sps', ''), i, k)
                for k, i in enumerate(order) if rank(i) < len(EFFECT_SHADERS)))
        self.shader_slot = {old: new for new, old in enumerate(order)}
        self.shader_usage = list(usages)
        new_shaders = [new_shaders[i] for i in order]
        usages = [usages[i] for i in order]
        extras = [extras[i] for i in order]
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

    def _clone_shader(self, ts_info, ds):
        """Clone a 360 shader (the template's, or another stock car's); fill
        parameters from donor shader ds by name hash. Parameter block laid out
        exactly as the source's: offsets | types (16-aligned) | names
        (16-aligned) | values (16-aligned)."""
        src, ts = ts_info['res'], ts_info['off']
        foreign = src is not self.t
        d, v = self.d, self.v
        tparams = shader_params(src, ts)
        dparams = {p['hash']: p for p in shader_params(d, ds)}
        # the donor's colour map: a bump, specular or dirt slot naming it is a
        # placeholder (the Batwing's), filled from the options or the template
        colour = None
        if SAMPLER_TEXTURE in dparams and dparams[SAMPLER_TEXTURE]['type'] == 0 and \
                dparams[SAMPLER_TEXTURE]['value'] is not None:
            np_ = d.vptr(dparams[SAMPLER_TEXTURE]['value'] + 20)
            colour = d.cstr(np_).lower() if np_ is not None else None
        so = v.put(src.virt[ts:ts + 0x60])
        if foreign:
            # its strings (effect and .sps names) live in the other file; the
            # rest of the header is values (+0x28 is the effect's name hash)
            for k in (0x44, 0x48):
                p = src.vptr(ts + k)
                v.w32(so + k, (VP | v.cstr(src.cstr(p))) if p is not None else 0)
        # The parameter block (offsets | types | names | values, size at +0x20)
        # is replicated byte for byte with the same 64-byte phase: the 360
        # compiler's packing has gaps no simple rule explains (tire: zeroes
        # before its matrix), so every value keeps the source's position.
        tpo, tpt, tpn = src.vptr(ts + 0x14), src.vptr(ts + 0x24), src.vptr(ts + 0x34)
        size = src.u32(ts + 0x20)
        # Kept inside one page when it fits (each array and value in it is
        # reached through its own pointer), with the source's 64-byte phase.
        po = v.alloc(size, 64, tpo % 64)
        v.buf[po:po + size] = src.virt[tpo:tpo + size]
        assert po % 64 == tpo % 64
        pt, pn = po + (tpt - tpo), po + (tpn - tpo)
        for i, p in enumerate(tparams):
            if p['type'] == 0:
                continue
            assert p['value'] is not None and tpo <= p['value'] < tpo + size
            o = po + (p['value'] - tpo)
            vsize = 16 * p['type']  # the type byte is the vec4 count (1, 4 = matrix, 15 = array)
            dp = dparams.get(p['hash'])
            if dp and dp['type'] == p['type'] and dp['value'] is not None:
                v.buf[o:o + vsize] = swap_words(d.virt[dp['value']:dp['value'] + vsize])
            v.w32(po + 4 * i, VP | o)
        # texture parameters: objects outside the block, as in the source
        for i, p in enumerate(tparams):
            if p['type'] != 0:
                continue
            dp = dparams.get(p['hash'])
            name = None
            if dp is not None and dp['type'] == 0:
                if dp['value'] is not None:
                    np_ = d.vptr(dp['value'] + 20)
                    name = d.cstr(np_) if np_ is not None else None
                use = name is not None
                if use and colour and p['hash'] in SAMPLER_PLACEHOLDER and name.lower() == colour:
                    opts = self.options or {}
                    # (only on the colour map they were made from, when named)
                    wanted = opts.get(SAMPLER_PLACEHOLDER[p['hash']]) \
                        if opts.get('colour', colour).lower() == colour else None
                    self.placeholders = getattr(self, 'placeholders', 0) + 1
                    if wanted:
                        name = wanted
                    else:
                        name = None
                        use = p['value'] is not None
                        if use:
                            np_ = src.vptr(p['value'] + 20)
                            name = src.cstr(np_) if np_ is not None else None
                # A PC reflection map ("vehicleenvmap128", in no dictionary
                # the console has; the BTTF IV DeLoreans name it in their dirt
                # slot) leaves the template's own texture there, or none.
                if use and 'envmap' in name.lower():
                    self.env_kept = getattr(self, 'env_kept', 0) + 1
                    name = None
                    use = p['value'] is not None
                    if use:
                        np_ = src.vptr(p['value'] + 20)
                        name = src.cstr(np_) if np_ is not None else None
            else:
                use = p['value'] is not None  # donor lacks it: keep the source's
                if use and foreign:
                    np_ = src.vptr(p['value'] + 20)
                    name = src.cstr(np_) if np_ is not None else None
            if not use:
                v.w32(po + 4 * i, 0)
                continue
            srcobj = src.virt[p['value']:p['value'] + 32] if p['value'] is not None else self.t_texobj
            obj = v.put(srcobj, 16)
            if name is not None:
                v.w32(obj + 20, VP | v.cstr(name))
                # (a reference is looked up by its whole name: a mod's
                # "x.dds" means the dictionary entry "x.dds.dds", hashed as
                # "x.dds", as the BTTF IV DeLoreans' time machine parts are)
                bare = name.split('/')[-1].lower()
                # a name kept from another car resolves in the shared pack or gets a stand-in
                if dp is not None or not bare.startswith('vehicle'):
                    self.texture_names.add(bare)
            v.w32(po + 4 * i, VP | obj)
        v.w32(so + 0x14, VP | po)
        v.w32(so + 0x24, VP | pt)
        v.w32(so + 0x34, VP | pn)
        return so

    # ---------------- geometry
    def _decl(self, usage, src_decl_off):
        if usage in self.decls:
            return self.decls[usage]
        d = self.d
        _, stride = vertex_layout(usage, SIZE_360)
        assert struct.unpack_from('<Q', d.virt, src_decl_off + 8)[0] == PC_TYPES
        b = bytearray(self.t_decl)
        struct.pack_into('>I', b, 0, usage)
        b[4] = stride
        b[5] = 0
        b[6] = d.virt[src_decl_off + 6]
        b[7] = d.virt[src_decl_off + 7]
        struct.pack_into('>Q', b, 8, TYPES_360)
        off = self.v.put(bytes(b))
        self.decls[usage] = off
        return off

    def convert_geometry(self, g, need=0):
        """Donor geometry -> phys data (shared across LOD duplicates). `need`:
        the vertex inputs of the 360 shader it is drawn with; a texcoord set
        the donor lacks is filled from texcoord 0."""
        d = self.d
        assert g['prim'] == 3, 'only triangle lists'
        src_usage = g['decl']['usage']
        fill_uv = need & ~src_usage & UV_BITS if src_usage & 0x40 else 0
        usage = src_usage | fill_uv
        # Some exporters leave the geometry's own counts zero; the vertex and
        # index buffers always carry them.
        vcount = g['vertex_count'] or g['vb_count']
        vsrc = d.phys[g['vb_data'][1]:g['vb_data'][1] + vcount * g['vb_stride']]
        bp = d.vptr(g['off'] + 56)
        bc = d.u16(g['off'] + 62)
        table = [d.u16(bp + 2 * k) for k in range(bc)] if bp is not None else []
        tb = [self.bone_map[b] for b in table]
        n = g['ib_count']
        isrc = d.phys[g['ib_data'][1]:g['ib_data'][1] + 2 * n]
        idx = list(struct.unpack('<%dH' % n, isrc))
        assert max(idx) < vcount
        vsrc, idx, vcount = self._cut_faces(vsrc, src_usage, vcount, g['vb_stride'], table, tb, idx)
        vsrc = self._split_vertices(vsrc, src_usage, vcount, g['vb_stride'], table, tb)
        vsrc = self._shift_vertices(vsrc, src_usage, vcount, g['vb_stride'], table)
        vdata, stride = convert_vertices(vsrc, src_usage, vcount, g['vb_stride'], fill_uv)
        if fill_uv:
            self.uv_filled += 1
        idata = struct.pack('>%dH' % n, *idx)
        assert max(idx) < vcount
        decl_src = d.vptr(g['vb_off'] + 16)
        # vo/io: set by place_buffers once every geometry is converted
        return dict(usage=usage, stride=stride, vcount=vcount, icount=n,
                    vdata=vdata, vbytes=len(vdata), idata=idata, ibytes=len(idata), bones=tb,
                    decl_src=decl_src)

    def _cut_faces(self, vsrc, usage, vcount, stride, table, tb, idx):
        """Whole triangles to their pivots (face_cut, Cond.faces): the copies
        appended to the vertex data, every moved vertex's influences pointed
        at the pivot's bone-table slot (appended to `tb` when new)."""
        rules = [(di, cond, tname) for di, cond, tname in self.face_rules if di in table]
        if not rules or not usage & 4:
            return vsrc, idx, vcount
        at = dict(vertex_layout(usage, SIZE_PC)[0])
        pos = [struct.unpack_from('<3f', vsrc, v * stride + at[0]) for v in range(vcount)]
        bone_of = [table[main_influence(vsrc, at, v, stride)] if main_influence(vsrc, at, v, stride) < len(table)
                   else -1 for v in range(vcount)]
        idx, dup, assign = face_cut(pos, bone_of, idx, rules)
        out = bytearray(vsrc)
        for v in dup:
            out += vsrc[v * stride:(v + 1) * stride]
        for v, tname in assign.items():
            ti = self.template_bone_index[tname]
            if ti not in tb:
                tb.append(ti)
            o = v * stride
            for slot in range(4):
                if 1 in at and out[o + at[1] + slot] == 0:
                    continue  # (an unused influence)
                out[o + at[2] + slot] = tb.index(ti)
        self.faces_moved = getattr(self, 'faces_moved', 0) + sum(1 for t in range(0, len(idx), 3)
                                                                 if idx[t] in assign)
        self.faces_copied = getattr(self, 'faces_copied', 0) + len(dup)
        return bytes(out), idx, vcount + len(dup)

    def _shift_vertices(self, vsrc, usage, vcount, stride, table):
        """The shift option: vertices whose main influence is a named donor
        bone moved by its offset (PC data)."""
        shifts = self.options.get('shift', {})
        if not shifts:
            return vsrc
        want = {}
        for k, b in enumerate(table):
            name = BONE_ALIAS.get(self.donor_bones[b]['name'], self.donor_bones[b]['name'])
            if name in shifts:
                want[k] = shifts[name]
        if not want:
            return vsrc
        at = dict(vertex_layout(usage, SIZE_PC)[0])
        out = bytearray(vsrc)
        moved = 0
        for v in range(vcount):
            d = want.get(main_influence(vsrc, at, v, stride))
            if d is None:
                continue
            o = v * stride + at[0]
            x, y, z = struct.unpack_from('<3f', out, o)
            struct.pack_into('<3f', out, o, x + d[0], y + d[1], z + d[2])
            moved += 1
        self.shifted = getattr(self, 'shifted', 0) + moved
        return bytes(out)

    def _split_vertices(self, vsrc, usage, vcount, stride, table, tb):
        """Re-bind the vertices a conditional pivot source claims (see
        parse_pivots): their bone index points at a bone-table slot for the
        pivot, appended to `tb` (the geometry's template bone table) when it
        is not there yet. Returns the (possibly patched) PC vertex data."""
        rules = {}
        for k, b in enumerate(table):
            for di, cond, tname in self.split_rules:
                if b == di:
                    rules.setdefault(k, []).append((cond, self.template_bone_index[tname]))
        if not rules or not usage & 4:
            return vsrc
        at = dict(vertex_layout(usage, SIZE_PC)[0])
        out = bytearray(vsrc)
        moved = 0
        for v in range(vcount):
            o = v * stride
            pos = struct.unpack_from('<3f', out, o + at[0])
            for slot in range(4):
                if 1 in at and out[o + at[1] + slot] == 0:
                    continue  # (an unused influence)
                k = out[o + at[2] + slot]
                for cond, ti in rules.get(k, ()):
                    if meets(pos, cond):
                        if ti not in tb:
                            tb.append(ti)
                        out[o + at[2] + slot] = tb.index(ti)
                        moved += 1
                        break
        self.split_moved = getattr(self, 'split_moved', 0) + moved
        return bytes(out)

    def place_buffers(self, *lod_sets):
        """Lay every converted geometry's vertex and index data out in the
        physical segment, each buffer inside one load chunk."""
        geoms, seen = [], set()
        for lods in lod_sets:
            for lod in lods:
                for gs, _ in (lod or []):
                    for c in gs:
                        if id(c) not in seen:
                            seen.add(id(c))
                            geoms.append(c)
        objs = []
        for c in geoms:
            objs += [(c['vdata'], 0x100), (c['idata'], 0x100)]
        phys, offs, count, shift = place_physical(objs)
        for k, c in enumerate(geoms):
            c['vo'], c['io'] = offs[2 * k], offs[2 * k + 1]
        self.p = Seg(phys)
        self.p_count, self.p_shift = count, shift
        chunks = load_chunks(count, 256 << shift)
        self.log('physical: %d buffers in %d chunks of up to %d KiB (%d x %d KiB pages)'
                 % (len(objs), len(chunks), max(e - s for s, e in chunks) >> 10, count, (256 << shift) >> 10))

    def emit_geometry(self, c):
        v = self.v
        decl = self._decl(c['usage'], c['decl_src'])
        # vertex buffer + its D3D header
        vb = v.put(self.t_vb)
        v.w16(vb + 4, c['vcount'])
        v.w32(vb + 8, PP | c['vo'])
        v.w32(vb + 12, c['stride'])
        v.w32(vb + 16, VP | decl)
        v.w32(vb + 24, PP | c['vo'])
        v.w32(vb + 28, VP | (vb + 0x20))
        f1 = struct.unpack_from('>I', self.t_vb, 0x3C)[0]
        assert c['vbytes'] % 4 == 0 and c['vbytes'] // 4 < (1 << 24)
        v.w32(vb + 0x38, (PP | c['vo']) | 3)
        v.w32(vb + 0x3C, (f1 & 0xFC000003) | ((c['vbytes'] // 4) << 2))
        # index buffer + its D3D header
        ib = v.put(self.t_ib)
        v.w32(ib + 4, c['icount'])
        v.w32(ib + 8, PP | c['io'])
        v.w32(ib + 12, VP | (ib + 0x10))
        v.w32(ib + 0x28, PP | c['io'])
        v.w32(ib + 0x2C, c['ibytes'])
        # bone table
        bt = 0
        if c['bones']:
            bt = v.alloc(2 * len(c['bones']), 16)
            for k, b in enumerate(c['bones']):
                v.w16(bt + 2 * k, b)
        geo = v.put(self.t_geom)
        v.w32(geo + 12, VP | vb)
        v.w32(geo + 28, VP | ib)
        v.w32(geo + 44, c['icount'])
        v.w32(geo + 48, c['icount'] // 3)
        v.w16(geo + 52, c['vcount'])
        v.w16(geo + 54, 3)
        v.w32(geo + 56, (VP | bt) if bt else 0)
        v.w16(geo + 60, c['stride'])
        v.w16(geo + 62, len(c['bones']))
        return geo

    def emit_lod(self, converted_models, model_template):
        """converted_models: list of (list of converted geometry, shader map)."""
        v = self.v
        models = []
        for geoms, smap in converted_models:
            gofs = [self.emit_geometry(c) for c in geoms]
            garr = v.alloc(4 * len(gofs))
            for k, g in enumerate(gofs):
                v.w32(garr + 4 * k, VP | g)
            sm = v.alloc(2 * len(smap), 16)
            for k, s in enumerate(smap):
                v.w16(sm + 2 * k, self.shader_slot.get(s, s))
            m = v.put(model_template)
            v.w32(m + 4, VP | garr)
            v.w16(m + 8, len(gofs))
            v.w16(m + 10, len(gofs))
            v.w32(m + 12, 0)
            v.w32(m + 16, VP | sm)
            v.w16(m + 26, len(gofs))
            models.append(m)
        marr = v.alloc(4 * len(models))
        for k, m in enumerate(models):
            v.w32(marr + 4 * k, VP | m)
        coll = v.put(self.t_coll)
        v.w32(coll, VP | marr)
        v.w16(coll + 4, len(models))
        v.w16(coll + 6, len(models))
        return coll

    def convert_drawable_lods(self, ddraw):
        out = []
        for coll in lod_collections(self.d, ddraw['off']):
            if coll is None:
                out.append(None)
                continue
            lod = []
            for m in model_collection(self.d, coll):
                gs = [g for g in rsc5.ptr_collection(self.d, m + 4) if g is not None]
                smap_p = self.d.vptr(m + 16)
                smap = [self.d.u16(smap_p + 2 * k) for k in range(len(gs))]
                geoms, shaders = [], []
                for g, si in zip(gs, smap):
                    whole = self.convert_geometry(rsc5.read_geometry(self.d, g), self.shader_usage[si])
                    parts = split_geometry(whole)
                    if len(parts) > 1:
                        self.log('geometry of %d vertices (%d KiB) drawn as %d parts of up to %d vertices'
                                 % (whole['vcount'], whole['vbytes'] >> 10, len(parts),
                                    max(p['vcount'] for p in parts)))
                    geoms += parts
                    shaders += [si] * len(parts)
                lod.append((geoms, shaders))
            out.append(lod)
        return out

    def check_shader_inputs(self, lods):
        for lod in lods:
            for geoms, smap in (lod or []):
                for c, si in zip(geoms, smap):
                    need = self.shader_usage[si]
                    assert need & ~c['usage'] == 0, \
                        'shader %d needs %04X, geometry has %04X' % (si, need, c['usage'])

    def copy_bounds(self, t_draw_off, d_draw_off):
        for k in (16, 32, 48):
            self.v.buf[t_draw_off + k:t_draw_off + k + 16] = swap_words(self.d.virt[d_draw_off + k:d_draw_off + k + 16])
        self.v.buf[t_draw_off + 112:t_draw_off + 116] = swap_words(self.d.virt[d_draw_off + 112:d_draw_off + 116])

    def run(self):
        t = self.t
        self.transplant_skeleton()
        sg = self.build_shader_group()
        tdraw = self.tf['drawable']['off']
        ddraw = self.df['drawable']
        self.v.w32(tdraw + 8, VP | sg)

        main = self.convert_drawable_lods(ddraw)
        lod_map = (self.options or {}).get('lods')
        if lod_map:
            # (a donor LOD no slot draws is left out of the file)
            main = [l if i in lod_map else None for i, l in enumerate(main)]
        wheel_d = self._donor_wheel()
        wheel = self.convert_drawable_lods(wheel_d) if wheel_d else []
        self.check_shader_inputs(main)
        self.check_shader_inputs(wheel)
        self.place_buffers(main, wheel)

        # main: template slots 0-2 used; donor has fewer -> repeat its last LOD
        tslots = [c is not None for c in lod_collections(t, tdraw)]
        dlods = [l for l in main if l is not None]
        for slot, used in enumerate(tslots):
            if not used:
                continue
            src = dlods[min(slot, len(dlods) - 1)]
            if lod_map and lod_map[min(slot, len(lod_map) - 1)] < len(main) and main[lod_map[min(slot, len(lod_map) - 1)]]:
                src = main[lod_map[min(slot, len(lod_map) - 1)]]
            self.v.w32(tdraw + 64 + 4 * slot, VP | self.emit_lod(src, self.t_model_main))
        self.copy_bounds(tdraw, ddraw['off'])

        twheel = self._template_wheel()
        tw_slots = [c is not None for c in lod_collections(t, twheel['off'])]
        wlods = [l for l in wheel if l is not None]
        if not wlods:
            # No wheel mesh of its own: the template's car wheels are not drawn
            # (a drawable without LODs, like the template's other children).
            for slot in range(4):
                self.v.w32(twheel['off'] + 64 + 4 * slot, 0)
            self.log('wheels: donor has no wheel mesh; template wheel meshes hidden')
        for slot, used in enumerate(tw_slots):
            if not used or not wlods:
                continue
            src = wlods[min(slot, len(wlods) - 1)]
            self.v.w32(twheel['off'] + 64 + 4 * slot, VP | self.emit_lod(src, self.t_model_wheel))
        if wheel_d:
            self.copy_bounds(twheel['off'], wheel_d['off'])

        # fragment bounding sphere (+16..+31), same layout on both (checked by caller)
        self.v.buf[16:32] = swap_words(self.d.virt[16:32])

        flags, vtot, ptot = make_flags(t.flags, len(self.v.buf), self.p_count, self.p_shift)
        assert ptot == len(self.p.buf)
        virt = bytes(self.v.buf) + b'\0' * (vtot - len(self.v.buf))
        return flags, virt + bytes(self.p.buf)


# ---------------------------------------------------------------- textures
A8R8G8B8 = '\x15\x00\x00\x00'  # D3DFMT_A8R8G8B8 as the PC info stores it
# uncompressed D3D formats the PC info can carry: (kind, bytes per texel)
UNCOMPRESSED = {A8R8G8B8: ('A8R8G8B8', 4), '\x1c\x00\x00\x00': ('A8', 1), '\x32\x00\x00\x00': ('L8', 1)}


def linear_texture(src):
    """A PC texture's level 0 as the 360 path takes it: (Xenos format, block
    bytes, linear blocks, label). The odd uncompressed texture is compressed
    to DXT5 so it takes the same verified path as every other one; one-channel
    formats are spread to B, G, R, A the way D3D samples them (A8: black with
    that alpha; L8: grey, opaque)."""
    w, h = src['w'], src['h']
    if src['fmt'] not in UNCOMPRESSED:
        fmt, bpp = xenos.FORMATS[src['fmt']]
        bw, bh = max(1, w // 4), max(1, h // 4)
        return fmt, bpp, src['res'].phys[src['data']:src['data'] + bw * bh * bpp], None
    kind, bpt = UNCOMPRESSED[src['fmt']]
    raw = src['res'].phys[src['data']:src['data'] + w * h * bpt]
    if kind == 'A8':
        raw = b''.join(bytes((0, 0, 0, a)) for a in raw)
    elif kind == 'L8':
        raw = b''.join(bytes((l, l, l, 255)) for l in raw)
    if w < 4 or h < 4:  # one block: clamp the texels into 4x4
        px = [raw[4 * (min(y, h - 1) * w + min(x, w - 1)):][:4] for y in range(4) for x in range(4)]
        raw, w, h = b''.join(px), 4, 4
    fmt, bpp = xenos.FORMATS['DXT5']
    return fmt, bpp, dxt.encode_dxt5(raw, w, h), 'DXT5 (from %s)' % kind


# Biggest texture side kept. A 2048x2048 DXT5 texture is 4 MiB, one load
# chunk the streaming heap must find whole (see MIN_PHYSICAL_PAGE); a bigger
# texture starts at its first mip level that fits instead (the same picture at
# a quarter of the memory per level dropped). The PC data holds the levels
# one after another, each tightly packed.
MAX_TEXTURE_SIDE = 1024


# A detail map (a normal, specular or bump map: a name ending _n, _s, _nrm,
# _normal, _spec, spec or bump) may be held to a smaller side than the
# colour maps (a heavy model's dictionary, the BTTF IV DeLoreans').
DETAIL_MAP = re.compile(r'(_n|_s|_nrm|_normal|_spec|spec|bump)(\.dds)?$')


def parse_options(text):
    """'lods=0:0:2,tex=512,detail=256' -> {'lods': [0, 0, 2], 'tex': 512, 'detail': 256}.
    lods: the donor LOD drawn in each of the template's LOD slots (a
    heavy model can leave out a middle LOD); tex / detail: the biggest side
    kept for colour maps / detail maps; textures: a directory of maps taking
    the place of the mod's own (remaster.py); bump / spec: the maps a shader
    gets where the mod gives its bump or specular slot its colour map (the
    Batwing's: its colour map in every slot); colour: the colour map those
    two go with (other shaders' placeholders take the template's).
    points=wheel_lr=-1.37:-1.49:-1.35;... puts template bones (and the physics
    parts on them) at those points instead of the donor's; radii=wheel_lr:0.24;...
    gives physics wheels those radii; shift=wheel_lf:0:0:0.15 moves the donor
    geometry bound to a bone (the Batwing's: its physics wheels under the
    wheels it draws, the nose wheel up so it stands level)."""
    out = {}
    for item in filter(None, (text or '').split(',')):
        key, value = item.split('=', 1)
        if key == 'lods':
            out[key] = [int(v) for v in value.split(':')]
        elif key in ('textures', 'bump', 'spec', 'colour'):
            out[key] = value
        elif key == 'points':  # bone=x:y:z;... : template bones put at these points
            out[key] = {b: tuple(float(v) for v in at.split(':'))
                        for b, at in (item.split('=') for item in value.split(';') if item)}
        elif key == 'radii':  # wheel_xx:r;... : the physics wheels' radii
            out[key] = {b: float(r) for b, r in (item.split(':') for item in value.split(';') if item)}
        elif key == 'shift':  # donor_bone:dx:dy:dz;... : the geometry bound to it moved
            out[key] = {}
            for item in filter(None, value.split(';')):
                b, *d = item.split(':')
                out[key][b] = tuple(float(v) for v in d)
        else:
            out[key] = int(value)
    return out


class _Data:
    """Texel data standing in for a resource's physical segment."""
    def __init__(self, phys):
        self.phys = phys


def texture_overrides(directory):
    """<directory>/<name>.png -> pc_textures-style entries (DXT5, one level)
    that take the place of the mod's own textures of those names (the
    textures= option: maps remastered by remaster.py)."""
    from PIL import Image
    import numpy as np
    out = {}
    for f in sorted(os.listdir(directory)):
        if not f.lower().endswith('.png'):
            continue
        name = f[:-4].lower()
        img = Image.open(os.path.join(directory, f)).convert('RGBA')
        w, h = img.size
        bgra = np.asarray(img)[..., [2, 1, 0, 3]].tobytes()
        out[name] = dict(name=name, w=w, h=h, fmt='DXT5', levels=1, res=_Data(dxt.encode_dxt5(bgra, w, h)), data=0)
    return out


def texture_side(name, options):
    side = (options or {}).get('tex', MAX_TEXTURE_SIDE)
    if DETAIL_MAP.search(name):
        side = min(side, (options or {}).get('detail', side))
    return side


def fit_texture(src, side=MAX_TEXTURE_SIDE):
    """`src` (a pc_textures entry), or its first level no wider or taller
    than `side`, as an entry of its own (label 'from' says what it was)."""
    w, h, levels, data = src['w'], src['h'], src['levels'], src['data']
    if src['fmt'] in UNCOMPRESSED:
        size = lambda w, h: w * h * UNCOMPRESSED[src['fmt']][1]  # noqa: E731
    elif src['fmt'] in xenos.FORMATS:
        size = lambda w, h: max(1, w // 4) * max(1, h // 4) * xenos.FORMATS[src['fmt']][1]  # noqa: E731
    else:
        return src
    dropped = 0
    while max(w, h) > side and levels > 1 and min(w, h) >= 8:
        data += size(w, h)
        w, h, levels, dropped = w // 2, h // 2, levels - 1, dropped + 1
    if not dropped:
        return src
    return dict(src, w=w, h=h, levels=levels, data=data, dropped=dropped, source_size=(src['w'], src['h']))


def pc_textures(r, dict_off):
    out = {}
    tl = r.vptr(dict_off + 24)
    for i in range(r.u16(dict_off + 20)):
        ti = r.vptr(tl + 4 * i)
        name = r.cstr(r.vptr(ti + 20))
        bare = name.split('/')[-1]
        if bare.lower().endswith('.dds'):
            bare = bare[:-4]
        fmt = bytes(r.virt[ti + 32:ti + 36]).decode('latin-1')
        data = r.resolve(r.u32(ti + 72))
        out.setdefault(bare.lower(), dict(name=bare.lower(), w=r.u16(ti + 28), h=r.u16(ti + 30),
                                          fmt=fmt, levels=r.u8(ti + 39), res=r,
                                          data=data[1] if data else None))
    return out


def build_xtd(tmpl, sources, wanted, log=print, options=None):
    """Template 360 dictionary kept whole (its textures stay valid); donor
    textures appended; hash table and list rebuilt, sorted by hash."""
    t = tmpl
    v = Seg(t.virt[:align(max(o for o in range(t.vsize) if t.virt[o] not in (0, 0xCD)) + 1, 16)],
            VIRTUAL_PAGE)
    p_objs = []   # new texture data, placed by place_physical once all are known
    patches = []  # (texture header, its fetch constant words, index in p_objs)
    tl, n = t.vptr(24), t.u16(20)
    ht = t.vptr(16)
    entries = []  # (hash, info offset)
    have = set()
    t_info = t_d3d = None
    for i in range(n):
        info = t.vptr(tl + 4 * i)
        entries.append((t.u32(ht + 4 * i), info))
        nm = t.cstr(t.vptr(info + 20)).split('/')[-1].lower()
        have.add(nm[:-4] if nm.endswith('.dds') else nm)
        d3d = t.vptr(info + 24)
        if t.u32(info + 32) == 1:  # a single-level template to clone
            t_info, t_d3d = t.virt[info:info + 0x40], t.virt[d3d:d3d + 0x34]
    added = []
    for name in sorted(wanted):
        if name in have:
            continue
        src = next((s[name] for s in sources if name in s), None)
        if src is None:
            continue
        src = fit_texture(src, texture_side(name, options))
        w, h = src['w'], src['h']
        bw, bh = max(1, w // 4), max(1, h // 4)
        fmt, bpp, lin, label = linear_texture(src)
        if label:
            src = dict(src, fmt=label)
        tiled = xenos.tile_level(xenos.swap16(lin), bw, bh, bpp)
        f = list(struct.unpack_from('>6I', t_d3d, 28))
        pitch = (align(bw, 32) * 4) >> 5
        f[0] = (f[0] & ~(0x1FF << 22)) | (pitch << 22)
        f[1] = fmt | (1 << 6) | (f[1] & 0xF00)  # base address: once placed
        f[2] = (f[2] & ~0x03FFFFFF) | (w - 1) | ((h - 1) << 13)
        f[4] = f[4] & ~(0xFF << 2)                      # mip min = max = 0
        f[5] = (f[5] & ~0xFFFFF800) | (1 << 9) & 0x600  # 2D, no packed mips, no mip address
        # A name with its own ".dds" also answers to the bare name (the same
        # data): whichever way a reference asks for it, it is found.
        names = [name] + ([name[:-4]] if name.endswith('.dds') and name[:-4] not in wanted else [])
        for alias in names:
            info = v.put(t_info)
            d3d = v.put(t_d3d)
            nm = v.cstr('pack:/%s.dds' % alias)
            v.w32(info + 20, VP | nm)
            v.w32(info + 24, VP | d3d)
            v.w16(info + 28, w)
            v.w16(info + 30, h)
            v.w32(info + 32, 1)
            patches.append((d3d, list(f), len(p_objs)))
            entries.append((joaat(alias), info))
            have.add(alias)
        p_objs.append((tiled, 0x1000))
        added.append('%s %dx%d %s%s' % (name, w, h, src['fmt'],
                                        ' (from %dx%d)' % src['source_size'] if src.get('dropped') else ''))
        have.add(name)
    # Textures the model borrows from another stock car's dictionary (not the
    # shared vehicle pack every car can see) get a plain black stand-in, so
    # nothing is drawn with a missing texture.
    for name in sorted(set(wanted) - have):
        if name.startswith('vehicle_generic'):
            continue
        lin = dxt.encode_dxt5(bytes([0, 0, 0, 255]) * 16 * 16, 16, 16)
        fmt, bpp = xenos.FORMATS['DXT5']
        tiled = xenos.tile_level(xenos.swap16(lin), 4, 4, bpp)
        info = v.put(t_info)
        d3d = v.put(t_d3d)
        nm = v.cstr('pack:/%s.dds' % name)
        v.w32(info + 20, VP | nm)
        v.w32(info + 24, VP | d3d)
        v.w16(info + 28, 16)
        v.w16(info + 30, 16)
        v.w32(info + 32, 1)
        f = list(struct.unpack_from('>6I', t_d3d, 28))
        f[0] = (f[0] & ~(0x1FF << 22)) | (((32 * 4) >> 5) << 22)
        f[1] = fmt | (1 << 6) | (f[1] & 0xF00)
        f[2] = (f[2] & ~0x03FFFFFF) | 15 | (15 << 13)
        f[4] = f[4] & ~(0xFF << 2)
        f[5] = (f[5] & ~0xFFFFF800) | (1 << 9) & 0x600
        patches.append((d3d, f, len(p_objs)))
        p_objs.append((tiled, 0x1000))
        entries.append((joaat(name), info))
        added.append('%s 16x16 black stand-in' % name)
        have.add(name)
    entries.sort()
    assert len({h for h, _ in entries}) == len(entries), 'hash collision'
    hs = v.alloc(4 * len(entries))
    ls = v.alloc(4 * len(entries))
    for k, (h, info) in enumerate(entries):
        v.w32(hs + 4 * k, h)
        v.w32(ls + 4 * k, VP | info)
    v.w32(16, VP | hs)
    v.w16(20, len(entries))
    v.w16(22, len(entries))
    v.w32(24, VP | ls)
    v.w16(28, len(entries))
    v.w16(30, len(entries))
    missing = sorted(set(wanted) - have)
    log('textures: %d added (%s); template kept %d; resolved elsewhere (vehicle shared pack): %s'
        % (len(added), ', '.join(added), n, ', '.join(missing)))
    # The template's texture data stays where it is: each of its own load
    # chunks must still fall inside one chunk of the new layout.
    pieces = [(cs, min(ce, len(t.phys))) for cs, ce in segment_chunks(t.flags) if cs < len(t.phys)]
    phys, offs, pcount, pshift = place_physical(p_objs, t.phys, pieces)
    for d3d, f, k in patches:
        f[1] |= (PP | offs[k]) & 0xFFFFF000
        struct.pack_into('>6I', v.buf, d3d + 28, *f)
    flags, vtot, _ = make_flags(t.flags, len(v.buf), pcount, pshift)
    return flags, bytes(v.buf) + b'\0' * (vtot - len(v.buf)) + phys
