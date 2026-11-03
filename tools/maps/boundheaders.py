"""Bound headers laid side by side: a console bound dictionary (.xbd) or
static bounds (.xbn) body, unpacked with pc2x360/xlzx unrsc, each bound type's
header words shown (pointers as P), and the conventions the two kinds of file
differ in checked (v1.7.34):

  - a composite's +0x50 (its centre of gravity): the dictionaries (props,
    objects with mass: rubbish.xbd, nj_01.xbd) hold the volume-weighted mean
    of its children's, the static bounds (nj_01_4.xbn) the plain sum of
    volume times centre
  - +0x6C (the volume): the dictionaries' meshes on their own -FLT_MAX (the
    game works it out), those in a composite their volume
  - load chunks: objects cross pages freely, but none crosses one of the
    chunks the console loads the segment in (batconv.load_chunks)

usage: python3 -I boundheaders.py <body.bin>:<flags hex>[:label] [...]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import bounds360  # noqa: E402
import rsc5  # noqa: E402

NEG_MAX = 0xFF7FFFFF
TYPES = {0: 'sphere', 1: 'capsule', 3: 'box', 4: 'geometry', 10: 'BVH', 12: 'composite'}


def f3(v, o):
    return struct.unpack_from('>3f', v, o)


def word(w):
    return '   P    ' if 0x50000000 <= w < 0x70000000 else '%08X' % w


def roots(r):
    v = r.virt
    if struct.unpack_from('>I', v, 0)[0] == 0xE8826100:  # (a dictionary: its values)
        n = struct.unpack_from('>H', v, 0x1C)[0]
        return [r.vptr(r.vptr(0x18) + 4 * i) for i in range(n)], 'dictionary'
    return [r.vptr(8)], 'static bounds'


def children(r, off):
    v = r.virt
    k = struct.unpack_from('>H', v, off + 0x90)[0]
    kids = [r.vptr(r.vptr(off + 0x80) + 4 * i) for i in range(k)]
    mats = r.vptr(off + 0x84)
    return kids, [struct.unpack_from('>16f', v, mats + 64 * i) for i in range(k)]


def check(spec):
    parts = spec.split(':')
    path, flags = parts[0], int(parts[1], 16)
    label = parts[2] if len(parts) > 2 else os.path.basename(path)
    r = rsc5.Resource(open(path, 'rb').read(), flags, True, 32, path)
    v = r.virt
    tops, kind = roots(r)
    shown, hist = {}, {}
    lone = [0, 0]  # meshes on their own: with -FLT_MAX at +0x6C, of all
    comps = []
    stack = [(o, True) for o in tops]
    while stack:
        off, top = stack.pop(0)
        t = v[off + 4]
        hist[t] = hist.get(t, 0) + 1
        if shown.get(t, 0) < 2:
            shown[t] = shown.get(t, 0) + 1
            w = struct.unpack_from('>60I', v, off)
            print('%s %s @%X%s:' % (label, TYPES.get(t, 'type %d' % t), off, ' (on its own)' if top else ''))
            for i in range(0, 0x80 // 4, 8):
                print('   +%02X ' % (4 * i) + ' '.join(word(x) for x in w[i:i + 8]))
        if t == 12:
            kids, mats = children(r, off)
            sv, s = 0.0, [0.0, 0.0, 0.0]
            for c, m in zip(kids, mats):
                vol, cg = struct.unpack_from('>f', v, c + 0x6C)[0], f3(v, c + 0x50)
                p = [cg[0] * m[j] + cg[1] * m[4 + j] + cg[2] * m[8 + j] + m[12 + j] for j in range(3)]
                if vol > -1e30:
                    sv += vol
                    s = [s[j] + vol * p[j] for j in range(3)]
            have = f3(v, off + 0x50)
            mean = [x / sv for x in s] if sv else None
            near = lambda a, b: all(abs(x - y) <= 1e-3 * max(1.0, abs(y)) for x, y in zip(a, b))  # noqa: E731
            comps.append('mean' if mean and near(have, mean) else 'sum' if near(have, s) else 'neither')
            stack += [(c, False) for c in kids]
        elif t in (4, 10) and top:
            lone[1] += 1
            lone[0] += struct.unpack_from('>I', v, off + 0x6C)[0] == NEG_MAX
    print('%s (%s): %s' % (label, kind, ', '.join('%d %s' % (n, TYPES.get(t, 'type %d' % t))
                                                   for t, n in sorted(hist.items()))))
    if comps:
        print('   composites\' centre of gravity (+0x50): %d the volume-weighted mean of their children\'s, %d the '
              'plain sum of volume times centre, %d neither' % (comps.count('mean'), comps.count('sum'),
                                                                comps.count('neither')))
    if lone[1]:
        print('   meshes on their own: %d of %d with -FLT_MAX for their volume (+0x6C)' % (lone[0], lone[1]))
    objs = bounds360.objects(r)
    page = 256 << ((flags >> 11) & 0xF)
    chunks = batconv.segment_chunks(flags, physical=False)
    cut = bounds360.chunk_crossings(r.data, flags)
    print('   %d objects, %d crossing a %d KiB page, %d cut by one of the %d load chunks (%s KiB)%s' % (
        len(objs), sum(1 for _, o, n in objs if n and o // page != (o + n - 1) // page), page // 1024, len(cut),
        len(chunks), '/'.join(str((e - s) // 1024) for s, e in chunks[:3]) + ('...' if len(chunks) > 3 else ''),
        ''.join(': %s at %X, %d B' % c for c in cut[:3])))


if __name__ == '__main__':
    for a in sys.argv[1:]:
        check(a)
