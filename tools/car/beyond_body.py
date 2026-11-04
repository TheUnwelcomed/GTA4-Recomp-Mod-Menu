"""The Batman Beyond Batmobile's body, made from scratch (v1.7.43): a dart
seen from above - a long pointed nose, the body widening on a concave curve
into two swept wings whose trailing edge cuts forward to the tail in a V,
a long bubble canopy over the spine (the pilot's head under its crown), a
cockpit tub under the fuselage, two jet thrusters sunk into the tail between
the wings, no wheels (it flies). Nothing is taken from any model: the shape
follows the show's car as described in words and the user's wish. Written
for the vehicle writer (pcwrite.Fragment) and, before that, drawn here for a
look (render()).

Coordinates: the game's (x right, y forward, z up); metres; the origin at
the template car's (the Infernus's: its wheel hubs' height, 0.52 m over the
road), the body's underside there, so resting on the template's hidden
wheels the body floats half a metre over the road (v1.7.44; v1.7.43 put the
floor 0.4 m higher still and the seats where no pilot fitted under the
canopy).

usage: python3 -I beyond_body.py <out.png>     a sheet of views
"""
import math
import sys

import numpy as np

# ---- dimensions (m)
LENGTH = 5.8        # nose to the wing tips
SPAN = 1.55         # half-span at the wing tips
HEIGHT = 0.95       # floor to the canopy's top
FLOOR = 0.0         # the body's underside, at the origin (the template's hubs)
NOSE_Y = LENGTH * 0.55
TAIL_Y = -LENGTH * 0.45          # the wing tips' y
NOTCH_Y = TAIL_Y + 1.15          # the trailing edge's V at the centre line
CANOPY = (0.28, 0.74)            # the canopy from .. to (fraction of the length from the nose)
CANOPY_HEIGHT = 0.44             # the bubble's crown over the spine
POD = (-1.4, 1.9, 0.40, 0.38)    # the cockpit tub under the fuselage: y from, to, half-width, depth
SEATS = ((0.0, 0.35, -0.20), (0.0, -0.35, -0.22))  # the pilot's and a passenger's (car space)
WELL_FLOOR = 0.05                # the cockpit well's floor over the body's underside


def hinge():
    """Where the canopy turns: the bubble's rear edge on the spine."""
    y = NOSE_Y - (NOSE_Y - TAIL_Y) * CANOPY[1]
    return (0.0, y, top_z(y, 0.0, well=False))


def smooth(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3 - 2 * t)


def frac(y):
    """0 at the nose .. 1 at the wing tips."""
    return (NOSE_Y - y) / (NOSE_Y - TAIL_Y)


def outer_x(y):
    """The leading edge's half-width: a needle nose opening on a concave
    curve, then the wings sweeping out to the tips."""
    v = frac(y)
    if v <= 0:
        return 0.0
    if v < 0.6:
        return 0.03 + 0.69 * math.sin(math.pi / 2 * v / 0.6) ** 0.9
    # the wing: from 0.72 at 60% back out to the span at the tips, convex
    u = (v - 0.6) / 0.4
    return 0.72 + (SPAN - 0.72) * (1 - (1 - u) ** 1.7)


def inner_x(y):
    """The trailing edge's half-width: zero up to the notch, then a V from
    the centre line back to the tips (the bat wing's cut)."""
    if y >= NOTCH_Y:
        return 0.0
    u = (NOTCH_Y - y) / (NOTCH_Y - TAIL_Y)  # 0 at the notch's point .. 1 at the tips
    return max(0.0, SPAN * u ** 0.9 - 0.02)


def canopy_z(y, x):
    """The bubble over the spine: long and narrow, highest a third of the
    way along it."""
    v = frac(y)
    a, b = CANOPY
    if not (a <= v <= b):
        return 0.0
    t = (v - a) / (b - a)
    along = math.sin(math.pi * t) ** 0.55 * (1 - 0.18 * t)
    half = canopy_half(t)
    s = min(1.0, abs(x) / half)
    across = math.sqrt(max(0.0, 1 - s * s))
    return CANOPY_HEIGHT * along * across


