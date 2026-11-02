"""The rules collide.py builds its BVH meshes by, checked against real files:
the PC map mod's .wbn (little-endian) or a stock console .xbn (big-endian).

  - nodes depth first: an inner node's children at i+1 and i+1+size(i+1), its
    u16 the size of its subtree; a leaf's u16 its first polygon, its u8 how
    many (leaves cover the polygons in order, four each but one)
  - a leaf's box the floor and ceil of its polygons' corners quantized; an
    inner box exactly the union of its children's
  - subtree headers: for each node with over 128 nodes under it, those of its
    children with 128 or fewer (Bullet's rule); the root's if it is small
  - neighbours: the polygon across edge k (corner k to the next round it)
  - quads: corners in order round a flat convex polygon (each turn agrees
    with the normal), area the whole quad's; a triangle's fourth corner 0

usage: python3 -I treerules.py <file.wbn> [...]
       python3 -I treerules.py --console <body.bin>:<flags> [...]
  (console: an .xbn or .xbd body unpacked with pc2x360/xlzx unrsc)
"""
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import bounds360  # noqa: E402
import rsc5  # noqa: E402


def meshes_pc(path):
    raw = open(path, 'rb').read()
    _, rtype, flags = struct.unpack_from('<III', raw, 0)
    r = rsc5.Resource(zlib.decompress(raw[12:]), flags, False, rtype, path)
    stack, out = [bounds360.read_bound(r, r.vptr(8))], []
    while stack:
        b = stack.pop()
        if b['type'] == 12:
            stack += b['children']
        else:
            out.append(b)
    return out


def meshes_console(arg):
    import xbdcheck
    path, flags = arg.rsplit(':', 1)
    r = rsc5.Resource(open(path, 'rb').read(), int(flags, 16), True, 32, path)
    if struct.unpack_from('>I', r.virt, 0)[0] == bounds360.VT_DICT:  # (a dictionary: its values)
        n = struct.unpack_from('>H', r.virt, 0x1C)[0]
        stack = [r.vptr(r.vptr(0x18) + 4 * i) for i in range(n)]
    else:  # (static bounds: a container pointing at one)
        stack = [r.vptr(8)]
    out = []
    while stack:
        off = stack.pop()
        kind = r.virt[off + 4]
        if kind == 12:
            k = struct.unpack_from('>H', r.virt, off + 0x90)[0]
            stack += [r.vptr(r.vptr(off + 0x80) + 4 * i) for i in range(k)]
        elif kind == 10:
            out.append(xbdcheck.read_mesh(r, off))
    return out


def check(m, label):
    n = len(m['polygons']) // 32
    raw = np.frombuffer(m['polygons'], np.uint8).reshape(n, 32)
    h = raw[:, 16:32].copy().view('<u2').reshape(n, 8).astype(np.int64)
    nrm = raw[:, :12].copy().view('<f4').reshape(n, 3).astype(np.float64)
    v = np.frombuffer(m['vertices'], '<i2').reshape(-1, 3).astype(np.float64) * np.array(m['quantum']) + \
        np.array(m['offset'])
    quad = h[:, 3] != 0
    four = h[:, :4].copy()
    four[~quad, 3] = four[~quad, 0]
    t = m['tree']
    lo, q = np.array(t['min']), np.array(t['quant'])
    nodes = np.frombuffer(t['nodes'], np.uint8).reshape(-1, 16)
    nh = nodes[:, :14].copy().view('<u2').reshape(-1, 7).astype(np.int64)
    cnt = nodes[:, 14].astype(np.int64)
    N = len(nh)

    def size(i):
        return 1 if cnt[i] else int(nh[i, 6])

    leaf_exact = leaf_near = inner = 0
    covered = 0
    for i in range(N):
        if cnt[i]:
            assert nh[i, 6] == covered, (label, i)
            covered += cnt[i]
            pts = v[four[nh[i, 6]:nh[i, 6] + cnt[i]].reshape(-1)]
            e = np.concatenate([np.floor((pts.min(0) - lo) * q), np.ceil((pts.max(0) - lo) * q)])
            d = np.abs(nh[i, :6] - e).max()
            leaf_exact += d == 0
            leaf_near += 0 < d <= 1
            assert d <= 1, (label, i, nh[i, :6], e)
        else:
            left = i + 1
            right = left + size(left)
            assert right + size(right) == i + nh[i, 6], (label, i)
            assert (nh[i, :3] == np.minimum(nh[left, :3], nh[right, :3])).all()
            assert (nh[i, 3:6] == np.maximum(nh[left, 3:6], nh[right, 3:6])).all()
            inner += 1
    assert covered == n
    want = []

    def walk(i):
        if cnt[i]:
            return
        left = i + 1
        right = left + size(left)
        walk(left)
        walk(right)
        if nh[i, 6] > 128:
            for c in (left, right):
                if size(c) <= 128:
                    want.append((c, c + size(c)))

    walk(0)
    want = want or [(0, N)]
    subs = np.frombuffer(t['subtrees'], '<u2').reshape(-1, 8).astype(np.int64)
    assert [(s[6], s[7]) for s in subs] == want, label
    edges = {}
    for p in range(n):
        k_n = 4 if quad[p] else 3
        for e in range(k_n):
            a, b = h[p, e], h[p, (e + 1) % k_n]
            edges.setdefault((min(a, b), max(a, b)), []).append(p)
    for p in range(n):
        k_n = 4 if quad[p] else 3
        for e in range(k_n):
            nb = h[p, 4 + e]
            if nb != 0xFFFF:
                a, b = h[p, e], h[p, (e + 1) % k_n]
                assert nb in edges[(min(a, b), max(a, b))], (label, p, e)
    convex = 0
    for k in range(4):
        turn = (np.cross(v[four[:, (k + 1) % 4]] - v[four[:, k]], v[four[:, (k + 2) % 4]] - v[four[:, (k + 1) % 4]]) *
                nrm).sum(1)
        convex += int((turn[quad] > 0).all())
    print('%s: %d polygons (%d quads), %d nodes: leaves %d exact, %d within one step; %d inner unions; %d subtree '
          'headers by the rule; neighbours across their edges; quads %s' % (
              label, n, int(quad.sum()), N, leaf_exact, leaf_near, inner, len(subs),
              'convex in order' if convex == 4 else 'NOT all convex in order'))


def main():
    args = sys.argv[1:]
    console = '--console' in args
    for a in [x for x in args if x != '--console']:
        for k, m in enumerate(meshes_console(a) if console else meshes_pc(a)):
            check(m, '%s[%d]' % (os.path.basename(a.split(':')[0]), k))


if __name__ == '__main__':
    main()
