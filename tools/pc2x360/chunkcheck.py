"""Does every GPU buffer of a 360 resource lie inside one load chunk?

The console loads a resource's physical segment as separate chunks (as many
16-page chunks as fit, then the rest in power-of-two chunks, largest first;
see batconv.load_chunks), so a vertex buffer, index buffer or texture level
that runs past its chunk's end is read from whatever memory follows that
chunk. On the PS5 port that showed as huge stretched polygons (v1.7.5).

usage: chunkcheck.py <file.xft|file.xtd> <decoded body .bin> [...pairs]
"""
import struct
import sys

import batconv
import rsc5
import xenos

# Xenos texture formats (GPUTEXTUREFORMAT) -> (texels per block side, bytes per block)
BLOCKS = {0x12: (4, 8), 0x13: (4, 16), 0x14: (4, 16), 0x31: (4, 16),            # DXT1, DXT3, DXT5, DXN
          0x3A: (4, 8), 0x3B: (4, 8), 0x3C: (4, 8),                             # DXT3A, DXT5A, CTX1
          0x02: (1, 1), 0x03: (1, 2), 0x04: (1, 2), 0x06: (1, 4), 0x07: (1, 4),  # 8, 1555, 565, 8888, 2_10_10_10
          0x0A: (1, 2), 0x0F: (1, 2)}                                           # 8_8, 4444


def buffers_xft(r):
    """(label, physical offset, bytes) of every vertex and index buffer."""
    f = rsc5.read_fragtype(r)
    draws = [('main', f['drawable'])] + [('child%d' % i, c['drawable']) for i, c in enumerate(f['children'])
                                          if c['drawable'] and c['drawable']['lods']]
    out = {}
    for label, d in draws:
        for li, lod in enumerate(d['lods']):
            for m in lod:
                for gi, g in enumerate(m['geometries']):
                    vb, ib = r.vptr(g['off'] + 12), r.vptr(g['off'] + 28)
                    vo = r.u32(vb + 8) & 0x0FFFFFFF
                    vn = ((r.u32(vb + 0x3C) >> 2) & 0xFFFFFF) * 4
                    io = r.u32(ib + 8) & 0x0FFFFFFF
                    out.setdefault(('vertex buffer', vo, vn), '%s lod%d g%d' % (label, li, gi))
                    out.setdefault(('index buffer', io, r.u32(ib + 0x2C)), '%s lod%d g%d' % (label, li, gi))
    return [(k + ' (' + who + ')', o, n) for (k, o, n), who in out.items()]


def level_bytes(fmt, w, h):
    side, bpb = BLOCKS[fmt]
    bw, bh = max(1, -(-w // side)), max(1, -(-h // side))
    return xenos.align(bw, 32) * xenos.align(bh, 32) * bpb


def buffers_xtd(t):
    """(label, physical offset, bytes) of every texture's base level and,
    where it has one, its mip chain: levels at power-of-two sizes, each padded
    to whole 32x32-block tiles; with packed mips, every level from the first
    one 16 texels or less on a side shares that level's tiles (Xenos)."""
    out = []
    tl, n = t.vptr(24), t.u16(20)
    for i in range(n):
        info = t.vptr(tl + 4 * i)
        name = t.cstr(t.vptr(info + 20))
        d3d = t.vptr(info + 24)
        f = struct.unpack_from('>6I', t.virt, d3d + 28)
        fmt = f[1] & 0x3F
        w, h = (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1
        base = (f[1] & 0xFFFFF000) & 0x0FFFFFFF
        if fmt not in BLOCKS:
            raise SystemExit('%s: unknown texture format %02X' % (name, fmt))
        out.append(('texture %s base' % name, base, level_bytes(fmt, w, h)))
        mip = (f[5] & 0xFFFFF000) & 0x0FFFFFFF
        top = (f[4] >> 6) & 0xF  # max mip level
        packed = (f[5] >> 11) & 1
        if mip and top:
            size = 0
            lw, lh = 1 << (w - 1).bit_length(), 1 << (h - 1).bit_length()
            for _ in range(top):
                lw, lh = max(1, lw // 2), max(1, lh // 2)
                size += level_bytes(fmt, lw, lh)
                if packed and min(lw, lh) <= 16:
                    break
            out.append(('texture %s mips' % name, mip, size))
    # Packed mip tails take less room than whole tiles (stock: a 128x128
    # DXT1's two mip levels in 4 KiB), and no two objects overlap, so a mip
    # chain ends no later than the next object starts.
    starts = sorted({o for _, o, _ in out} | {t.psize})
    return [(k, o, min(n, next(s for s in starts if s > o) - o) if k.endswith(' mips') else n)
            for k, o, n in out]


def crossings(res, objs):
    chunks = batconv.segment_chunks(res.flags)
    bad = []
    for label, o, n in objs:
        if n and not any(cs <= o and o + n <= ce for cs, ce in chunks):
            bad.append('%s at %X+%d' % (label, o, n))
    return chunks, bad


def check(path, body):
    r = rsc5.Resource.x360(path, body)
    objs = buffers_xtd(r) if path.lower().endswith('.xtd') else buffers_xft(r)
    chunks, bad = crossings(r, objs)
    return r, objs, chunks, bad


if __name__ == '__main__':
    args = sys.argv[1:]
    total = 0
    for path, body in zip(args[::2], args[1::2]):
        r, objs, chunks, bad = check(path, body)
        total += len(bad)
        print('%-40s %3d objects, %d chunks (%s KiB): %s' % (
            path.split('/')[-1], len(objs), len(chunks), '+'.join(str((e - s) >> 10) for s, e in chunks),
            'ok' if not bad else '%d CROSS: %s' % (len(bad), '; '.join(bad[:4]))))
    sys.exit(1 if total else 0)
