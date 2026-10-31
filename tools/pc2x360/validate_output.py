"""Structural validation of a converted 360 vehicle against its PC donor.

usage: validate_output.py <out_dir> <oiv_car_dir>
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batconv  # noqa: E402
import rsc5  # noqa: E402
import xenos  # noqa: E402

out_dir, car = sys.argv[1:3]
flags = dict(l.split()[0:3:2] for l in open(os.path.join(out_dir, 'flags.txt')))
x = rsc5.Resource(open(os.path.join(out_dir, 'fbi.xft.bin'), 'rb').read(), int(flags['fbi.xft'], 16), True, 112, 'xft')
t = rsc5.Resource(open(os.path.join(out_dir, 'fbi.xtd.bin'), 'rb').read(), int(flags['fbi.xtd'], 16), True, 7, 'xtd')
d = rsc5.Resource.pc(os.path.join(car, 'fbi.wft'))
raw = open(os.path.join(car, 'fbi.wtd'), 'rb').read()
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
xwheel = next(c['drawable'] for c in xf['children'] if c['drawable'] and c['drawable']['lods'])
dwheel = next(c['drawable'] for c in df['children'] if c['drawable'] and c['drawable']['lods'])
_, xbones = batconv.bones(x, xd['skeleton_off'])
_, dbones = batconv.bones(d, dd['skeleton_off'])

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
def counts(draw, n):
    return [(sum(len(m['geometries']) for m in lod), sum(g['vertex_count'] for m in lod for g in m['geometries']))
            for lod in draw['lods'][:n]]
check('main LOD 0-1 geometry/vertex counts equal donor', counts(xd, 2) == counts(dd, 2), str(counts(xd, 3)))
check('main LOD 2 reuses donor LOD 1 (template has 3 LOD slots)', counts(xd, 3)[2] == counts(dd, 2)[1])
check('wheel LOD 0-1 geometry/vertex counts equal donor', counts(xwheel, 2) == counts(dwheel, 2), str(counts(xwheel, 2)))


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
        return list(struct.unpack('<3f', b))
    if i == 14:
        return list(struct.unpack('<4f', b))[:3]
    if 6 <= i <= 13:
        return list(struct.unpack('<2f', b))
    return list(b)


worst = {}
bone_bad = 0
pairs = list(zip([g for lod in xd['lods'][:2] for m in lod for g in m['geometries']],
                 [g for lod in dd['lods'][:2] for m in lod for g in m['geometries']])) + \
    list(zip([g for lod in xwheel['lods'][:2] for m in lod for g in m['geometries']],
             [g for lod in dwheel['lods'][:2] for m in lod for g in m['geometries']]))
for xg, dg in pairs:
    u = xg['decl']['usage']
    assert u == dg['decl']['usage']
    xl, _ = batconv.vertex_layout(u, batconv.SIZE_360)
    dl, _ = batconv.vertex_layout(u, batconv.SIZE_PC)
    xb, xs = xg['vb_data'][1], xg['vb_stride']
    db, ds = dg['vb_data'][1], dg['vb_stride']
    xbp, dbp = x.vptr(xg['off'] + 56), d.vptr(dg['off'] + 56)
    xtab = [x.u16(xbp + 2 * k) for k in range(x.u16(xg['off'] + 62))] if xbp is not None else []
    dtab = [d.u16(dbp + 2 * k) for k in range(d.u16(dg['off'] + 62))] if dbp is not None else []
    for xi, di in zip(xtab, dtab):  # bone tables bind the same-named bones
        if xbones[xi] != batconv.BONE_ALIAS.get(dbones[di], dbones[di]):
            bone_bad += 1
    step = max(1, xg['vertex_count'] // 400)
    for v in range(0, xg['vertex_count'], step):
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
check('bone tables bind the same-named FBI bones', bone_bad == 0, 'bad=%d' % bone_bad)

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
    fmt, bpp = xenos.FORMATS[s['fmt']]
    bw, bh = w // 4, h // 4
    tiled_size = xenos.align(bw, 32) * xenos.align(bh, 32) * bpp
    ok = (w, h) == (s['w'], s['h']) and (f[1] & 0x3F) == fmt and base % 4096 == 0 and \
        base + tiled_size <= t.psize and ((f[0] >> 22) & 0x1FF) << 5 == xenos.align(bw, 32) * 4
    lin = s['res'].phys[s['data']:s['data'] + bw * bh * bpp]
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
print('FAILURES: %d' % fails)
sys.exit(1 if fails else 0)
