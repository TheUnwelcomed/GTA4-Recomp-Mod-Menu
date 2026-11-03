"""Check the mip-chain layout (pc2x360/xenos.py: mip_layout, packed_offset)
against the game's own textures: every level of every stock texture with
mips is read out where the layout says it is and decoded; each must look like
its parent level scaled down (a wrong offset or pitch decodes to noise).

usage: python3 -I mipcheck.py <decompressed stock .xtd.bin:flags> [...]
  (xlzx unrsc gives the body and, in its log, the flags)
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
import dxt  # noqa: E402
import rsc5  # noqa: E402
import xenos  # noqa: E402

BPP = {18: 8, 19: 16, 20: 16}  # DXT1, DXT3 (decoded as 5's colour), DXT5


def decode(fmt, linear, w, h):
    pc = xenos.swap16(linear)
    if fmt == 18:
        return dxt.decode_dxt1(pc, w, h)
    return dxt.decode_dxt5(pc, w, h)


def textures(r):
    tl, n = r.vptr(24), r.u16(20)
    for i in range(n):
        info = r.vptr(tl + 4 * i)
        d3d = r.vptr(info + 24)
        f = struct.unpack_from('>6I', r.virt, d3d + 28)
        yield dict(name=r.cstr(r.vptr(info + 20)), w=(f[2] & 0x1FFF) + 1, h=((f[2] >> 13) & 0x1FFF) + 1,
                   fmt=f[1] & 0x3F, base=(f[1] & 0x0FFFF000), mip=(f[5] & 0x0FFFF000),
                   packed=(f[5] >> 11) & 1, levels=((f[4] >> 6) & 0xF) + 1, pitch=((f[0] >> 22) & 0x1FF) * 32)


def level_error(child, parent):
    """Mean absolute difference (0-255) between a level and its parent scaled
    down 2x2 (colour only, where both are opaque enough to mean anything)."""
    ph, pw = parent.shape[:2]
    p = parent[:ph // 2 * 2, :pw // 2 * 2].astype(np.float64)
    p = p.reshape(ph // 2, 2, pw // 2, 2, 4).mean((1, 3))
    h, w = min(child.shape[0], p.shape[0]), min(child.shape[1], p.shape[1])
    c = child[:h, :w].astype(np.float64)
    p = p[:h, :w]
    # (a cutout's colour under its transparent texels means nothing, and
    # differs level to level: Wake Island's palm leaves)
    solid = (c[:, :, 3] > 64) & (p[:, :, 3] > 64)
    if solid.sum() < 0.02 * solid.size:
        return 0.0
    return float(np.abs(c[:, :, :3] - p[:, :, :3])[solid].mean())


def check(r, log=print):
    worst = 0.0
    checked = 0
    for t in textures(r):
        if t['levels'] < 2 or t['fmt'] not in BPP:
            continue
        bpp = BPP[t['fmt']]
        w, h = t['w'], t['h']
        bw, bh = (w + 3) // 4, (h + 3) // 4
        base = r.phys[t['base']:]
        lv0 = decode(t['fmt'], xenos.untile_from(base, bw, bh, bpp, xenos.align(bw, 32)), w, h)
        layout, size = xenos.mip_layout(w, h, t['levels'], bpp)
        parent = lv0
        errors = []
        for L in layout:
            region = r.phys[t['mip'] + L['offset']:]
            lin = xenos.untile_from(region, L['bw'], L['bh'], bpp, L['pitch'], L['x'], L['y'])
            tw, th = xenos.level_size(w, h, L['level'])
            img = decode(t['fmt'], lin, max(tw, 4), max(th, 4))[:th, :tw]
            if parent.shape[0] >= 8 and parent.shape[1] >= 8:  # (the last few are too small to judge)
                errors.append(level_error(img, parent))
            parent = img
        e = max(errors) if errors else 0.0
        worst = max(worst, e)
        checked += 1
        log('%-36s %4dx%-4d fmt %d levels %d packed %d mip region %6d: worst level error %.1f (%s)' % (
            t['name'], w, h, t['fmt'], t['levels'], t['packed'], size, e,
            ', '.join('%.1f' % x for x in errors)))
    return checked, worst


def main():
    worst = 0.0
    for arg in sys.argv[1:]:
        path, flags = arg.rsplit(':', 1)
        r = rsc5.Resource(open(path, 'rb').read(), int(flags, 16), True, 7, path)
        n, w = check(r)
        worst = max(worst, w)
        print('%s: %d textures with mips, worst %.1f' % (os.path.basename(path), n, w))
    print('PASS' if worst < 12.0 else 'FAIL', 'worst level error %.1f (noise is ~40+)' % worst)
    sys.exit(0 if worst < 12.0 else 1)


if __name__ == '__main__':
    main()
