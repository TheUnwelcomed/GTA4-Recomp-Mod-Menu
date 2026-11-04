"""What a console bound dictionary (.xbd) holds, entry by entry (v1.7.42).

Reads a dictionary as the game does (bounds360's layouts): each key (a
model-name hash, named when a placement list or name list is given), its
bound's type (10 a BVH mesh, 12 a composite of them), box, radius, the
mesh's vertex, polygon and material counts, its tree's node count and depth,
the vertex quantum - the things that tell one entry from another when the
game collides with some of a set's cells and not with others (Wake Island's
runway cell, v1.7.41).

usage: python3 -I xbdinfo.py <x.xbd | x.bin> [...] [--names=place.txt,objs.ide,...] [--key=wk_c2]
  a .xbd is decompressed with xlzx (the XLZX environment variable, or the
  file next to this script's ../../conv), a .bin is a decompressed body with
  its <name>.log from xlzx unrsc beside it (flags=...)
  --names: files whose first word per line is a model name (hashes named)
  --key: only this entry (a name or hex hash)
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import bounds360  # noqa: E402
import rsc5  # noqa: E402


def load(path):
    if path.endswith('.bin'):
        log = open(path[:-4] + '.log').read()
        flags = int(re.search(r'flags=([0-9A-Fa-f]+)', log).group(1), 16)
        body = open(path, 'rb').read()
    else:
        xlzx = os.environ.get('XLZX') or os.path.join(HERE, '..', '..', 'conv', 'xlzx')
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, 'body.bin')
            log = subprocess.run([xlzx, 'unrsc', path, out], capture_output=True, text=True).stdout
            flags = int(re.search(r'flags=([0-9A-Fa-f]+)', log).group(1), 16)
            body = open(out, 'rb').read()
    return rsc5.Resource(body, flags, True, 32, os.path.basename(path))


def tree_depth(r, tr):
    """The deepest leaf of a BVH tree: nodes 16 bytes (u16 x 7, u8 count),
    the escape index at +12, leaves those with a count."""
    nodes, n = r.vptr(tr), r.u32(tr + 4)
    depth = 0
    stack = [(0, 1)]
    seen = 0
    while stack and seen < n:
        i, d = stack.pop()
        if i >= n:
            continue
        seen += 1
        depth = max(depth, d)
        count = r.u8(nodes + 16 * i + 14)
        esc = r.u16(nodes + 16 * i + 12)
        if count:
            stack.append((esc if esc > i else i + 1, d))
        else:
            stack.append((i + 1, d + 1))
            if esc > i + 1:
                stack.append((esc, d))
    return n, depth


def describe(r, off, indent=''):
    t = r.u8(off + 4)
    mx = struct.unpack_from('>3f', r.virt, off + 0x10)
    mn = struct.unpack_from('>3f', r.virt, off + 0x20)
    cen = struct.unpack_from('>3f', r.virt, off + 0x30)
    radius, far = struct.unpack_from('>2f', r.virt, off + 8)
    margin = struct.unpack_from('>3f', r.virt, off + 0x70)
    lines = ['%stype %d  box (%.1f %.1f %.1f)..(%.1f %.1f %.1f)  centroid (%.1f %.1f %.1f)  radius %.1f  farthest %.1f'
             '  margin %.3f' % ((indent, t) + mn + mx + cen + (radius, far, margin[0]))]
    if t == 12:
        k = r.u16(off + 0x90)
        kids = r.vptr(off + 0x80)
        lines[-1] += '  children %d' % k
        for j in range(k):
            lines += describe(r, r.vptr(kids + 4 * j), indent + '    ')
    elif t in (4, 10):
        nv, npo = r.u32(off + 0xC8), r.u32(off + 0xCC)
        q = struct.unpack_from('>3f', r.virt, off + 0x90)
        o = struct.unpack_from('>3f', r.virt, off + 0xA0)
        mats = r.u8(off + 0xD8)
        mp = r.vptr(off + 0xD0)
        mat_ids = [r.u32(mp + 4 * i) for i in range(mats)] if mp else []
        pp = r.vptr(off + 0x8C)
        quads = sum(1 for i in range(npo) if r.u16(pp + 32 * i + 22) != 0)
        s = '%s    %d vertices, %d polygons (%d quads), %d materials [%s], quantum (%.4f %.4f %.4f) offset (%.1f %.1f %.1f)' % (
            (indent, nv, npo, quads, mats, ' '.join('%08X' % m for m in mat_ids)) + q + o)
        if t == 10:
            n, depth = tree_depth(r, r.vptr(off + 0xE0))
            tr = r.vptr(off + 0xE0)
            sub = r.u16(tr + 0x54)
            s += '; tree %d nodes, depth %d, %d subtrees' % (n, depth, sub)
        lines.append(s)
    return lines


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=', 1)[0]: a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--') and '=' in a}
    names = {}
    for f in opts.get('--names', '').split(','):
        if f:
            for line in open(f):
                w = line.split()
                if w and not line.startswith('#'):
                    names[bounds360.joaat(w[0].lower())] = w[0].lower()
    only = opts.get('--key')
    if only:
        only = int(only, 16) if re.fullmatch(r'[0-9A-Fa-f]{8}', only) else bounds360.joaat(only.lower())
    for path in args:
        r = load(path)
        n = r.u16(0x14)
        keys = [r.u32(r.vptr(0x10) + 4 * i) for i in range(n)]
        print('%s: %d entries, flags %08X, %d bytes%s' % (path, n, r.flags, len(r.virt),
                                                         '' if keys == sorted(keys) else ' (KEYS NOT SORTED)'))
        for i, k in enumerate(keys):
            if only is not None and k != only:
                continue
            off = r.vptr(r.vptr(0x18) + 4 * i)
            print('  %08X %-22s at %06X' % (k, names.get(k, ''), off))
            for line in describe(r, off, '      '):
                print(line)


if __name__ == '__main__':
    main()
