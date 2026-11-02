"""Collision for map sets, made light enough for the console to stream.

The Gator Keys' collision is its render meshes: 293,000 triangles, 15 MiB as
one bound dictionary, which the game never loaded (v1.7.30: none of the
island's 56 models came in). Here each piece's mesh is welded, cut below the
shallows, decimated (quadric edge collapse within a set error, its open
borders kept where they are) and split into BVH meshes of a few thousand
polygons, each with its own tree built the way the stock ones are (checked
against the mod's PC files and the console's nj_01_4.xbn):

  vertices   int16 x3 over the mesh's box (offset its centre, quantum half its
             size / 32767), the box a hair bigger than the vertices
  polygons   32 bytes: unit normal, area (its low byte the material's index),
             u16 vertices x4 (the fourth 0: a triangle), u16 neighbours x4
             (the polygon across the edge v[k] v[k+1]; 65535 none)
  tree       the mesh's box, quantization 65535 / its size and the inverse;
             nodes of 16 bytes, depth first: u16 min x3, max x3 (floor and
             ceil of (p - box min) * quantization), u16 (a leaf: its first
             polygon; inside: the nodes in its subtree), u8 (a leaf: its
             polygons, four but the last one; inside: 0), pad 0xCD. Subtree
             headers (min x3, max x3, first node, end) for each subtree of at
             most 128 nodes whose parent's is bigger (Bullet's rule), or the
             whole tree's when it is that small.
  header     margin 0.005 (a composite: 0.04); +0x50 the centroid of the
             mesh's (signed) volume and +0x6C the volume (a composite: the
             sum of its children's volume times centroid at +0x50).
"""
import heapq
import math
import struct

import numpy as np

MAX_POLYS = 6000      # per BVH mesh: its polygon array well inside a 256 KiB load chunk
LEAF = 4              # polygons per leaf
SUBTREE_NODES = 128   # Bullet's 2048-byte subtrees of 16-byte nodes
BVH_MARGIN, COMPOSITE_MARGIN = 0.005, 0.04
BOX_SLACK = 0.01      # the mesh's box past its vertices


# ------------------------------------------------------------------ reading
def soup(b, matrix=None):
    """A bound read by bounds360.read_bound as a triangle soup: vertices (n x
    3, float64), triangles (m x 3), each triangle's material (the u32 value).
    Quads come out as two triangles."""
    if b['type'] == 12:
        parts = []
        for k, c in enumerate(b['children']):
            m = np.frombuffer(b['matrices'][k], '<f4').reshape(4, 4).astype(np.float64)
            parts.append(soup(c, m))
        return join(parts)
    n = len(b['polygons']) // 32
    raw = np.frombuffer(b['polygons'], np.uint8).reshape(n, 32)
    idx = raw[:, 16:24].copy().view('<u2').astype(np.int64)
    mat_index = raw[:, 12].astype(np.int64)  # (the area's low byte: little-endian, its first)
    v = np.frombuffer(b['vertices'], '<i2').reshape(-1, 3).astype(np.float64)
    v = v * np.array(b['quantum']) + np.array(b['offset'])
    if matrix is not None and not np.allclose(matrix[:3, :3], np.eye(3), atol=1e-6) or \
            matrix is not None and np.abs(matrix[3, :3]).max() > 1e-6:
        v = v @ matrix[:3, :3] + matrix[3, :3]
    quad = idx[:, 3] != 0
    tris = np.concatenate([idx[:, :3], idx[quad][:, [0, 2, 3]]])
    mats = np.array(b['materials'], np.uint32)
    mi = np.concatenate([mat_index, mat_index[quad]])
    mi = np.minimum(mi, len(mats) - 1)
    return v, tris, mats[mi]


def join(parts):
    vs, ts, ms, base = [], [], [], 0
    for v, t, m in parts:
        vs.append(v)
        ts.append(t + base)
        ms.append(m)
        base += len(v)
    if not vs:
        return np.zeros((0, 3)), np.zeros((0, 3), np.int64), np.zeros(0, np.uint32)
    return np.concatenate(vs), np.concatenate(ts), np.concatenate(ms)


