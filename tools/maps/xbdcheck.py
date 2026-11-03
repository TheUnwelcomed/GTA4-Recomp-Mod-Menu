"""A console bound dictionary read back as the game reads it, every BVH mesh
checked: its tree's layout (depth first, escape counts, leaves holding the
polygons in order, boxes holding their polygons, inner boxes the union of
their children, subtree headers by Bullet's rule), its quads flat-convex in
order round them, and vertical rays walked through the tree stacklessly (and
through its subtree headers) against every polygon tried in turn
(collide.check_bvh); and every object inside one load chunk (the console
loads a segment in chunks, pointers translated chunk by chunk, so an array
over a chunk's edge is read from the memory after the chunk: v1.7.34's
island froze the game on arrival in a node walk through such a tree).

usage: python3 -I xbdcheck.py <body.bin> <flags>
  body.bin   the dictionary's body, unpacked (pc2x360/xlzx unrsc in out)
  flags      its resource flags (xlzx prints them), hex
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import bounds360  # noqa: E402
import collide  # noqa: E402
import rsc5  # noqa: E402


def read_mesh(r, off):
    """The BVH at `off` (big-endian) as collide's dict (little-endian bytes)."""
    def be(o, n, fmt='f'):
        return struct.unpack_from('>%d%s' % (n, fmt), r.virt, o)
    nv, npo = be(off + 0xC8, 2, 'I')
    pp, vp = r.vptr(off + 0x8C), r.vptr(off + 0xB0)
    polys = bytearray()
    for i in range(npo):
        polys += struct.pack('<4f8H', *struct.unpack_from('>4f8H', r.virt, pp + 32 * i))
    verts = struct.pack('<%dh' % (3 * nv), *struct.unpack_from('>%dh' % (3 * nv), r.virt, vp))
    t = r.vptr(off + 0xE0)
    nn, nodes_p = be(t + 4, 1, 'I')[0], r.vptr(t)
    nodes = bytearray()
    for i in range(nn):
        nodes += struct.pack('<7H', *struct.unpack_from('>7H', r.virt, nodes_p + 16 * i))
        nodes += bytes(r.virt[nodes_p + 16 * i + 14:nodes_p + 16 * i + 16])
    ns, sp = struct.unpack_from('>H', r.virt, t + 0x54)[0], r.vptr(t + 0x50)
    subs = struct.pack('<%dH' % (8 * ns), *struct.unpack_from('>%dH' % (8 * ns), r.virt, sp))
    return dict(type=10, polygons=bytes(polys), vertices=verts, quantum=list(be(off + 0x90, 3)),
                offset=list(be(off + 0xA0, 3)),
                tree=dict(nodes=bytes(nodes), subtrees=subs, min=list(be(t + 0x10, 3)), max=list(be(t + 0x20, 3)),
                          quant=list(be(t + 0x30, 3)), inv=list(be(t + 0x40, 3)), count=nn, cap=nn))


def main():
    body, flags = open(sys.argv[1], 'rb').read(), int(sys.argv[2], 16)
    r = rsc5.Resource(body, flags, True, 32, sys.argv[1])
    n = struct.unpack_from('>H', r.virt, 0x14)[0]
    keys = [struct.unpack_from('>I', r.virt, r.vptr(0x10) + 4 * i)[0] for i in range(n)]
    assert keys == sorted(keys) and len(set(keys)) == n, 'keys unsorted or repeated'
    stats = dict(meshes=0, polygons=0, quads=0, worst=0.0)

    def walk(off):
        kind = r.virt[off + 4]
        if kind == 12:
            k = struct.unpack_from('>H', r.virt, off + 0x90)[0]
            kids = r.vptr(off + 0x80)
            for i in range(k):
                walk(r.vptr(kids + 4 * i))
            return
        assert kind == 10, kind
        mesh = read_mesh(r, off)
        stats['worst'] = max(stats['worst'], collide.check_bvh(mesh, rays=40))
        idx = np.frombuffer(mesh['polygons'], np.uint8).reshape(-1, 32)[:, 16:24].copy().view('<u2')
        stats['meshes'] += 1
        stats['polygons'] += len(idx)
        stats['quads'] += int((idx[:, 3] != 0).sum())

    for i in range(n):
        walk(r.vptr(r.vptr(0x18) + 4 * i))
    cut = bounds360.chunk_crossings(body, flags)
    for label, off, size in cut[:8]:
        print('CUT BY A LOAD CHUNK EDGE: %s at %X, %d bytes' % (label, off, size))
    print('%d bounds, %d meshes, %d polygons (%d quads); trees walked against every polygon: worst %.4f m; %d objects '
          'in %d load chunks, %d cut by a chunk edge' % (
              n, stats['meshes'], stats['polygons'], stats['quads'], stats['worst'], len(bounds360.objects(r)),
              len(batconv.segment_chunks(flags, physical=False)), len(cut)))
    assert not cut


if __name__ == '__main__':
    main()