def canopy_half(t):
    """The bubble's half-width along it (room for the pilot's shoulders)."""
    return 0.27 + 0.07 * math.sin(math.pi * t)


def in_well(y, x):
    """Under the canopy: the cockpit well cut into the top skin (the pilot
    sits in it; open, the canopy shows it)."""
    v = frac(y)
    a, b = CANOPY
    if not (a <= v <= b):
        return False
    return abs(x) < canopy_half((v - a) / (b - a)) * 0.88


def top_z(y, x, well=True):
    """The top skin's height: a thin wedge rising from the nose to a spine
    over the middle, the wings thinner out to a sharp edge, lifting a
    little toward the tips; the cockpit well cut in under the canopy (the
    glass itself is glass_z)."""
    if well and in_well(y, x):
        return FLOOR + WELL_FLOOR
    v = frac(y)
    w = outer_x(y)
    s = min(1.0, abs(x) / w) if w > 1e-6 else 1.0
    spine = FLOOR + 0.10 + 0.24 * smooth(v / 0.45) - 0.08 * smooth((v - 0.75) / 0.25)
    edge = FLOOR + 0.07 + 0.06 * smooth(v / 0.45)
    z = spine + (edge - spine) * s ** 1.6
    tip_lift = 0.10 * smooth((v - 0.6) / 0.4) * s ** 2  # the wing tips cant up
    return z + tip_lift


def glass_z(y, x):
    """The canopy's bubble over the spine."""
    return top_z(y, x, well=False) + canopy_z(y, x)


def bottom_z(y, x):
    v = frac(y)
    w = outer_x(y)
    s = min(1.0, abs(x) / w) if w > 1e-6 else 1.0
    belly = FLOOR + 0.02 * (1 - smooth(v / 0.3))     # the nose's underside lifts
    edge = FLOOR + 0.05 + 0.06 * smooth(v / 0.45)
    tip_lift = 0.10 * smooth((v - 0.6) / 0.4) * s ** 2
    return belly + (edge - belly) * s ** 1.6 + tip_lift


# ---- the surfaces
def grid(fn, nu, nv):
    """Points fn(u, v) on a grid, as triangles (two per cell), u and v in 0..1."""
    pts = np.array([[fn(i / nu, j / nv) for j in range(nv + 1)] for i in range(nu + 1)], np.float32)
    verts = pts.reshape(-1, 3)
    tris = []
    for i in range(nu):
        for j in range(nv):
            a = i * (nv + 1) + j
            b = a + nv + 1
            tris.append((a, b, b + 1))
            tris.append((a, b + 1, a + 1))
    return verts, np.array(tris, np.int32)


def half(side, top, nu=60, nv=14):
    """One side's top or bottom skin: along the length (u) from the nose to
    the tips, across (v) from the inner edge (the spine, or the trailing
    edge's V) to the leading edge."""
    def fn(u, v):
        y = NOSE_Y - (NOSE_Y - TAIL_Y) * u ** 0.85
        x = inner_x(y) + (outer_x(y) - inner_x(y)) * v
        z = top_z(y, x) if top else bottom_z(y, x)
        return (side * x, y, z)
    verts, tris = grid(fn, nu, nv)
    if (side < 0) == top:  # keep the faces outward
        tris = tris[:, ::-1]
    return verts, tris


def notch_wall(side, nu=24, nz=3):
    """The trailing edge's face between the top and bottom skins along the
    V (a thin vertical wall; the leading edge is sharp and needs none)."""
    def fn(u, v):
        y = NOTCH_Y - (NOTCH_Y - TAIL_Y) * u
        x = inner_x(y)
        return (side * x, y, bottom_z(y, x) + (top_z(y, x) - bottom_z(y, x)) * v)
    verts, tris = grid(fn, nu, nz)
    if side > 0:
        tris = tris[:, ::-1]
    return verts, tris