# ------------------------------------------------------------------ cleaning
def weld(v, t, m, tol=0.01):
    """Vertices within `tol` made one; degenerate triangles and repeats (either
    winding) dropped; unused vertices dropped."""
    if len(t) == 0:
        return v[:0], t, m
    key = np.round(v / tol).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inv = inv.reshape(-1)
    v = v[first]
    t = inv[t]
    ok = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    t, m = t[ok], m[ok]
    _, keep = np.unique(np.sort(t, axis=1), axis=0, return_index=True)
    keep.sort()
    t, m = t[keep], m[keep]
    area = 0.5 * np.linalg.norm(np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]]), axis=1)
    t, m = t[area > 1e-5], m[area > 1e-5]
    return compact(v, t, m)


def compact(v, t, m):
    used = np.unique(t)
    remap = np.full(len(v), -1, np.int64)
    remap[used] = np.arange(len(used))
    return v[used], remap[t], m


def cut_below(v, t, m, z):
    """Triangles wholly below `z` dropped (the deep sea floor)."""
    keep = v[t][:, :, 2].max(axis=1) >= z
    return compact(v, t[keep], m[keep])


# ------------------------------------------------------------------ decimation
def decimate(v, t, m, err, min_dot=0.3, border=25.0):
    """Quadric edge collapse (Garland and Heckbert), each vertex's quadric the
    sum of its faces' planes unweighted, so a collapse whose cost is under
    err^2 keeps the new point within `err` of every plane it stood for. An
    open edge adds the plane through it square to its face, weighted
    `border`: the mesh's open borders (where pieces meet) move along
    themselves only, within err / sqrt(border) (`border` None: their vertices
    do not move at all). The vertices of edges three faces share stay. A
    collapse that would turn a face over (or crush it) is not made."""
    nv, nf = len(v), len(t)
    if nf < 8:
        return v, t, m
    p = v.copy()
    e1, e2 = p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]]
    nrm = np.cross(e1, e2)
    ln = np.linalg.norm(nrm, axis=1, keepdims=True)
    nrm = nrm / np.maximum(ln, 1e-12)
    d = -(nrm * p[t[:, 0]]).sum(axis=1)
    a, b, c = nrm[:, 0], nrm[:, 1], nrm[:, 2]
    fq = np.stack([a * a, a * b, a * c, a * d, b * b, b * c, b * d, c * c, c * d, d * d], axis=1)
    q = np.zeros((nv, 10))
    for k in range(3):
        np.add.at(q, t[:, k], fq)
    edges = np.concatenate([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]])
    es = np.sort(edges, axis=1)
    uniq, inv, counts = np.unique(es, axis=0, return_inverse=True, return_counts=True)
    inv = inv.reshape(-1)
    locked = np.zeros(nv, bool)
    bad = uniq[counts > 2] if border is not None else uniq[counts != 2]
    locked[bad[:, 0]] = True
    locked[bad[:, 1]] = True
    if border is not None:
        # open edges: the plane through the edge, square to its face
        rows = np.flatnonzero(counts[inv] == 1)
        if len(rows):
            ea, eb = edges[rows, 0], edges[rows, 1]
            face = rows % nf
            along = p[eb] - p[ea]
            bn = np.cross(along, nrm[face])
            bl = np.linalg.norm(bn, axis=1, keepdims=True)
            ok = bl[:, 0] > 1e-9
            bn = bn[ok] / bl[ok]
            ea, eb = ea[ok], eb[ok]
            bd = -(bn * p[ea]).sum(axis=1)
            x, y, z = bn[:, 0], bn[:, 1], bn[:, 2]
            bq = border * np.stack([x * x, x * y, x * z, x * bd, y * y, y * z, y * bd, z * z, z * bd, bd * bd], axis=1)
            np.add.at(q, ea, bq)
            np.add.at(q, eb, bq)

    P = p.tolist()
    Q = q.tolist()
    F = t.tolist()
    alive = [True] * nf
    vf = [set() for _ in range(nv)]
    for f, tri in enumerate(F):
        for x in tri:
            vf[x].add(f)
    stamp = [0] * nv
    dead = [False] * nv
    lock = locked.tolist()
    limit = err * err

    def qcost(qq, x, y, z):
        return (qq[0] * x * x + 2 * qq[1] * x * y + 2 * qq[2] * x * z + 2 * qq[3] * x + qq[4] * y * y +
                2 * qq[5] * y * z + 2 * qq[6] * y + qq[7] * z * z + 2 * qq[8] * z + qq[9])

    def plan(u, w):
        """(cost, keep, gone, point) for collapsing the edge u-w."""
        if lock[u] and lock[w]:
            return None
        qq = [Q[u][i] + Q[w][i] for i in range(10)]
        if lock[u] or lock[w]:
            keep, gone = (u, w) if lock[u] else (w, u)
            x, y, z = P[keep]
            return qcost(qq, x, y, z), keep, gone, (x, y, z)
        # the best point: solve the 3x3 system, else the better end or the middle
        a11, a12, a13, a22, a23, a33 = qq[0], qq[1], qq[2], qq[4], qq[5], qq[7]
        b1, b2, b3 = -qq[3], -qq[6], -qq[8]
        det = (a11 * (a22 * a33 - a23 * a23) - a12 * (a12 * a33 - a23 * a13) + a13 * (a12 * a23 - a22 * a13))
        best = None
        if abs(det) > 1e-9:
            x = (b1 * (a22 * a33 - a23 * a23) - a12 * (b2 * a33 - a23 * b3) + a13 * (b2 * a23 - a22 * b3)) / det
            y = (a11 * (b2 * a33 - a23 * b3) - b1 * (a12 * a33 - a23 * a13) + a13 * (a12 * b3 - b2 * a13)) / det
            z = (a11 * (a22 * b3 - b2 * a23) - a12 * (a12 * b3 - b2 * a13) + b1 * (a12 * a23 - a22 * a13)) / det
            # (only near the edge: far off, a nearly flat system's answer is noise)
            mx, my, mz = [(P[u][i] + P[w][i]) / 2 for i in range(3)]
            span = sum((P[u][i] - P[w][i]) ** 2 for i in range(3))
            if (x - mx) ** 2 + (y - my) ** 2 + (z - mz) ** 2 <= span + 1e-6:
                best = (qcost(qq, x, y, z), (x, y, z))
        for cand in (tuple(P[u]), tuple(P[w]), tuple((P[u][i] + P[w][i]) / 2 for i in range(3))):
            cc = qcost(qq, *cand)
            if best is None or cc < best[0]:
                best = (cc, cand)
        return best[0], w, u, best[1]  # (u goes into w, at the point)

    heap = []

    def push(u, w):
        r = plan(u, w)
        if r is None or r[0] > limit:
            return
        heapq.heappush(heap, (max(r[0], 0.0), r[1], r[2], stamp[r[1]], stamp[r[2]], r[3]))

    seen = set()
    for f, tri in enumerate(F):
        for k in range(3):
            u, w = tri[k], tri[(k + 1) % 3]
            key = (u, w) if u < w else (w, u)
            if key not in seen:
                seen.add(key)
                push(u, w)
    del seen

    def normal(a_, b_, c_):
        ux, uy, uz = b_[0] - a_[0], b_[1] - a_[1], b_[2] - a_[2]
        vx, vy, vz = c_[0] - a_[0], c_[1] - a_[1], c_[2] - a_[2]
        return uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx

    collapses = 0
    while heap:
        cost, keep, gone, sk, sg, pt = heapq.heappop(heap)
        if dead[keep] or dead[gone] or stamp[keep] != sk or stamp[gone] != sg:
            continue
        # faces around either end not on the edge: none turned over or crushed
        shared = vf[keep] & vf[gone]
        if not shared:
            continue  # (no longer an edge)
        ok = True
        for vv in (keep, gone):
            for f in vf[vv]:
                if f in shared:
                    continue
                tri = F[f]
                old = [P[x] for x in tri]
                new = [pt if x == vv else P[x] for x in tri]
                n0 = normal(*old)
                n1 = normal(*new)
                l0 = math.sqrt(n0[0] ** 2 + n0[1] ** 2 + n0[2] ** 2)
                l1 = math.sqrt(n1[0] ** 2 + n1[1] ** 2 + n1[2] ** 2)
                if l1 < 1e-6 or (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2]) < min_dot * l0 * l1:
                    ok = False
                    break
            if not ok:
                break
        if not ok:
            continue
        # gone -> keep, at pt
        for f in shared:
            alive[f] = False
            for x in F[f]:
                if x != gone:
                    vf[x].discard(f)
        for f in vf[gone] - shared:
            F[f] = [keep if x == gone else x for x in F[f]]
            vf[keep].add(f)
        vf[gone] = set()
        dead[gone] = True
        P[keep] = list(pt)
        Q[keep] = [Q[keep][i] + Q[gone][i] for i in range(10)]
        stamp[keep] += 1
        collapses += 1
        around = set()
        for f in vf[keep]:
            around.update(F[f])
        around.discard(keep)
        for w in around:
            push(keep, w)
    tt = np.array([F[f] for f in range(nf) if alive[f]], np.int64).reshape(-1, 3)
    mm = np.array([m[f] for f in range(nf) if alive[f]], np.uint32)
    vv = np.array(P)
    return weld(vv, tt, mm, 1e-4)


