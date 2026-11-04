"""The Batman Beyond Batmobile's body, made from scratch (v1.7.43): a mesh
lofted from cross-sections along the car - a long pointed nose, a flat wide
body with its shoulders swept back into two fins, a bubble canopy, two jet
thrusters at the tail, no wheels (it flies). Nothing is taken from any
model: the shape follows the show's car as described in words and the
user's wish. Written for the vehicle writer (pcwrite.Fragment) and, before
that, drawn here for a look (render()).

Coordinates: the game's (x right, y forward, z up); metres; the origin
under the middle of the car at the floor's height.

usage: python3 -I beyond_body.py <out.png>     a sheet of views
"""
import math
import os
import sys

import numpy as np

# ---- dimensions (m)
LENGTH = 5.6        # nose to tail
HALF_WIDTH = 1.15   # the body's widest half-width (without the fins)
FIN_SPAN = 0.65     # each fin beyond the body
HEIGHT = 1.05       # floor to the canopy's top
FLOOR = 0.42        # the floor over the ground (it hovers)
NOSE_Y = LENGTH * 0.52
TAIL_Y = -LENGTH * 0.48


def smooth(t):
    return t * t * (3 - 2 * t)


def width_at(y):
    """Half-width of the body along its length (without the fins): a point
    at the nose widening on a concave curve, the full width from a third
    back, a little narrower at the tail."""
    v = (NOSE_Y - y) / (NOSE_Y - TAIL_Y)  # 0 nose .. 1 tail
    if v < 0.42:
        return 0.03 + (HALF_WIDTH - 0.03) * math.sin(math.pi / 2 * v / 0.42) ** 1.35
    return HALF_WIDTH * (1 - 0.08 * smooth((v - 0.42) / 0.58))


def canopy_at(y, x):
    """The canopy's bubble: long and narrow, from a quarter back to past
    the middle, its crest a little behind its front."""
    v = (NOSE_Y - y) / (NOSE_Y - TAIL_Y)
    dy = (v - 0.46) / 0.24
    dx = x / 0.46
    r2 = dx * dx + dy * dy * (0.8 if dy > 0 else 1.6)
    return 0.36 * max(0.0, 1 - r2) ** 1.25 if r2 < 1 else 0.0


def top_at(y, s):
    """The top's height at a point across (s in -1..1 of the half-width):
    a low wedge rising from the nose, the canopy over the middle, the
    shoulders dropping to a sharp edge."""
    v = (NOSE_Y - y) / (NOSE_Y - TAIL_Y)
    base = FLOOR + 0.12 + 0.20 * smooth(min(1.0, v / 0.5))   # rises to the shoulders' height by the middle
    base -= 0.04 * smooth(max(0.0, (v - 0.8) / 0.2))          # dips a little at the tail
    w = width_at(y)
    edge = 1 - abs(s) ** 2.5
    return base * (0.78 + 0.22 * edge) + 0.02 + canopy_at(y, s * w)


def bottom_at(y, s):
    v = (NOSE_Y - y) / (NOSE_Y - TAIL_Y)
    return FLOOR + 0.04 * abs(s) + 0.05 * (1 - smooth(min(1.0, v / 0.3)))  # the nose's underside lifts a little


def body_sections(stations=48, across=22):
    """The body's ring of points at each station (nose to tail): the top
    across, then the bottom back - a closed loop."""
    rings = []
    for i in range(stations + 1):
        u = i / stations
        y = NOSE_Y - (NOSE_Y - TAIL_Y) * u
        w = width_at(y)
        ring = []
        for k in range(across + 1):          # the top, left to right
            s = -1 + 2 * k / across
            ring.append((s * w, y, top_at(y, s)))
        for k in range(across, -1, -1):      # the bottom, right to left
            s = -1 + 2 * k / across
            ring.append((s * w, y, bottom_at(y, s)))
        rings.append(ring)
    return rings


