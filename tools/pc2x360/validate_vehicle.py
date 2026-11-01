"""Structural validation of a converted 360 vehicle against its PC donor.

usage: validate_vehicle.py <out_dir> <name> <pc.wft> <pc.wtd> [pivot]

pivot: as given to run_vehicle.py ("extra_1=misc_a,...").
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batconv  # noqa: E402
import dxt  # noqa: E402
import rsc5  # noqa: E402
import restpose  # noqa: E402
import xenos  # noqa: E402

out_dir, name, wft, wtd = sys.argv[1:5]
PIVOT, PIVOT_SOURCES = None, []
if len(sys.argv) > 5:
    PIVOT, _src = sys.argv[5].split('=')
    PIVOT_SOURCES = _src.split(',')
flags = dict(l.split()[0:3:2] for l in open(os.path.join(out_dir, 'flags.txt')))
x = rsc5.Resource(open(os.path.join(out_dir, name + '.xft.bin'), 'rb').read(), int(flags[name + '.xft'], 16), True, 112, 'xft')
t = rsc5.Resource(open(os.path.join(out_dir, name + '.xtd.bin'), 'rb').read(), int(flags[name + '.xtd'], 16), True, 7, 'xtd')
d = rsc5.Resource.pc(wft)
raw = open(wtd, 'rb').read()
_, rt, rf = struct.unpack_from('<III', raw, 0)
dtd = rsc5.Resource(zlib.decompress(raw[12:]), rf, False, rt, 'wtd')
fails = 0


def check(label, ok, detail=''):
    global fails
    print('%-66s %s %s' % (label, 'PASS' if ok else 'FAIL', detail))
    fails += 0 if ok else 1


assert len(x.data) == x.vsize + x.psize and len(t.data) == t.vsize + t.psize
xf, df = rsc5.read_fragtype(x), rsc5.read_fragtype(d)
xd, dd = xf['drawable'], df['drawable']
# the template's wheel child (the one holding bone wheel_lf's mesh slot); the
# donor may have none (wheels modelled into the body)
_, _dbn = batconv.bones(d, df['drawable']['skeleton_off'])
_dw = [(d.u16(c['off'] + 0x0E), c['drawable']) for c in df['children'] if c['drawable'] and c['drawable']['lods']]
dwheel = next((dr for b, dr in _dw if _dbn[b] == 'wheel_lf'), next((dr for b, dr in _dw if _dbn[b].startswith('wheel_')), None))
xwheel = next((c['drawable'] for c in xf['children'] if c['drawable'] and c['drawable']['lods']), None)
EMPTY = {'lods': []}
if dwheel is None:
    check('donor has no wheel mesh: no wheel meshes drawn', xwheel is None)
    xwheel = dwheel = EMPTY


def vcount(g):
    return g['vertex_count'] or g.get('vb_count', 0)
_, xbones = batconv.bones(x, xd['skeleton_off'])
dbo, dbones = batconv.bones(d, dd['skeleton_off'])

# every geometry: pointers in range, descriptors consistent, decoded data == donor
geo_out = [(g, 'main') for lod in xd['lods'] for m in lod for g in m['geometries']] + \
          [(g, 'wheel') for lod in xwheel['lods'] for m in lod for g in m['geometries']]
bad_ptr = bad_desc = bad_idx = 0
for g, _ in geo_out:
    for key, seg, size in (('vb_data', 'p', g['vertex_count'] * g['vb_stride']), ('ib_data', 'p', 2 * g['ib_count'])):
        s, o = g[key]
        if s != seg or o + size > x.psize:
            bad_ptr += 1
    if ((x.u32(g['vb_off'] + 0x3C) >> 2) & 0xFFFFFF) * 4 != g['vertex_count'] * g['vb_stride']:
        bad_desc += 1
    if x.u32(g['vb_off'] + 0x38) != (0x60000000 | g['vb_data'][1]) | 3:
        bad_desc += 1
    if x.u32(g['ib_off'] + 0x28) != 0x60000000 | g['ib_data'][1] or x.u32(g['ib_off'] + 0x2C) != 2 * g['ib_count']:
        bad_desc += 1
    if g['decl']['usage'] and batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_360)[1] != g['vb_stride']:
        bad_desc += 1
    idx = struct.unpack_from('>%dH' % g['ib_count'], x.phys, g['ib_data'][1])
    if max(idx) >= g['vertex_count'] or g['ib_count'] % 3:
        bad_idx += 1
check('%d geometries: buffer pointers inside the GPU segment' % len(geo_out), bad_ptr == 0, 'bad=%d' % bad_ptr)
check('vertex/index GPU descriptors and declarations consistent', bad_desc == 0, 'bad=%d' % bad_desc)
check('index buffers: whole triangles, indices in range', bad_idx == 0, 'bad=%d' % bad_idx)

# geometry counts and vertex totals equal the donor's (main LOD 0/1, wheel LOD 0/1)
def expanded(r, lod, bones):
    """Donor geometries of a LOD (with their bone tables)."""
    out = []
    for m in lod:
        for g in m['geometries']:
            bp = r.vptr(g['off'] + 56)
            table = [r.u16(bp + 2 * k) for k in range(r.u16(g['off'] + 62))] if bp is not None else []
            out.append((g, None, table))
    return out


def counts(draw, n, res=None, bones=None):
    if res is not None:
        return [(len(expanded(res, lod, bones)), sum(vcount(g) for g, _, _ in expanded(res, lod, bones)))
                for lod in draw['lods'][:n]]
    return [(sum(len(m['geometries']) for m in lod), sum(vcount(g) for m in lod for g in m['geometries']))
            for lod in draw['lods'][:n]]
nd = len(dd['lods'])
dcounts = counts(dd, 3, d, dbones)
check('main LOD geometry/vertex counts equal donor (%d donor LODs)' % nd,
      counts(xd, min(nd, 3)) == dcounts[:min(nd, 3)], str(counts(xd, 3)))
if nd < 3:
    check('main LOD 2 reuses the donor\'s last LOD (template has 3 LOD slots)', counts(xd, 3)[2] == dcounts[-1])
dw = counts(dwheel, 2)
check('wheel LOD geometry/vertex counts equal donor (a missing LOD repeats the last)',
      counts(xwheel, 2) == [dw[min(k, len(dw) - 1)] for k in range(len(counts(xwheel, 2)))] if dw else
      counts(xwheel, 2) == [], str(counts(xwheel, 2)))


# decode every output vertex back and compare with the donor's floats
def dec(i, b):
    u = struct.unpack('>I', b)[0]
    if i in (3, 14, 15):
        q = lambda v: (v - 1024 if v & 0x200 else v) / 511.0  # noqa: E731
        return [q((u >> s) & 0x3FF) for s in (0, 10, 20)]
    if 6 <= i <= 13:
        return list(struct.unpack('>2e', b))
    return list(b[::-1])


def dec_pc(i, b):
    if i == 0:
        return list(struct.unpack('<3f', b))
    if i in (3, 15):
        return list(batconv.unit(*struct.unpack('<3f', b)))
    if i == 14:
        return list(batconv.unit(*struct.unpack('<4f', b)[:3]))
    if 6 <= i <= 13:
        return list(struct.unpack('<2f', b))
    return list(b)


worst = {}
bone_bad = 0
pairs = list(zip([g for lod in xd['lods'][:3] for m in lod for g in m['geometries']],
                 [e for lod in dd['lods'][:3] for e in expanded(d, lod, dbones)])) + \
    list(zip([g for lod in xwheel['lods'][:2] for m in lod for g in m['geometries']],
             [(g, None, []) for lod in dwheel['lods'][:2] for m in lod for g in m['geometries']]))
pose_bad = 0
uv_fill_bad = 0
for xg, (dg, pose, dtable) in pairs:
    u = dg['decl']['usage']
    xu = xg['decl']['usage']
    assert xu & u == u and not (xu & ~u & ~batconv.UV_BITS & 0xFFFF), (hex(xu), hex(u))
    xl_all, _ = batconv.vertex_layout(xu, batconv.SIZE_360)
    xat = dict(xl_all)
    xl = [(i, xat[i]) for i, _ in batconv.vertex_layout(u, batconv.SIZE_360)[0]]
    dl, _ = batconv.vertex_layout(u, batconv.SIZE_PC)
    filled = [i for i in range(16) if (xu & ~u) & (1 << i)]
    xb, xs = xg['vb_data'][1], xg['vb_stride']
    db, ds = dg['vb_data'][1], dg['vb_stride']
    xbp, dbp = x.vptr(xg['off'] + 56), d.vptr(dg['off'] + 56)
    xtab = [x.u16(xbp + 2 * k) for k in range(x.u16(xg['off'] + 62))] if xbp is not None else []
    dtab = [d.u16(dbp + 2 * k) for k in range(d.u16(dg['off'] + 62))] if dbp is not None else []
    for xi, di in zip(xtab, dtab):  # bone tables: the same-named safe bone, the pivot, or the chassis
        want = batconv.BONE_ALIAS.get(dbones[di], dbones[di])
        got = xbones[xi]
        if dbones[di] in PIVOT_SOURCES:
            ok = got == PIVOT
        elif got == want:
            ok = batconv.carries(got)
        else:
            ok = got in (batconv.BONE_FALLBACK, PIVOT)
        if not ok:
            bone_bad += 1
    step = max(1, xg['vertex_count'] // 400)
    for v in range(0, xg['vertex_count'], step):
        for i in filled:  # a filled texcoord set repeats texcoord 0
            if xs and x.phys[xb + v * xs + xat[i]:xb + v * xs + xat[i] + 4] != x.phys[xb + v * xs + xat[6]:xb + v * xs + xat[6] + 4]:
                uv_fill_bad += 1
        for (i, xo), (_, do) in zip(xl, dl):
            if i == 0:
                a = list(struct.unpack_from('>3f', x.phys, xb + v * xs + xo))
            else:
                a = dec(i, bytes(x.phys[xb + v * xs + xo:xb + v * xs + xo + 4]))
            b = dec_pc(i, bytes(d.phys[db + v * ds + do:db + v * ds + do + batconv.SIZE_PC[i]]))
            if i in (6, 7, 8, 9, 10, 11, 12, 13):
                err = max(abs(p - q) / max(1.0, abs(q)) for p, q in zip(a, b))
            else:
                err = max(abs(p - q) for p, q in zip(a, b))
            worst[i] = max(worst.get(i, 0), err)
names = {0: 'position', 1: 'weights', 2: 'bone indices', 3: 'normal', 4: 'color', 14: 'tangent', 15: 'binormal', 6: 'uv0', 7: 'uv1'}
check('vertex positions decode exactly to the donor\'s', worst.get(0, 1) == 0)
check('normals/tangents within packing precision (<= 2/511)',
      max(worst.get(3, 0), worst.get(14, 0), worst.get(15, 0)) <= 2 / 511.0,
      'max err %.4f' % max(worst.get(3, 0), worst.get(14, 0)))
check('UVs within half precision (rel <= 2^-10)', max(worst.get(i, 0) for i in range(6, 14)) <= 2 ** -10,
      'max rel err %.6f' % max(worst.get(i, 0) for i in range(6, 14)))
check('weights/indices/colors carried byte-exact', max(worst.get(1, 0), worst.get(2, 0), worst.get(4, 0)) == 0)
check('bone tables: same-named safe bones, the pivot or the chassis', bone_bad == 0, 'bad=%d' % bone_bad)
_ok, _msg = restpose.check_rest_pose(x, PIVOT)
check('rest pose: every vertex skinned where it is stored; safe bones only', _ok, _msg.split(': ', 1)[1])
check('texcoord sets filled for the 360 shader repeat texcoord 0', uv_fill_bad == 0, 'bad=%d' % uv_fill_bad)

# shaders: inputs satisfied; texture names resolvable
sg = xd['shader_group']
need = [x.u32(x.vptr(sg['off'] + 64) + 4 * i) for i in range(len(sg['shaders']))]
unmet = 0
for lod in xd['lods'] + xwheel['lods']:
    for m in lod:
        smap = [x.u16(x.vptr(m['off'] + 16) + 2 * k) for k in range(len(m['geometries']))]
        for g, si in zip(m['geometries'], smap):
            if si >= len(need) or need[si] & ~g['decl']['usage']:
                unmet += 1
check('%d shaders: every geometry supplies its shader\'s inputs' % len(need), unmet == 0, 'bad=%d' % unmet)

tl, tn = t.vptr(24), t.u16(20)
hashes = [t.u32(t.vptr(16) + 4 * i) for i in range(tn)]
check('texture dictionary hash table sorted, unique', hashes == sorted(set(hashes)), '%d textures' % tn)
dict_names = set()
tex_bad = 0
src = batconv.pc_textures(dtd, 0)
emb = dd['shader_group']['texdict']
src_e = batconv.pc_textures(d, emb[1]) if emb else {}
for i in range(tn):
    info = t.vptr(tl + 4 * i)
    nm = t.cstr(t.vptr(info + 20)).split('/')[-1].lower()
    nm = nm[:-4] if nm.endswith('.dds') else nm
    dict_names.add(nm)
    if batconv.joaat(nm) != hashes[i]:
        tex_bad += 1
    s = src.get(nm) or src_e.get(nm)
    if s is None:
        continue  # a stock FBI texture, kept as-is
    d3d = t.vptr(info + 24)
    f = struct.unpack_from('>6I', t.virt, d3d + 28)
    base = ((f[1] >> 12) << 12) & 0x0FFFFFFF
    w, h = (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1
    fmt, bpp, lin, _ = batconv.linear_texture(s)
    bw, bh = max(1, w // 4), max(1, h // 4)
    tiled_size = xenos.align(bw, 32) * xenos.align(bh, 32) * bpp
    ok = (w, h) == (s['w'], s['h']) and (f[1] & 0x3F) == fmt and base % 4096 == 0 and \
        base + tiled_size <= t.psize and ((f[0] >> 22) & 0x1FF) << 5 == xenos.align(bw, 32) * 4
    back = xenos.untile_level(t.phys[base:base + tiled_size], bw, bh, bpp)
    if not ok or back != xenos.swap16(lin):
        tex_bad += 1
check('textures: names hash right; donor textures untile back to source exactly', tex_bad == 0, 'bad=%d' % tex_bad)

shader_tex = set()
for s in sg['shaders']:
    for p in batconv.shader_params(x, s['off']):
        if p['type'] == 0 and p['value'] is not None:
            shader_tex.add(x.cstr(x.vptr(p['value'] + 20)).lower())
unresolved = sorted(n for n in shader_tex if n not in dict_names and not n.startswith('vehicle_generic'))
check('shader textures resolve (own dictionary or vehicle_generic shared pack)', not unresolved, str(unresolved))
# The console loads the virtual segment page by page: no object reached
# through a pointer may straddle a page (Rockstar's packer never does).
def pointed_objects_xft(r):
    out = []
    def strlen(o):
        e = r.virt.find(b'\0', o)
        return (e - o + 1) if e >= 0 else 1
    def draw(d):
        for coll in batconv.lod_collections(r, d['off']):
            if coll is None:
                continue
            out.append(('lod collection', coll, 8))
            arr, n = r.vptr(coll), r.u16(coll + 4)
            if arr is not None:
                out.append(('model array', arr, 4 * n))
        for lod in d['lods']:
            for m in lod:
                n = len(m['geometries'])
                out.append(('model', m['off'], 0x20))
                ga, sm = r.vptr(m['off'] + 4), r.vptr(m['off'] + 16)
                if ga is not None:
                    out.append(('geometry array', ga, 4 * n))
                if sm is not None:
                    out.append(('shader map', sm, 2 * n))
                for g in m['geometries']:
                    # Buffers and their GPU headers are separate objects, each
                    # reached through its own pointer (+28 / +12).
                    out.extend([('geometry', g['off'], 0x50), ('vertex buffer', g['vb_off'], 0x20),
                                ('vertex buffer header', g['vb_off'] + 0x20, 0x20),
                                ('index buffer', g['ib_off'], 0x10),
                                ('index buffer header', g['ib_off'] + 0x10, 0x20)])
                    dcl = r.vptr(g['vb_off'] + 16)
                    if dcl is not None:
                        out.append(('declaration', dcl, 16))
                    bt = r.vptr(g['off'] + 56)
                    if bt is not None:
                        out.append(('bone table', bt, 2 * r.u16(g['off'] + 62)))
        sg = d['shader_group']
        if sg:
            n = len(sg['shaders'])
            for k, size in ((8, 4 * n), (64, 4 * n), (72, 4 * n)):
                a = r.vptr(sg['off'] + k)
                if a is not None:
                    out.append(('shader group array', a, size))
            for sh in sg['shaders']:
                out.append(('shader', sh['off'], 0x60))
                for k in (0x14, 0x24, 0x34):
                    a = r.vptr(sh['off'] + k)
                    cnt = r.u32(sh['off'] + 0x1C)
                    if a is not None:
                        out.append(('shader param array', a, {0x14: 4, 0x24: 1, 0x34: 4}[k] * cnt))
                for k in (0x44, 0x48):
                    a = r.vptr(sh['off'] + k)
                    if a is not None:
                        out.append(('shader name', a, strlen(a)))
                for p in batconv.shader_params(r, sh['off']):
                    if p['value'] is None:
                        continue
                    if p['type'] == 0:
                        out.append(('texture reference', p['value'], 32))
                        nm = r.vptr(p['value'] + 20)
                        if nm is not None:
                            out.append(('texture name', nm, strlen(nm)))
                    else:
                        out.append(('shader value', p['value'], 16 * p['type']))
    draw(xd)
    for c in xf['children']:
        if c['drawable'] and c['drawable']['lods']:
            draw(c['drawable'])
    return out


def pointed_objects_xtd(t):
    out = []
    tl, n = t.vptr(24), t.u16(20)
    out.extend([('texture hash table', t.vptr(16), 4 * n), ('texture list', tl, 4 * n)])
    for i in range(n):
        info = t.vptr(tl + 4 * i)
        out.append(('texture info', info, 0x40))
        out.append(('texture header', t.vptr(info + 24), 0x34))
        nm = t.vptr(info + 20)
        e = t.virt.find(b'\0', nm)
        out.append(('texture name', nm, e - nm + 1))
    return out


for label, res, objs in (('model', x, pointed_objects_xft(x)), ('textures', t, pointed_objects_xtd(t))):
    # At least 4 KiB: stock texture dictionaries (flags of 256-byte units)
    # cross 256-byte lines freely, but no stock car crosses a 4 KiB one.
    page = max(4096, 256 << ((res.flags >> 11) & 0xF))
    crossing = [(k, o, n) for k, o, n in objs if n and o // page != (o + n - 1) // page]
    kinds = sorted({k for k, _, _ in crossing})
    check('%s: %d pointed objects, none straddles a %d-byte page' % (label, len(objs), page),
          not crossing, ', '.join(kinds))

print('FAILURES: %d' % fails)
sys.exit(1 if fails else 0)