# ------------------------------------------------------------------ splitting
def split(v, t, m, most=MAX_POLYS):
    """Meshes of at most `most` triangles: halved along the longest side of
    their triangles' centres until they fit (each with its own vertices)."""
    out = []
    stack = [np.arange(len(t))]
    cen = v[t].mean(axis=1)
    while stack:
        sel = stack.pop()
        if len(sel) <= most:
            vv, tt, mm = compact(v, t[sel], m[sel])
            out.append((vv, tt, mm))
            continue
        c = cen[sel]
        axis = int(np.argmax(c.max(axis=0) - c.min(axis=0)))
        order = sel[np.argsort(c[:, axis], kind='stable')]
        half = len(order) // 2
        stack += [order[:half], order[half:]]
    return out


# ------------------------------------------------------------------ the BVH
def pair_quads(vq, t, m, dot_min=0.9995, off_max=0.01):
    """Triangles paired across an edge into quads where the two are one flat,
    convex four-sided polygon of one material (the stock meshes are mostly
    quads): (n x 4) corners in order round the polygon, -1 the fourth of a
    triangle. A quad's fourth corner is never vertex 0 (the game reads a 0
    there as a triangle)."""
    n = len(t)
    cr = np.cross(vq[t[:, 1]] - vq[t[:, 0]], vq[t[:, 2]] - vq[t[:, 0]])
    ar = np.linalg.norm(cr, axis=1)
    un = cr / ar[:, None]
    owner = {}
    for i in range(n):
        for k in range(3):
            owner[(int(t[i, k]), int(t[i, (k + 1) % 3]))] = (i, k)
    used = np.zeros(n, bool)
    polys, pm = [], []

    def turns(corners):
        """Convex in order round it, every corner turning by more than half a
        degree, against the normal written for it (its two triangles' sum)."""
        a_, b_, c_, d_ = (vq[x] for x in corners)
        normal = np.cross(b_ - a_, c_ - a_) + np.cross(c_ - a_, d_ - a_)
        normal = normal / max(np.linalg.norm(normal), 1e-12)
        for k in range(4):
            p0, p1, p2 = vq[corners[k]], vq[corners[(k + 1) % 4]], vq[corners[(k + 2) % 4]]
            e0, e1 = p1 - p0, p2 - p1
            if np.dot(np.cross(e0, e1), normal) <= 0.01 * np.linalg.norm(e0) * np.linalg.norm(e1):
                return False
        return True

    for i in np.argsort(-ar, kind='stable'):
        if used[i]:
            continue
        best = None
        for k in range(3):
            p_, q_, r_ = int(t[i, k]), int(t[i, (k + 1) % 3]), int(t[i, (k + 2) % 3])
            hit = owner.get((q_, p_))
            if hit is None:
                continue
            j, kj = hit
            if j == i or used[j] or m[j] != m[i]:
                continue
            dot = float(np.dot(un[i], un[j]))
            if dot < dot_min:
                continue
            s_ = int(t[j, (kj + 2) % 3])
            if abs(float(np.dot(vq[s_] - vq[p_], un[i]))) > off_max:
                continue
            quad = [r_, p_, s_, q_]
            if not turns(quad):
                continue
            if best is None or dot > best[0]:
                best = (dot, j, quad)
        if best is None:
            continue
        _, j, quad = best
        while quad[3] == 0:
            quad = quad[1:] + quad[:1]
        used[i] = used[j] = True
        polys.append(quad)
        pm.append(m[i])
    for i in np.flatnonzero(~used):
        polys.append([int(t[i, 0]), int(t[i, 1]), int(t[i, 2]), -1])
        pm.append(m[i])
    return np.array(polys, np.int64).reshape(-1, 4), np.array(pm, np.uint32)


