"""Planting a map mod's trees (v1.7.37): a vegetation model is often a
cluster - one drawable holding up to twenty palms spread over 60 m, placed
with its middle at ground level, so the palms on the low side of a slope
stand buried to the fronds and those on the high side show their roots
(the Gator Keys: trunks 1-9 m underground by the mod's own trunk
collision). Each plant is a set of connected pieces of the model's
triangles (a palm's trunk, its fronds, the tufts at its foot); its base is
where its lowest piece starts. Each plant is moved up or down (its vertices
only, in the model) so its base sits just under the ground there, and gets
a trunk of collision.

Carefully, as a wrong move is worse than none: a tall plant moves only to a
surface its own trunk passes through or stands just above (not a roof over
its crown, not a seabed far under it), and pieces that are not a plant of
their own (fronds, coconuts the grouping left out) move with the plant they
touch, or not at all.

  plants(tri)              the connected pieces of a drawable's triangles
  group(pieces)            the pieces gathered into plants
  Layers(soups)            the ground's surfaces, layer upon layer, on a grid
  shifts(plants, place, layers)   each plant's move; the vertex table
  trunks(moves, place, material)   collision boxes round the planted trunks
"""
import numpy as np

MIN_HEIGHT = 2.0    # a piece shorter is a leaf card, a tuft (it goes with a plant, or stays)
TALL = 4.0          # a plant this tall has a trunk to plant
MAX_MOVE = 15.0     # the furthest a buried plant is lifted
MAX_DROP = 3.0      # and the furthest one standing above the ground is lowered
COVER_MOVE = 1.0    # a low plant of its own (ground cover) moves at most this far
TOUCH = 1.2         # a piece this near a moved plant moves with it
MAX_WIDTH = 26.0    # a plant wider is pieces of several welded together (left as it is)
SINK = 0.25         # the base this far under the ground
TRUNK_HEIGHT = 3.0  # the collision trunk's height


def key(p):
    return (round(float(p[0]), 3), round(float(p[1]), 3), round(float(p[2]), 3))


def plants(tri, cell=0.002):
    """Connected pieces of a triangle array (n x 3 x 3): triangles joined by
    a shared corner (positions within `cell`). Each: its positions (k x 3),
    base (x, y, z: the middle of its lowest vertices, its lowest z), height,
    trunk radius, triangles."""
    if not len(tri):
        return []
    pts = tri.reshape(-1, 3)
    q = np.round(pts / cell).astype(np.int64)
    _, inv = np.unique(q, axis=0, return_inverse=True)
    inv = inv.reshape(-1, 3)
    n = int(inv.max()) + 1
    parent = np.arange(n)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a, b, c in inv:
        ra = find(a)
        for x in (b, c):
            rx = find(x)
            if rx != ra:
                parent[rx] = ra
    roots = np.array([find(i) for i in range(n)])
    labels = roots[inv[:, 0]]  # (a triangle's piece: its first corner's)
    out = []
    for lab in np.unique(labels):
        p = np.unique(tri[labels == lab].reshape(-1, 3), axis=0)
        zmin, zmax = p[:, 2].min(), p[:, 2].max()
        low = p[p[:, 2] <= zmin + 0.6]
        base = np.array([low[:, 0].mean(), low[:, 1].mean(), zmin])
        radius = float(np.sqrt(((low[:, :2] - base[:2]) ** 2).sum(axis=1)).max()) if len(low) > 1 else 0.3
        out.append(dict(positions=p, base=base, height=float(zmax - zmin), radius=radius,
                        triangles=int((labels == lab).sum())))
    return out


