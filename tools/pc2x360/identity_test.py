"""Identity test: convert the stock PC FBI into the stock 360 FBI template.

Since both describe the same car, a correct converter must reproduce the
real 360 file's skeleton, shader parameter values and bounds exactly, and
its vertices must be found in the real 360 vertex data.

usage: identity_test.py <export_dir> <pc_fbi_dir> <work_dir>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batconv  # noqa: E402
import rsc5  # noqa: E402

export, pcdir, work = sys.argv[1:4]
tx = rsc5.Resource.x360(os.path.join(export, 'fbi.xft'), os.path.join(work, 'fbi.xft.bin'))
pc = rsc5.Resource.pc(os.path.join(pcdir, 'fbi.wft'))
conv = batconv.Converter(tx, pc, log=lambda *a: None)
flags, body = conv.run()
vs, ps = rsc5.seg_sizes(flags)
out = rsc5.Resource(body, flags, True, 112, 'identity')
fails = 0


def check(label, ok, detail=''):
    global fails
    print('%-62s %s %s' % (label, 'PASS' if ok else 'FAIL', detail))
    fails += 0 if ok else 1


# 1. skeleton: moved-to-donor-pose must equal the template where names match
tsk = conv.tf['drawable']['skeleton_off']
tbo, tn = batconv.bones(tx, tsk)
same = out.virt[tbo:tbo + 0xE0 * len(tn)] == tx.virt[tbo:tbo + 0xE0 * len(tn)]
xf = all(out.virt[tx.vptr(tsk + k):tx.vptr(tsk + k) + 64 * len(tn)] ==
         tx.virt[tx.vptr(tsk + k):tx.vptr(tsk + k) + 64 * len(tn)] for k in (8, 12, 16))
check('skeleton bones byte-identical to the real 360 FBI', same)
check('skeleton transforms (3 x %d matrices) byte-identical' % len(tn), xf)

# 2. bounds
od = rsc5.read_fragtype(out)['drawable']
check('fragment bounding sphere identical', out.virt[16:32] == tx.virt[16:32])
check('drawable center/min/max/radius identical',
      all(out.virt[od['off'] + k:od['off'] + k + 16] == tx.virt[od['off'] + k:od['off'] + k + 16]
          for k in (16, 32, 48)) and out.virt[od['off'] + 112:od['off'] + 116] == tx.virt[od['off'] + 112:od['off'] + 116])

# 3. shaders: every cloned parameter block equals the real block for that shader
td = conv.tf['drawable']
treal = {batconv.shader_sps(tx, s['off']): s['off'] for s in td['shader_group']['shaders']}
blocks_ok = 0
osh = od['shader_group']['shaders']
for s in osh:
    sps = batconv.shader_sps(out, s['off'])
    ts = treal[sps]
    a = [(p['hash'], p['type'], out.virt[p['value']:p['value'] + 16 * p['type']] if p['type'] and p['value'] is not None else None)
         for p in batconv.shader_params(out, s['off'])]
    b = [(p['hash'], p['type'], tx.virt[p['value']:p['value'] + 16 * p['type']] if p['type'] and p['value'] is not None else None)
         for p in batconv.shader_params(tx, ts)]
    names_a = [out.cstr(out.vptr(p['value'] + 20)) if p['type'] == 0 and p['value'] is not None else None
               for p in batconv.shader_params(out, s['off'])]
    names_b = [tx.cstr(tx.vptr(p['value'] + 20)) if p['type'] == 0 and p['value'] is not None else None
               for p in batconv.shader_params(tx, ts)]
    if a == b and names_a == names_b:
        blocks_ok += 1
    else:
        diff = [(hex(x[0]), x[1]) for x, y in zip(a, b) if x != y]
        print('   %s differs: %s %s' % (sps, diff[:4], [(x, y) for x, y in zip(names_a, names_b) if x != y][:3]))
check('shader parameter values + texture names equal real 360 (%d shaders)' % len(osh),
      blocks_ok == len(osh), '%d/%d' % (blocks_ok, len(osh)))

# 4. vertices: converted PC vertices must exist in the real 360 vertex data.
# Blend indices are palette slots into each geometry's bone table, and the
# two files group geometry differently, so they are compared as the bones
# they resolve to.
def vertex_set(r, draw):
    out_ = set()
    for m in draw['lods'][0]:
        for g in m['geometries']:
            base, st = g['vb_data'][1], g['vb_stride']
            usage = g['decl']['usage']
            bp, bc = r.vptr(g['off'] + 56), r.u16(g['off'] + 62)
            table = [r.u16(bp + 2 * k) for k in range(bc)] if bp is not None else []
            layout, _ = batconv.vertex_layout(usage, batconv.SIZE_360)
            lay = dict(layout)
            bi_off, bw_off = lay.get(2), lay.get(1)
            for i in range(g['vertex_count']):
                v = bytes(r.phys[base + i * st:base + (i + 1) * st])
                infl = ()
                rest = v
                if bi_off is not None and bw_off is not None and table:
                    # zero-weight slots carry arbitrary indices: compare only
                    # weighted influences, as (resolved bone, weight)
                    infl = tuple(sorted((table[v[bi_off + k]], v[bw_off + k])
                                        for k in range(4) if v[bw_off + k]))
                    rest = v[:bi_off] + v[bi_off + 4:]
                out_.add((usage, rest, infl))
    return out_


real = vertex_set(tx, td)
conv_v = vertex_set(out, od)
exact = len(conv_v & real)
check('converted lod0 vertices identical to real 360 (all elements)',
      exact == len(conv_v), '%d/%d (%.2f%%)' % (exact, len(conv_v), 100.0 * exact / len(conv_v)))

# 5. index/fetch consistency on the output
bad = 0
for lod in od['lods']:
    for m in lod:
        for g in m['geometries']:
            vb, ib = g['vb_off'], g['ib_off']
            f1 = out.u32(vb + 0x3C)
            if ((f1 >> 2) & 0xFFFFFF) * 4 != g['vertex_count'] * g['vb_stride']:
                bad += 1
            if out.u32(ib + 0x2C) != 2 * g['ib_count']:
                bad += 1
            idx = struct.unpack_from('>%dH' % g['ib_count'], out.phys, g['ib_data'][1])
            if max(idx) >= g['vertex_count']:
                bad += 1
            if g['decl']['stride'] & 0xFF00:
                pass
check('vertex/index GPU descriptors match buffer sizes, indices in range', bad == 0, 'bad=%d' % bad)
print('FAILURES: %d' % fails)
sys.exit(1 if fails else 0)