def poly_triangles(polys):
    """The triangles a polygon list covers (a quad's two: 0 1 2, 0 2 3)."""
    quad = polys[:, 3] >= 0
    return np.concatenate([polys[:, :3], polys[quad][:, [0, 2, 3]]])


def build_bvh(v, t, m, quads=True):
    """A type-10 bound (the dict bounds360.write_bound takes) for the mesh."""
    mats = sorted(set(int(x) for x in m))
    assert len(mats) <= 255, len(mats)
    mat_of = {x: i for i, x in enumerate(mats)}
    lo = v.min(axis=0) - BOX_SLACK
    hi = v.max(axis=0) + BOX_SLACK
    offset = (lo + hi) / 2
    quantum = (hi - lo) / 2 / 32767.0
    s16 = np.clip(np.round((v - offset) / quantum), -32767, 32767).astype(np.int16)
    vq = s16.astype(np.float64) * quantum + offset
    # (a triangle crushed by the quantization: dropped)
    cr = np.cross(vq[t[:, 1]] - vq[t[:, 0]], vq[t[:, 2]] - vq[t[:, 0]])
    keep = np.linalg.norm(cr, axis=1) > 2e-6
    t, m = t[keep], m[keep]
    if quads:
        polys, m = pair_quads(vq, t, m)
    else:
        polys = np.concatenate([t, np.full((len(t), 1), -1, np.int64)], axis=1)
    n = len(polys)
    assert 0 < n <= 65535 and len(v) <= 65535, (n, len(v))
    full = polys.copy()
    full[full[:, 3] < 0, 3] = full[full[:, 3] < 0, 0]
    corners = vq[full]
    cen = corners.mean(axis=1)
    pmin = corners.min(axis=1)
    pmax = corners.max(axis=1)
    # the polygons' order and the tree: halves of the longest side of the
    # centres, split on a multiple of four so every leaf but the last has four
    order = np.arange(n)
    nodes = []  # ['leaf'|'inner', min, max, first polygon | subtree size, polygons]

    def build(start, end):
        me = len(nodes)
        nodes.append(None)
        if end - start <= LEAF:
            sel = order[start:end]
            nodes[me] = ['leaf', pmin[sel].min(axis=0), pmax[sel].max(axis=0), start, end - start]
            return
        sel = order[start:end]
        c = cen[sel]
        axis = int(np.argmax(c.max(axis=0) - c.min(axis=0)))
        order[start:end] = sel[np.argsort(c[:, axis], kind='stable')]
        groups = (end - start + LEAF - 1) // LEAF
        mid = start + LEAF * (groups // 2)
        mid = min(max(mid, start + 1), end - 1)
        build(start, mid)
        build(mid, end)
        nodes[me] = ['inner', None, None, len(nodes) - me, 0]

    import sys
    sys.setrecursionlimit(max(sys.getrecursionlimit(), 10000))
    build(0, n)
    count = len(nodes)
    assert count <= 65535, count
    size = [nd[3] if nd[0] == 'inner' else 1 for nd in nodes]
    quant = 65535.0 / (hi - lo)
    qmin = [None] * count
    qmax = [None] * count
    # leaves from their polygons; inner nodes exactly the union of their
    # children's (as the stock ones are), back to front
    for i in range(count - 1, -1, -1):
        nd = nodes[i]
        if nd[0] == 'leaf':
            qmin[i] = np.clip(np.floor((nd[1] - lo) * quant), 0, 65535).astype(np.int64)
            qmax[i] = np.clip(np.ceil((nd[2] - lo) * quant), 0, 65535).astype(np.int64)
        else:
            left = i + 1
            right = left + size[left]
            qmin[i] = np.minimum(qmin[left], qmin[right])
            qmax[i] = np.maximum(qmax[left], qmax[right])
    node_bytes = bytearray()
    for i, nd in enumerate(nodes):
        idx = nd[3] if nd[0] == 'leaf' else size[i]
        cnt = nd[4] if nd[0] == 'leaf' else 0
        node_bytes += struct.pack('<7HBB', *[int(x) for x in qmin[i]], *[int(x) for x in qmax[i]], int(idx), cnt, 0xCD)
    # subtree headers
    subs = []

    def headers(i):
        if nodes[i][0] == 'leaf':
            return
        left = i + 1
        right = left + size[left]
        headers(left)
        headers(right)
        if size[i] > SUBTREE_NODES:
            for ch in (left, right):
                if size[ch] <= SUBTREE_NODES:
                    subs.append((ch, ch + size[ch]))

    headers(0)
    if not subs:
        subs = [(0, count)]
    sub_bytes = b''.join(struct.pack('<8H', *[int(x) for x in qmin[r]], *[int(x) for x in qmax[r]], r, e)
                         for r, e in subs)
    # the polygons, in the tree's order
    polys = polys[order]
    m = m[order]
    nb = neighbours(polys)
    poly = bytearray(32 * n)
    for i in range(n):
        c4 = polys[i]
        a_, b_, c_ = vq[c4[0]], vq[c4[1]], vq[c4[2]]
        crs = np.cross(b_ - a_, c_ - a_)
        area = np.linalg.norm(crs) / 2
        if c4[3] >= 0:
            c2 = np.cross(c_ - a_, vq[c4[3]] - a_)
            area += np.linalg.norm(c2) / 2
            crs = crs + c2
        unit = crs / np.linalg.norm(crs)
        bits = struct.unpack('<I', struct.pack('<f', float(area)))[0]
        bits = (bits & 0xFFFFFF00) | mat_of[int(m[i])]
        struct.pack_into('<3fI4H4H', poly, 32 * i, float(unit[0]), float(unit[1]), float(unit[2]), bits,
                         int(c4[0]), int(c4[1]), int(c4[2]), int(c4[3]) if c4[3] >= 0 else 0,
                         *[int(x) for x in nb[i]])
    tris = poly_triangles(polys)
    vol, vcen = volume(vq, tris)
    centroid = (lo + hi) / 2
    radius = float(np.linalg.norm(vq - centroid, axis=1).max()) + BOX_SLACK
    return dict(type=10, radius=radius, max=hi.tolist(), min=lo.tolist(), centroid=centroid.tolist(),
                cg=[0.0, 0.0, 0.0], dist=vcen.tolist(), margin=[BVH_MARGIN] * 3, volume=vol,
                polygons=bytes(poly), vertices=s16.astype('<i2').tobytes(), quantum=quantum.tolist(),
                offset=offset.tolist(), materials=mats,
                tree=dict(nodes=bytes(node_bytes), subtrees=sub_bytes, min=lo.tolist(), max=hi.tolist(),
                          quant=quant.tolist(), inv=(1.0 / quant).tolist(), count=count, cap=count))


def neighbours(polys):
    """For each polygon and edge k (corner k to the next round it) the other
    polygon on that edge, 65535 if none (on an edge three share: one of the
    others); a triangle's fourth 65535."""
    n = len(polys)
    out = np.full((n, 4), 0xFFFF, np.int64)
    owner = {}
    for i in range(n):
        k_n = 4 if polys[i, 3] >= 0 else 3
        for k in range(k_n):
            a, b = int(polys[i, k]), int(polys[i, (k + 1) % k_n])
            owner.setdefault((a, b) if a < b else (b, a), []).append((i, k))
    for lst in owner.values():
        if len(lst) < 2:
            continue
        for i, k in lst:
            for j, _ in lst:
                if j != i:
                    out[i, k] = j
                    break
    return out


def volume(v, t):
    """The mesh's signed volume (the divergence theorem over its triangles)
    and that volume's centroid (the box's centre when it is next to none)."""
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
    six = (a * np.cross(b, c)).sum(axis=1)
    vol = six.sum() / 6.0
    if abs(vol) < 1e-6:
        return 0.0, (v.min(axis=0) + v.max(axis=0)) / 2
    cen = ((a + b + c) * six[:, None]).sum(axis=0) / (24.0 * vol)
    return float(vol), cen


def composite(children, identity):
    """A type-12 bound holding the meshes (in its own space)."""
    lo = np.min([c['min'] for c in children], axis=0)
    hi = np.max([c['max'] for c in children], axis=0)
    weighted = np.sum([np.array(c['dist']) * c['volume'] for c in children], axis=0)
    return dict(type=12, radius=float(np.linalg.norm(hi - lo) / 2), max=hi.tolist(), min=lo.tolist(),
                centroid=((lo + hi) / 2).tolist(), cg=[0.0, 0.0, 0.0], dist=weighted.tolist(),
                margin=[COMPOSITE_MARGIN] * 3, children=children, matrices=[identity] * len(children),
                boxes=[(list(c['min']), list(c['max'])) for c in children])


def bound_for(v, t, m, identity, most=MAX_POLYS):
    """One BVH, or a composite of several when the mesh is big."""
    parts = split(v, t, m, most)
    kids = [build_bvh(*p) for p in parts]
    return kids[0] if len(kids) == 1 else composite(kids, identity)


# ------------------------------------------------------------------ checking
def check_bvh(b, rays=200, seed=1):
    """The tree read back as the game walks it: its layout (depth first,
    escape counts, leaves in polygon order, boxes holding their polygons, inner
    boxes the union of their children, subtree headers by the rule), then
    vertical rays walked through it stacklessly (and through its subtree
    headers) against every polygon tried in turn. Returns the worst miss."""
    n = len(b['polygons']) // 32
    raw = np.frombuffer(b['polygons'], np.uint8).reshape(n, 32)
    idx = raw[:, 16:24].copy().view('<u2').astype(np.int64)
    nbr = raw[:, 24:32].copy().view('<u2').astype(np.int64)
    quad = idx[:, 3] != 0
    assert (nbr[~quad, 3] == 0xFFFF).all()
    v = np.frombuffer(b['vertices'], '<i2').reshape(-1, 3).astype(np.float64)
    v = v * np.array(b['quantum']) + np.array(b['offset'])
    assert idx.max() < len(v)
    # each polygon: its normal and area as written (a quad flat and convex)
    nrm = raw[:, :12].copy().view('<f4').reshape(-1, 3).astype(np.float64)
    four = idx.copy()
    four[~quad, 3] = four[~quad, 0]
    for k in range(4):
        e0 = v[four[:, (k + 1) % 4]] - v[four[:, k]]
        e1 = v[four[:, (k + 2) % 4]] - v[four[:, (k + 1) % 4]]
        turn = (np.cross(e0, e1) * nrm).sum(axis=1)
        assert (turn[quad] > 0).all(), k
    assert np.allclose(np.linalg.norm(nrm, axis=1), 1.0, atol=1e-4)
    tr = b['tree']
    lo, quant = np.array(tr['min']), np.array(tr['quant'])
    nodes = np.frombuffer(tr['nodes'], np.uint8).reshape(-1, 16)
    nh = nodes[:, :14].copy().view('<u2').reshape(-1, 7).astype(np.int64)
    cnt = nodes[:, 14].astype(np.int64)
    N = len(nh)
    assert N == tr['count']

    def size(i):
        return 1 if cnt[i] else int(nh[i, 6])

    assert size(0) == N
    covered = 0
    for i in range(N):
        if cnt[i]:
            first = nh[i, 6]
            assert first == covered, (i, first, covered)
            covered += cnt[i]
            pts = v[four[first:first + cnt[i]].reshape(-1)]
            q0 = np.floor((pts.min(axis=0) - lo) * quant)
            q1 = np.ceil((pts.max(axis=0) - lo) * quant)
            assert (nh[i, :3] <= q0 + 1).all() and (nh[i, 3:6] >= q1 - 1).all(), (i, nh[i], q0, q1)
        else:
            left = i + 1
            right = left + size(left)
            assert right + size(right) == i + nh[i, 6]
            assert (nh[i, :3] == np.minimum(nh[left, :3], nh[right, :3])).all()
            assert (nh[i, 3:6] == np.maximum(nh[left, 3:6], nh[right, 3:6])).all()
    assert covered == n
    subs = np.frombuffer(tr['subtrees'], '<u2').reshape(-1, 8).astype(np.int64)
    # vertical rays from above at random points over the mesh
    rng = np.random.default_rng(seed)
    a, bb, c, dd = v[idx[:, 0]], v[idx[:, 1]], v[idx[:, 2]], v[four[:, 3]]
    facing = np.flatnonzero(np.abs(nrm[:, 2]) > 0.2)  # (not walls: seen from above)
    if len(facing) == 0:
        return 0.0
    pick = facing[rng.integers(0, len(facing), rays)]
    w = rng.dirichlet([1, 1, 1], rays)
    pts = w[:, :1] * a[pick] + w[:, 1:2] * bb[pick] + w[:, 2:] * c[pick]

    def hits_xy(x, y, polys):
        best = None
        for p in polys:
            for p0, p1, p2 in ((a[p], bb[p], c[p]), (a[p], c[p], dd[p])) if quad[p] else ((a[p], bb[p], c[p]),):
                d = (p1[1] - p2[1]) * (p0[0] - p2[0]) + (p2[0] - p1[0]) * (p0[1] - p2[1])
                if abs(d) < 1e-12:
                    continue
                l1 = ((p1[1] - p2[1]) * (x - p2[0]) + (p2[0] - p1[0]) * (y - p2[1])) / d
                l2 = ((p2[1] - p0[1]) * (x - p2[0]) + (p0[0] - p2[0]) * (y - p2[1])) / d
                l3 = 1 - l1 - l2
                if min(l1, l2, l3) < -1e-9:
                    continue
                z = l1 * p0[2] + l2 * p1[2] + l3 * p2[2]
                best = z if best is None else max(best, z)
        return best

    def walk(x, y, start, end):
        found = []
        qx, qy = (x - lo[0]) * quant[0], (y - lo[1]) * quant[1]
        i = start
        while i < end:
            over = nh[i, 0] <= qx <= nh[i, 3] and nh[i, 1] <= qy <= nh[i, 4]
            if cnt[i] and over:
                found += list(range(nh[i, 6], nh[i, 6] + cnt[i]))
            i += 1 if (over or cnt[i]) else int(nh[i, 6])
        return found

    worst = 0.0
    for x, y, _ in pts:
        brute = hits_xy(x, y, range(n))
        tree = hits_xy(x, y, walk(x, y, 0, N))
        via = []
        for s in subs:
            qx, qy = (x - lo[0]) * quant[0], (y - lo[1]) * quant[1]
            if s[0] <= qx <= s[3] and s[1] <= qy <= s[4]:
                via += walk(x, y, int(s[6]), int(s[7]))
        sub = hits_xy(x, y, via)
        assert brute is not None and tree is not None and sub is not None, (x, y)
        worst = max(worst, abs(brute - tree), abs(brute - sub))
    return worst