def loft(rings, close_ends=True):
    """Quads between consecutive rings (as triangle pairs); the first and
    last rings fanned shut when asked."""
    verts, tris = [], []
    n = len(rings[0])
    for ring in rings:
        verts.extend(ring)
    for i in range(len(rings) - 1):
        a, b = i * n, (i + 1) * n
        for k in range(n):
            k2 = (k + 1) % n
            tris.append((a + k, b + k, b + k2))
            tris.append((a + k, b + k2, a + k2))
    if close_ends:
        for base in (0, (len(rings) - 1) * n):
            c = len(verts)
            verts.append(tuple(np.mean(rings[base // n], axis=0)))
            for k in range(n):
                k2 = (k + 1) % n
                tris.append((c, base + k2, base + k) if base == 0 else (c, base + k, base + k2))
    return np.array(verts, np.float32), np.array(tris, np.int32)


def fin(side):
    """A fin off the rear shoulder: a flat wing swept back, its leading edge
    leaving the body behind the canopy, the tip trailing past the tail; a
    thin airfoil, thicker at the root."""
    rings = []
    root_y0, root_y1 = -0.2, TAIL_Y + 0.05   # where it leaves the body (front, back)
    tip_y0, tip_y1 = -1.9, TAIL_Y - 0.55     # the tip's leading and trailing edge
    for i in range(8):
        u = i / 7
        x = side * (width_at(-1.0) * 0.985 + FIN_SPAN * u)
        y0 = root_y0 + (tip_y0 - root_y0) * smooth(u) ** 0.8
        y1 = root_y1 + (tip_y1 - root_y1) * u
        z = top_at(-1.0, 0.97) - 0.08 - 0.05 * u
        th = 0.075 * (1 - 0.85 * u) + 0.006
        ring = []
        for k in range(11):   # the top surface, front to back
            w_ = k / 10
            y = y0 + (y1 - y0) * w_
            ring.append((x, y, z + th * math.sin(math.pi * w_) ** 0.7))
        for k in range(10, -1, -1):
            w_ = k / 10
            y = y0 + (y1 - y0) * w_
            ring.append((x, y, z - 0.35 * th * math.sin(math.pi * w_)))
        rings.append(ring)
    return loft(rings, close_ends=True)


def thruster(side):
    """A jet at the tail: a tube sunk into the body's rear, a lip, its mouth
    open to the back."""
    rings = []
    cx, cz = side * 0.5, FLOOR + 0.27
    for y, r in ((TAIL_Y + 1.0, 0.24), (TAIL_Y + 0.35, 0.28), (TAIL_Y - 0.05, 0.3), (TAIL_Y - 0.2, 0.29),
                 (TAIL_Y - 0.22, 0.22), (TAIL_Y + 0.05, 0.19)):
        ring = [(cx + r * math.cos(t), y, cz + r * math.sin(t)) for t in np.linspace(0, 2 * math.pi, 21)[:-1]]
        rings.append(ring)
    return loft(rings, close_ends=False)


def canopy_glass():
    """The canopy's glass: the bubble's skin a hair above the body, over its
    footprint only (the glass shader, tinted)."""
    rings = []
    for i in range(17):
        v = 0.24 + 0.44 * i / 16
        y = NOSE_Y - (NOSE_Y - TAIL_Y) * v
        dy = (v - 0.46) / 0.24
        half = 0.46 * math.sqrt(max(0.0, 1 - dy * dy * (0.8 if dy > 0 else 1.6))) * 0.97
        ring = []
        for k in range(13):
            x = -half + 2 * half * k / 12
            w = width_at(y)
            s_ = max(-1.0, min(1.0, x / w))
            ring.append((s_ * w, y, top_at(y, s_) + 0.008))
        rings.append(ring)
    return loft(rings, close_ends=False)


def accents():
    """The red lines: a strip down the nose's spine and along each fin's
    leading edge (an emissive shader)."""
    rings = []
    for i in range(14):
        v = 0.02 + 0.2 * i / 13
        y = NOSE_Y - (NOSE_Y - TAIL_Y) * v
        w = width_at(y)
        hw = min(0.035, w * 0.6)
        rings.append([(-hw, y, top_at(y, -hw / w) + 0.006), (hw, y, top_at(y, hw / w) + 0.006)])
    strips = [loft(rings, close_ends=False)]
    return strips


def parts():
    out = {'body': loft(body_sections()), 'fin_l': fin(-1), 'fin_r': fin(1),
           'jet_l': thruster(-1), 'jet_r': thruster(1), 'glass': canopy_glass()}
    for k, strip in enumerate(accents()):
        out['accent_%d' % k] = strip
    return out


def render(path):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    ps = parts()
    colours = {'body': (0.08, 0.08, 0.1), 'fin_l': (0.1, 0.08, 0.1), 'fin_r': (0.1, 0.08, 0.1),
               'jet_l': (0.25, 0.08, 0.06), 'jet_r': (0.25, 0.08, 0.06), 'glass': (0.45, 0.08, 0.1),
               'accent_0': (0.9, 0.1, 0.05)}
    fig = plt.figure(figsize=(16, 12))
    views = [(22, 40, '3/4 front'), (22, -140, '3/4 rear'), (90, -90, 'top'), (4, 0, 'side')]
    light = np.array([0.4, -0.6, 0.7])
    light /= np.linalg.norm(light)
    for i, (el, az, title) in enumerate(views):
        ax = fig.add_subplot(2, 2, i + 1, projection='3d')
        for name, (v, t) in ps.items():
            tri = v[t]
            nrm = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
            shade = 0.35 + 0.65 * np.clip(nrm @ light, 0, 1)
            base = np.array(colours[name])
            fc = np.clip(base[None, :] * (0.6 + 1.6 * shade[:, None]) + 0.25 * shade[:, None] ** 8, 0, 1)
            coll = Poly3DCollection(tri, facecolors=fc, edgecolors='none')
            ax.add_collection3d(coll)
        ax.set_xlim(-3, 3); ax.set_ylim(-3, 3); ax.set_zlim(-2.5, 3.5)
        ax.set_box_aspect((1, 1, 1))
        ax.view_init(elev=el, azim=az)
        ax.set_title(title)
        ax.set_axis_off()
    fig.patch.set_facecolor((0.75, 0.78, 0.82))
    plt.tight_layout()
    plt.savefig(path, dpi=70)
    n = sum(len(t) for _, t in ps.values())
    print('%d triangles in %d parts -> %s' % (n, len(ps), path))


if __name__ == '__main__':
    render(sys.argv[1] if len(sys.argv) > 1 else 'beyond_body.png')