def group(pieces, same=1.0, near=4.0):
    """Pieces gathered into plants, the tallest first: a piece goes with the
    nearest plant within `near` (twice that for one under MIN_HEIGHT) whose
    height its base is within (a frond up in a crown), or whose base its own
    is within `same` of (a stem from the same root); the rest are plants of
    their own. Each plant: positions, base, top, height, radius (its first,
    tallest piece's), members."""
    out = []
    for c in sorted(pieces, key=lambda c: -c['height']):
        tall = c['height'] >= MIN_HEIGHT
        at = c['base'] if tall else c['positions'].mean(axis=0)
        if out:
            bases = np.array([g['base'] for g in out])
            tops = np.array([g['top'] for g in out])
            d = np.hypot(bases[:, 0] - at[0], bases[:, 1] - at[1])
            within = (d < (near if tall else 2 * near)) & (at[2] >= bases[:, 2] + 1.0) & (at[2] <= tops + 1.0)
            k = int(np.argmin(np.where(within, d, np.inf))) if within.any() else int(np.argmin(d))
            if within.any() or d[k] < (same if tall else near):
                g = out[k]
                g['members'].append(c)
                if c['base'][2] < g['base'][2]:
                    g['base'] = np.array([g['base'][0], g['base'][1], c['base'][2]])
                g['top'] = max(g['top'], float(c['base'][2] + c['height']))
                continue
        out.append(dict(base=c['base'].copy(), radius=c['radius'], members=[c],
                        top=float(c['base'][2] + c['height'])))
    for g in out:
        g['positions'] = np.concatenate([m['positions'] for m in g['members']])
        g['top'] = float(g['positions'][:, 2].max())
        g['height'] = g['top'] - float(g['base'][2])
        g['triangles'] = sum(m['triangles'] for m in g['members'])
    return out


class Layers:
    """The ground's upward surfaces (world space), every height there is on
    a grid of `cell` metres: from triangle soups (vertices, triangles)."""

    def __init__(self, soups, cell=1.0, up=0.5):
        self.cell = cell
        pts = []
        for v, t in soups:
            if not len(t):
                continue
            tri = v[t]
            n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
            ln = np.linalg.norm(n, axis=1)
            tri = tri[n[:, 2] > up * np.maximum(ln, 1e-12)]
            if not len(tri):
                continue
            edge = np.max([np.linalg.norm(tri[:, a] - tri[:, b], axis=1) for a, b in ((0, 1), (1, 2), (2, 0))], axis=0)
            steps = np.clip(np.ceil(edge / (cell / 2)), 1, 120).astype(np.int64)
            for k in np.unique(steps):
                sel = tri[steps == k]
                g = np.array([(i / k, j / k) for i in range(k + 1) for j in range(k + 1 - i)])
                p = sel[:, None, 0, :] + (sel[:, None, 1, :] - sel[:, None, 0, :]) * g[None, :, 0:1] + \
                    (sel[:, None, 2, :] - sel[:, None, 0, :]) * g[None, :, 1:2]
                pts.append(p.reshape(-1, 3))
        p = np.concatenate(pts) if pts else np.zeros((0, 3))
        ij = np.floor(p[:, :2] / cell).astype(np.int64)
        zq = np.round(p[:, 2] * 10).astype(np.int64)
        keys = np.unique(np.stack([ij[:, 0], ij[:, 1], zq], 1), axis=0)
        self.layers = {}
        for i, j, z in keys.tolist():
            self.layers.setdefault((i, j), []).append(z / 10.0)

    def at(self, x, y):
        """The surface heights at (x, y), lowest first ([] where there are none)."""
        return self.layers.get((int(np.floor(x / self.cell)), int(np.floor(y / self.cell))), [])


def ground_for(layers, x, y, base, top):
    """The surface a plant at (x, y), its base at `base` and crown at `top`
    (world), stands on, and how it was found: one within half a metre of its
    base (it stands there); else the first above its base, below its crown
    (buried); else the first under it, within MAX_DROP (standing above the
    ground). (None, why) when there is none."""
    s = layers.at(x, y)
    near = [h for h in s if abs(h - base) <= 0.5]
    if near:
        return min(near, key=lambda h: abs(h - base)), 'on'
    above = [h for h in s if base + 0.5 < h <= min(base + MAX_MOVE, top - 2.0)]
    if above:
        return min(above), 'buried'
    below = [h for h in s if base - MAX_DROP <= h < base - 0.5]
    if below:
        return max(below), 'above'
    return None, 'no_ground'


