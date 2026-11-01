"""Landing gear for a mod whose wheels hang bare under its body (the Batwing's:
three black discs, no legs). Applied to the PC donor in memory before it is
converted (run_vehicle.py), and by validate_vehicle.py to the donor it checks
against, so the conversion is still checked vertex for vertex.

  gear=wheel_lr:-1.235:-0.62:0.045;wheel_rr:1.235:-0.62:0.045;wheel_lf:0:-1.05:0.04
      a leg for each wheel bone: its strut at x, from the top z (up in the
      body: where the trainer's gear hinge goes) down to the wheel's axle,
      that thick; and an axle from the strut to the wheel's middle (or, for
      twin wheels astride the strut, from one wheel to the other).
  move=wheel_lr:0:0:-0.15;wheel_rr:0:0:-0.15
      the geometry bound to those bones moved first (the main wheels down to
      the nose wheel's level, so the aircraft stands level).

With gear=, the geometries drawn only on wheel bones (the wheels and their
hubs) get texture coordinates of their own on the wheel texture
(gear_texture.py paints it): a face planar-mapped onto the wheel picture
(hub, rim, tyre wall, by radius), a tread across its width, and the legs on
the strut strip (a painted strut over a chrome oleo).

The legs are appended to the first wheel geometry each bone draws in, with
that geometry's vertex format; the vertex and index data are rewritten at
the end of the physical segment and the geometry pointed there.
"""
import math
import struct

import rsc5
import batconv

# The wheel texture's layout (gear_texture.py), in texture coordinates.
FACE_CENTRE = (0.25, 0.25)   # the wheel's face: a disc of this radius
FACE_RADIUS = 0.24
HUB_EDGE = 0.55              # the metal wheel inside this share of the face's radius
TREAD_U = (0.52, 0.98)       # across the tread (inner to outer side)
TREAD_V = 0.25
STRUT_V = (0.52, 0.99)       # down the strut (top to the axle)
AXLE_V = 0.60                # (an axle on the strut's painted part)
SEGMENTS = 10


def parse(text):
    legs = {}
    for item in filter(None, (text or '').split(';')):
        b, x, top, r = item.split(':')
        legs[b] = (float(x), float(top), float(r))
    return legs


def _geometries(d, dr):
    """(lod, geometry dict) for every geometry of the drawable."""
    for li, lod in enumerate(dr['lods']):
        for m in lod:
            for g in m['geometries']:
                yield li, g


def _table(d, g):
    bp = d.vptr(g['off'] + 56)
    return [d.u16(bp + 2 * k) for k in range(d.u16(g['off'] + 62))] if bp is not None else []


def _cylinder(a, b, radius, v0, v1, segments=SEGMENTS, cap_b=False):
    """A tube from a to b: (positions, normals, uvs, triangles), u around (the
    seam doubled), v from v0 at a to v1 at b; cap_b closes b's end."""
    ax = [b[k] - a[k] for k in range(3)]
    n = math.sqrt(sum(c * c for c in ax))
    ax = [c / n for c in ax]
    ref = (1.0, 0.0, 0.0) if abs(ax[0]) < 0.9 else (0.0, 0.0, 1.0)
    e1 = [ref[1] * ax[2] - ref[2] * ax[1], ref[2] * ax[0] - ref[0] * ax[2], ref[0] * ax[1] - ref[1] * ax[0]]
    m = math.sqrt(sum(c * c for c in e1))
    e1 = [c / m for c in e1]
    e2 = [ax[1] * e1[2] - ax[2] * e1[1], ax[2] * e1[0] - ax[0] * e1[2], ax[0] * e1[1] - ax[1] * e1[0]]
    pos, nrm, uv, tri = [], [], [], []
    for s in range(segments + 1):
        t = 2 * math.pi * s / segments
        dirn = [math.cos(t) * e1[k] + math.sin(t) * e2[k] for k in range(3)]
        for end, v in ((a, v0), (b, v1)):
            pos.append(tuple(end[k] + radius * dirn[k] for k in range(3)))
            nrm.append(tuple(dirn))
            uv.append((s / segments, v))
    for s in range(segments):
        i = 2 * s
        tri += [(i, i + 2, i + 1), (i + 1, i + 2, i + 3)]
    if cap_b:
        c = len(pos)
        pos.append(tuple(b))
        nrm.append(tuple(ax))
        uv.append((0.5, v1))
        ring = []
        for s in range(segments):
            t = 2 * math.pi * s / segments
            dirn = [math.cos(t) * e1[k] + math.sin(t) * e2[k] for k in range(3)]
            ring.append(len(pos))
            pos.append(tuple(b[k] + radius * dirn[k] for k in range(3)))
            nrm.append(tuple(ax))
            uv.append((0.5, v1))
        for s in range(segments):
            tri.append((c, ring[(s + 1) % segments], ring[s]))
    return pos, nrm, uv, tri