def thruster(side):
    """A jet sunk into the tail beside the spine: a tube, its mouth open to
    the back, a lip."""
    rings = []
    cx, cz = side * 0.40, FLOOR + 0.17
    for y, r in ((NOTCH_Y + 0.9, 0.12), (NOTCH_Y + 0.3, 0.145), (NOTCH_Y - 0.1, 0.15), (NOTCH_Y - 0.26, 0.145),
                 (NOTCH_Y - 0.28, 0.11), (NOTCH_Y - 0.02, 0.09)):
        rings.append([(cx + r * math.cos(t), y, cz + r * 0.85 * math.sin(t)) for t in np.linspace(0, 2 * math.pi, 21)[:-1]])
    return loft(rings)


def loft(rings):
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
    return np.array(verts, np.float32), np.array(tris, np.int32)


def canopy_glass(nu=26, nv=12):
    """The bubble's skin a hair over the body, over its footprint only."""
    a, b = CANOPY

    def fn(u, v):
        t = a + (b - a) * u
        y = NOSE_Y - (NOSE_Y - TAIL_Y) * t
        half_w = canopy_half(u) * 0.98
        x = -half_w + 2 * half_w * v
        return (x, y, glass_z(y, x) + 0.006)
    return grid(fn, nu, nv)


def pod(nu=30, nv=14):
    """The cockpit tub under the fuselage: a half-ellipse section hung from
    the bottom skin, deepest under the seat, tapering to nothing at its
    ends (the pilot's legs are in it, not in the air under the wing)."""
    y0, y1, half_w, depth = POD

    def fn(u, v):
        y = y0 + (y1 - y0) * u
        shape = math.sin(math.pi * u)
        w = half_w * shape ** 0.25 + 1e-3
        d = depth * shape ** 0.45
        x = -w + 2 * w * v
        s = min(1.0, abs(x) / w)
        return (x, y, bottom_z(y, x) - d * math.sqrt(max(0.0, 1 - s * s)) ** 0.8)
    verts, tris = grid(fn, nu, nv)
    return verts, tris[:, ::-1]


def accent(side, nu=40):
    """A red line along the leading edge, just inboard of it."""
    def fn(u, v):
        y = NOSE_Y - (NOSE_Y - TAIL_Y) * (0.03 + 0.97 * u)
        w = outer_x(y)
        x = w * (0.90 + 0.06 * v)
        return (side * x, y, top_z(y, x) + 0.004)
    verts, tris = grid(fn, nu, 1)
    if side < 0:
        tris = tris[:, ::-1]
    return verts, tris


def parts():
    out = {}
    for side, name in ((1, 'r'), (-1, 'l')):
        out['top_' + name] = half(side, True)
        out['bottom_' + name] = half(side, False)
        out['notch_' + name] = notch_wall(side)
        out['jet_' + name] = thruster(side)
        out['accent_' + name] = accent(side)
    out['glass'] = canopy_glass()
    out['pod'] = pod()
    return out


def render(path):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    ps = parts()
    body = (0.09, 0.09, 0.11)
    colours = {k: body for k in ps}
    colours.update({'jet_l': (0.22, 0.08, 0.06), 'jet_r': (0.22, 0.08, 0.06), 'glass': (0.42, 0.08, 0.10), 'pod': body,
                    'accent_l': (0.9, 0.1, 0.05), 'accent_r': (0.9, 0.1, 0.05)})
    fig = plt.figure(figsize=(16, 12))
    views = [(24, 38, '3/4 front'), (22, -142, '3/4 rear'), (90, -90, 'top'), (3, 0, 'side')]
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
            ax.add_collection3d(Poly3DCollection(tri, facecolors=fc, edgecolors='none'))
        ax.set_xlim(-3.2, 3.2); ax.set_ylim(-3.2, 3.2); ax.set_zlim(-2.8, 3.6)
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