def shifts(plant_list, place, layers, sink=SINK):
    """Each plant's move (dz, model space) so its base sits `sink` under the
    ground: a table position (key) -> dz for every vertex moved, the plants
    moved (with their dz), and what was left and why. `place`: the model's
    placement (pos, q; unrotated models only)."""
    assert abs(place['q'][3]) > 0.99999, 'planting needs an unrotated placement'
    ox, oy, oz = place['pos']
    moves, table, waiting = [], {}, []
    left = dict(no_ground=0, too_wide=0, stays=0)
    found = dict(on=0, buried=0, above=0)
    for pl in plant_list:
        bx, by, bz = pl['base'][0] + ox, pl['base'][1] + oy, pl['base'][2] + oz
        if pl['height'] >= TALL:
            if max(np.ptp(pl['positions'][:, 0]), np.ptp(pl['positions'][:, 1])) > MAX_WIDTH:
                left['too_wide'] += 1  # (pieces of several plants welded together: no one move fits them)
                continue
            g, how = ground_for(layers, bx, by, bz, pl['top'] + oz)
            if g is None:
                left['no_ground'] += 1
                continue
            found[how] += 1
            dz = (g - sink) - bz
        else:
            s = [h for h in layers.at(bx, by) if abs(h - sink - bz) <= COVER_MOVE]
            if not s:
                waiting.append(pl)  # (a leaf card, a coconut: with the plant it touches, if one moves)
                continue
            dz = min(s, key=lambda h: abs(h - sink - bz)) - sink - bz
        for p in pl['positions']:
            table[key(p)] = dz
        moves.append(dict(pl, dz=float(dz)))
    # the pieces left waiting move with a moved plant they touch
    if waiting and moves:
        grid = {}
        for k, m in enumerate(moves):
            for c in {tuple(x) for x in np.floor(m['positions'] / TOUCH).astype(np.int64).tolist()}:
                grid.setdefault(c, k)
        near = [(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)]
        for pl in waiting:
            hit = None
            for c in {tuple(x) for x in np.floor(pl['positions'] / TOUCH).astype(np.int64).tolist()}:
                for d in near:
                    hit = grid.get((c[0] + d[0], c[1] + d[1], c[2] + d[2]))
                    if hit is not None:
                        break
                if hit is not None:
                    break
            if hit is None:
                left['stays'] += 1
                continue
            dz = moves[hit]['dz']
            for p in pl['positions']:
                table[key(p)] = dz
            moves[hit]['members'] = moves[hit]['members'] + pl['members']
    elif waiting:
        left['stays'] += len(waiting)
    left.update(('tall ' + k, v) for k, v in found.items())
    return table, moves, left


def trunks(moves, place, material, height=TRUNK_HEIGHT, max_radius=0.8):
    """Collision boxes round the planted trunks (world space; a plant at
    least TALL with a trunk no wider than `max_radius`): vertices (n x 3),
    triangles (m x 3), materials (m)."""
    vs, ts, ms = [], [], []
    for pl in moves:
        if pl['height'] < TALL or pl['radius'] > max_radius:
            continue
        r = min(max(pl['radius'], 0.25), 0.6)
        bx, by = pl['base'][0] + place['pos'][0], pl['base'][1] + place['pos'][1]
        z0 = pl['base'][2] + place['pos'][2] + pl['dz']
        z1 = z0 + min(height, pl['height'] * 0.6)
        k = len(vs)
        for z in (z0, z1):
            for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                vs.append([bx + sx * r, by + sy * r, z])
        faces = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),  # bottom (down), top (up)
                 (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
        for f in faces:
            ts.append([k + f[0], k + f[1], k + f[2]])
            ms.append(material)
    if not vs:
        return np.zeros((0, 3)), np.zeros((0, 3), np.int64), np.zeros(0, np.uint32)
    return np.array(vs, np.float64), np.array(ts, np.int64), np.array(ms, np.uint32)