def apply(d, options):
    """Patch the PC donor `d` (rsc5.Resource) in place; returns a report."""
    legs = options.get('gear') or {}
    move = options.get('move') or {}
    if not legs and not move:
        return None
    if not isinstance(d.virt, bytearray):
        d.virt = bytearray(d.virt)
        d.phys = bytearray(d.phys)
    f = rsc5.read_fragtype(d)
    dr = f['drawable']
    _, bones = batconv.read_skeleton(d, dr['skeleton_off'])
    names = [b['name'] for b in bones]
    for b in list(legs) + list(move):
        assert b in names, 'gear: no bone %s in the donor' % b

    def unpack(g):
        usage = g['decl']['usage']
        n = g['vertex_count'] or g['vb_count']
        st = g['vb_stride']
        at = dict(batconv.vertex_layout(usage, batconv.SIZE_PC)[0])
        vb = bytearray(d.phys[g['vb_data'][1]:g['vb_data'][1] + n * st])
        table = _table(d, g)
        bone_of = []
        for v in range(n):
            k = batconv.main_influence(vb, at, v, st)
            bone_of.append(table[k] if k < len(table) else -1)
        idx = list(struct.unpack_from('<%dH' % g['ib_count'], d.phys, g['ib_data'][1]))
        return usage, n, st, at, vb, table, bone_of, idx

    geoms = list(_geometries(d, dr))
    data = {g['off']: unpack(g) for _, g in geoms}
    report = {'moved': 0, 'uv': 0, 'legs': []}

    # 1. moves
    for _, g in geoms:
        usage, n, st, at, vb, table, bone_of, idx = data[g['off']]
        for v in range(n):
            mv = move.get(names[bone_of[v]]) if bone_of[v] >= 0 else None
            if mv:
                o = v * st + at[0]
                p = struct.unpack_from('<3f', vb, o)
                struct.pack_into('<3f', vb, o, *(p[k] + mv[k] for k in range(3)))
                report['moved'] += 1

    # 2. the wheels: each wheel bone's centre, radius, sides, hub
    wheel_bones = {names.index(b) for b in legs}
    wheel_geoms = [g for li, g in geoms if li == 0 and data[g['off']][6] and
                   all(b in wheel_bones for b in data[g['off']][6])]
    info = {}
    for b in wheel_bones:
        pts = []
        for g in wheel_geoms:
            usage, n, st, at, vb, table, bone_of, idx = data[g['off']]
            pts += [struct.unpack_from('<3f', vb, v * st + at[0]) for v in range(n) if bone_of[v] == b]
        if not pts:
            continue
        lo = [min(p[k] for p in pts) for k in range(3)]
        hi = [max(p[k] for p in pts) for k in range(3)]
        cy, cz = (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2
        radius = (hi[2] - lo[2]) / 2
        # twin wheels: a gap across x through the middle
        xs = sorted({round(p[0], 4) for p in pts})
        gaps = [(xs[i + 1] - xs[i], xs[i], xs[i + 1]) for i in range(len(xs) - 1)]
        widest = max(gaps) if gaps else (0, 0, 0)
        twin = widest[0] > 0.1 and widest[1] < (lo[0] + hi[0]) / 2 < widest[2]
        # the hub: the geometry whose vertices of this bone all lie well inside the tyre
        hub = 0.0
        for g in wheel_geoms:
            usage, n, st, at, vb, table, bone_of, idx = data[g['off']]
            rs = [math.hypot(struct.unpack_from('<3f', vb, v * st + at[0])[1] - cy,
                             struct.unpack_from('<3f', vb, v * st + at[0])[2] - cz) for v in range(n) if bone_of[v] == b]
            if rs and max(rs) < 0.9 * radius:
                hub = max(hub, max(rs))
        info[b] = dict(lo=lo, hi=hi, cy=cy, cz=cz, radius=radius, hub=hub or 0.6 * radius, twin=twin,
                       gap=(widest[1], widest[2]) if twin else None)

    def tread_u(b, x):
        w = info[b]
        if w['twin']:
            # each wheel of the pair: from its inner side (by the gap) out
            inner, outer = (w['gap'][1], w['hi'][0]) if x > (w['lo'][0] + w['hi'][0]) / 2 else (w['gap'][0], w['lo'][0])
        else:
            mid = (w['lo'][0] + w['hi'][0]) / 2
            inner, outer = (w['lo'][0], w['hi'][0]) if mid > 0 else (w['hi'][0], w['lo'][0])
        t = (x - inner) / (outer - inner) if outer != inner else 0.5
        return TREAD_U[0] + (TREAD_U[1] - TREAD_U[0]) * min(max(t, 0.0), 1.0)

    # 3. their texture coordinates
    if legs:
        for g in wheel_geoms:
            usage, n, st, at, vb, table, bone_of, idx = data[g['off']]
            if 6 not in at:
                continue
            for v in range(n):
                b = bone_of[v]
                if b not in info:
                    continue
                w = info[b]
                p = struct.unpack_from('<3f', vb, v * st + at[0])
                nx = struct.unpack_from('<3f', vb, v * st + at[3])[0] if 3 in at else 1.0
                if abs(nx) > 0.7:  # a face: by radius onto the wheel picture
                    dy, dz = p[1] - w['cy'], p[2] - w['cz']
                    r = math.hypot(dy, dz)
                    if r <= w['hub']:
                        rt = HUB_EDGE * r / w['hub'] if w['hub'] else 0.0
                    else:
                        rt = HUB_EDGE + (1 - HUB_EDGE) * min((r - w['hub']) / max(w['radius'] - w['hub'], 1e-6), 1.0)
                    s = rt * FACE_RADIUS / r if r > 1e-6 else 0.0
                    uv = (FACE_CENTRE[0] + dy * s, FACE_CENTRE[1] - dz * s)
                else:  # the tread
                    uv = (tread_u(b, p[0]), TREAD_V)
                struct.pack_into('<2f', vb, v * st + at[6], *uv)
                report['uv'] += 1

    # 4. the legs, on the first wheel geometry each bone draws in (LOD 0)
    adds = {}
    for bname, (sx, top, radius) in legs.items():
        b = names.index(bname)
        w = info.get(b)
        if w is None:
            continue
        g = next(g for g in wheel_geoms if b in data[g['off']][6])
        usage, n, st, at, vb, table, bone_of, idx = data[g['off']]
        proto = bone_of.index(b)  # a vertex of this wheel: its weights, bone slot, colour
        axle = (sx, w['cy'], w['cz'])
        parts = [_cylinder((sx, w['cy'], top), (sx, w['cy'], w['cz'] - radius * 0.6), radius,
                           STRUT_V[0], STRUT_V[1], cap_b=True)]
        if w['twin']:
            a0, a1 = w['gap'][0] - 0.01, w['gap'][1] + 0.01
            parts.append(_cylinder((a0, w['cy'], w['cz']), (a1, w['cy'], w['cz']), radius * 0.7, AXLE_V, AXLE_V))
        else:
            mid = (w['lo'][0] + w['hi'][0]) / 2
            parts.append(_cylinder(axle, (mid, w['cy'], w['cz']), radius * 0.65, AXLE_V, AXLE_V))
        new = adds.setdefault(g['off'], {'verts': bytearray(), 'tris': [], 'count': 0})
        for pos, nrm, uv, tri in parts:
            base = n + new['count']
            for p_, n_, uv_ in zip(pos, nrm, uv):
                rec = bytearray(vb[proto * st:(proto + 1) * st])
                struct.pack_into('<3f', rec, at[0], *p_)
                if 3 in at:
                    struct.pack_into('<3f', rec, at[3], *n_)
                if 6 in at:
                    struct.pack_into('<2f', rec, at[6], *uv_)
                if 14 in at:  # (a tangent along the tube's u)
                    struct.pack_into('<4f', rec, at[14], -n_[1], n_[0], 0.0, 1.0)
                new['verts'] += rec
            new['tris'] += [(base + i, base + j, base + k) for i, j, k in tri]
            new['count'] += len(pos)
        report['legs'].append('%s: strut x %.3f from z %.2f to the axle at (%.2f, %.2f), r %.3f%s' %
                              (bname, sx, top, w['cy'], w['cz'], radius, ', twin wheels' if w['twin'] else ''))

    # 5. write back: patched data in place, grown geometries at the end of the physical segment
    def append_phys(blob):
        off = (len(d.phys) + 15) & ~15
        d.phys += b'\0' * (off - len(d.phys)) + blob
        return off

    for _, g in geoms:
        usage, n, st, at, vb, table, bone_of, idx = data[g['off']]
        extra = adds.get(g['off'])
        if not extra:
            d.phys[g['vb_data'][1]:g['vb_data'][1] + n * st] = vb
            continue
        total = n + extra['count']
        assert total < 65536
        allidx = idx + [i for t in extra['tris'] for i in t]
        voff = append_phys(bytes(vb) + bytes(extra['verts']))
        ioff = append_phys(struct.pack('<%dH' % len(allidx), *allidx))
        vbo, ibo = g['vb_off'], g['ib_off']
        for slot in (8, 24):
            if d.u32(vbo + slot):
                struct.pack_into('<I', d.virt, vbo + slot, 0x60000000 | voff)
        struct.pack_into('<H', d.virt, vbo + 4, total)
        struct.pack_into('<I', d.virt, ibo + 4, len(allidx))
        struct.pack_into('<I', d.virt, ibo + 8, 0x60000000 | ioff)
        struct.pack_into('<I', d.virt, g['off'] + 44, len(allidx))
        struct.pack_into('<I', d.virt, g['off'] + 48, len(allidx) // 3)
        struct.pack_into('<H', d.virt, g['off'] + 52, total)
    d.psize = len(d.phys)
    d.data = bytes(d.virt) + bytes(d.phys)
    return report
