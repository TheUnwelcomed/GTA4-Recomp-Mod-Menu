"""The Beyond suit's throwing blade (v1.7.47), a prop made here: a flat
two-winged throwing blade of my own shape - a swept crescent with a
scalloped trailing edge and a grip in the middle, 26 cm across, 4 mm
thick, gunmetal with a brighter bevel along its edges (an 8x8 shade
ramp). Written as a PC piece (.wdr, .wtd, its objs.ide line) for
propset.py; the trainer throws it (its model spun in flight).

usage: python3 -I blade.py <out_dir> --samples=<dir of IV .wdr>[,...]
"""
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import maptex  # noqa: E402
import pcwrite  # noqa: E402

NAME = 'bk_blade'
SPAN, CHORD, THICK = 0.26, 0.075, 0.004


def outline():
    """The blade's outline, counter-clockwise seen from above (x right, y
    forward): the leading edge a swept arc, the trailing edge scalloped
    into three bites, the tips sharp."""
    pts = []
    n = 24
    # leading edge: from the left tip over the front to the right tip (an arc bowed forward)
    for i in range(n + 1):
        t = -1.0 + 2.0 * i / n
        x = t * SPAN / 2
        y = CHORD * 0.35 * (1.0 - t * t) ** 0.6 + CHORD * 0.12
        pts.append((x, y))
    # trailing edge: back from the right tip, three scallops, to the left tip
    for i in range(n, -1, -1):
        t = -1.0 + 2.0 * i / n
        x = t * SPAN / 2
        base = -CHORD * 0.30 * (1.0 - t * t) ** 0.6 + CHORD * 0.12
        bite = -CHORD * 0.28 * max(0.0, math.sin(math.pi * (t + 1.0) * 1.5)) ** 1.4 * (1.0 - abs(t)) ** 0.3
        pts.append((x, base + bite))
    # the grip: a small round boss in the middle (on the outline, the top is flat)
    return pts


def mesh():
    o = outline()
    n = len(o)
    top = [(x, y, THICK / 2) for x, y in o]
    bot = [(x, y, -THICK / 2) for x, y in o]
    cx = sum(x for x, _ in o) / n
    cy = sum(y for _, y in o) / n
    pos, nrm, uv, tris = [], [], [], []

    def add(p, nv, t):
        pos.append(p)
        nrm.append(nv)
        uv.append(t)
        return len(pos) - 1
    # the faces: fans about the centre (the outline is star-shaped about it)
    ct = add((cx, cy, THICK / 2), (0, 0, 1), (0.5, 0.5))
    cb = add((cx, cy, -THICK / 2), (0, 0, -1), (0.5, 0.5))
    ti = [add(p, (0, 0, 1), (0.5 + (p[0] - cx) / SPAN, 0.5 - (p[1] - cy) / SPAN)) for p in top]
    bi = [add(p, (0, 0, -1), (0.5 + (p[0] - cx) / SPAN, 0.5 - (p[1] - cy) / SPAN)) for p in bot]
    for i in range(n):
        j = (i + 1) % n
        tris.append((ct, ti[i], ti[j]))
        tris.append((cb, bi[j], bi[i]))
    # the edge: a strip round the outline with the bevel's bright line (u along the ramp)
    for i in range(n):
        j = (i + 1) % n
        ex, ey = o[j][0] - o[i][0], o[j][1] - o[i][1]
        ln = math.hypot(ex, ey) or 1e-6
        nv = (ey / ln, -ex / ln, 0.0)
        a = add(top[i], nv, (0.95, 0.1))
        b = add(top[j], nv, (0.95, 0.1))
        c = add(bot[j], nv, (0.95, 0.9))
        d = add(bot[i], nv, (0.95, 0.9))
        tris.append((a, b, c))
        tris.append((a, c, d))
    return np.array(pos, np.float32), np.array(nrm, np.float32), np.array(uv, np.float32), np.array(tris, np.int64)


def texture():
    """Gunmetal with a bright bevel ramp down the right edge."""
    img = np.zeros((32, 32, 4), np.uint8)
    img[..., :3] = (58, 60, 66)
    img[..., 3] = 255
    for x in range(26, 32):
        k = (x - 26) / 5.0
        img[:, x, :3] = (int(58 + 120 * k), int(60 + 122 * k), int(66 + 125 * k))
    return img


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    out = args[0]
    samples = [p for p in opts.get('samples', '').split(',') if p]
    os.makedirs(out, exist_ok=True)
    lib = pcwrite.ShaderLibrary(pcwrite.glob_wdr(samples))
    d = pcwrite.Drawable(lib)
    s = d.shader('gta_default.sps', {batconv.SAMPLER_TEXTURE: NAME + '_d'})
    pos, nrm, uv, tris = mesh()
    col = np.full((len(pos), 4), 255, np.uint8)
    d.geometry(0, s, pos, nrm, col, [uv], tris)
    info = d.write(os.path.join(out, NAME + '.wdr'))
    fmt, levels = maptex.encode_levels(texture(), 32)
    pcwrite.Dictionary({NAME + '_d': (levels[0][0], levels[0][1], fmt, [lv[2] for lv in levels])}).write(
        os.path.join(out, NAME + '.wtd'))
    lo, hi = info['bbox']
    open(os.path.join(out, NAME + '.ide'), 'w').write(
        '%s, %s, 300, %d, 0, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 0.0, 0.0, 0.0, %.4f, null\n' % (
            NAME, NAME, 0xC00000, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], info['radius']))
    print('%s: %d vertices, %d triangles, %s .. %s' % (NAME, len(pos), len(tris), [round(x, 3) for x in lo],
                                                      [round(x, 3) for x in hi]))


if __name__ == '__main__':
    main()
