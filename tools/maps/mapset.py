"""A PC map mod as a destination for the DeLorean's time travel: its models,
textures and collision as console resources, ready for the installer to put
into vehicles.img, with the lines and placements the installer and the
trainer need. Nothing here is part of the city: the trainer places the
pieces as frozen objects when the destination is chosen, and deletes them
when the trip home is made (one destination loaded at a time).

usage: python3 -I mapset.py <set.json> <source dir> <xdr dir> <template.xtd.bin:flags> <xlzx> <out dir>
                             [--draw=<template.xdr.bin:flags>,<shader sources dir>]
                             [--skip-textures] [--skip-models]
  set.json     the set's settings (tools/maps/sets/<name>.json)
  source dir   the mod's pieces: <model>.wdr / .wtd / .wbn, the .ide and the
               .wpl files the set names (a mod of several archives: one
               directory holding them all, links will do)
  xdr dir      the models converted by mapdraw.py (<model>.xdr.bin, flags.txt)
  template     a decompressed stock .xtd with a texture with mips
  xlzx         the resource packer (pc2x360/xlzx.cpp)
  --draw       what mapdraw.py converted the models with: needed for a set
               with tiles or carriers (drawables made here)

Writes to <out dir>:
  <prefix><model>.xdr, <prefix><txd>.xtd   models and texture dictionaries
  <prefix>col.xbd   the collision: a bound dictionary keyed by model name
               (collide.py: the mod's meshes welded, cut below the shallows,
               decimated and split into BVH meshes with trees of their own;
               16 KiB pages); several (<prefix>col0.xbd...) when it would be
               big. The game links a model to its bound only when the
               dictionary holding it loads (its key the model's name hash),
               whatever the dictionary is called; at boot it loads them all
               only without a good cache, so the trainer asks for the set's
               with its models (v1.7.34; v1.7.33's was gtxd.xbd, named after
               the .ide, on the wrong idea that the .ide's name picks it)
  objs.ide     the models' lines for that .ide
  place.txt    one line per piece for the trainer
  map.txt      the destination: label, date, the shift from the mod's
               coordinates, its sea level, the arrival, its area, the
               prefix (the installer drops image files with it that the set
               no longer has), its collision dictionaries' names

Every model is renamed <prefix><name> (no clash with the game's own names),
shortened where that is over 20 characters: the game registers an image's
files by the first 24 characters of the name, the extension among them (a
longer one is never registered: v1.7.30's gk_gtr_deco_polgrnda018).
WPL rotations are stored inverted (checked: Gator Keys' rotated pad lines up
with its collision only that way), so place.txt carries the true rotation.
Only models the .wpl files place are made; a model placed several times is
one file, one piece per placement.

Pieces of the game's own models (v1.7.37; cfg['stock']: the game's model
names, stocknames.py): a mod placing the game's props (Wake Island: its
landing lights, crates, buoys, bushes) gets no file for them - the piece
names the game's model, and the game's own collision stands there (place.txt
collision 2).

Collision comes in world space. Per model (collision.mode "model", the
default): one .wbn named like the model, placed once, moved into the model's
space (its placement's rotation undone). By area (mode "world", v1.7.37: Wake
Island's 52 files cover regions, not models): every file's triangles cut
into cells of collision.cell metres, each cell's bound carried by a piece of
its own (<prefix>c<n>, an invisible drawable of one triangle far below the
ground, its .ide line's box the cell's collision) placed unrotated at the
cell's centre; the mod's models then have no bounds of their own.

A set's own additions and edits (v1.7.38, Benny's on the Gator Keys):
cfg['extra'] names a text file of placements beyond the mod's (model x y z
heading; the game's own props by name, or models made for the set:
props.py, their .wdr/.wtd and custom.ide in the source directory, the .ide
named in cfg['ide']); cfg['edit'] is {model: {"drop_textures": [...],
"drop_box": [[x0, y0, z0, x1, y1, z1], ...]}}: the geometries drawn with
those colour maps left out of the model (mapdraw.py --edits), and the
collision triangles whose centres lie in those boxes (the model's space)
dropped. A <model>.colmesh.npz in the source directory (v, t, m in the
model's space) is a piece's collision like a .wbn's (props.py writes them).

Tiles (v1.7.32): a model over the set's tiles.radius across, placed once and
unrotated, is cut into tiles of tiles.cell metres (mapdraw.plan_tiles), each
a model of its own named <model>_<n>, its drawable converted here, its
collision (mode "model") the piece's triangles over its cell, all in its own
space round its origin; the game neither draws nor collides with a model
several hundred metres across placed as an object (v1.7.31's island: its
terrain and roads missing, its buildings there).
"""
import collections
import fnmatch
import glob
import json
import math
import os
import struct
import subprocess
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import bounds360  # noqa: E402
import collide  # noqa: E402
import mapdraw  # noqa: E402
import alphamask  # noqa: E402
import maptex  # noqa: E402
import plant  # noqa: E402
import rsc5  # noqa: E402

XTD_TYPE, XDR_TYPE, XBD_TYPE = 7, 109, 32
DYNAMIC_FLAG = 0x20000  # (an IDE flag: a movable object - cleared from a model with collision since v1.7.42)
NAME_LIMIT = 20  # a file's name with its 4-character extension within the game's 24
DICTIONARY_BYTES = 3 << 20  # a collision dictionary's size, about, before another is started
CARRIER_FLAGS = 0xC00000    # a carrier's .ide flags (the mods' usual, GIMS's default)
CARRIER_DRAW = 300          # and its draw distance


def short_names(names, prefix, rename=None):
    """Each name with the prefix, within NAME_LIMIT: the set's own renames
    first, then without underscores, then cut with three characters of its
    hash; never two the same."""
    rename = {k.lower(): v.lower() for k, v in (rename or {}).items()}
    out, taken = {}, set()
    for n in sorted(names):
        full = prefix + rename.get(n, n)
        if len(full) > NAME_LIMIT:
            full = prefix + rename.get(n, n).replace('_', '')
        if len(full) > NAME_LIMIT:
            full = full[:NAME_LIMIT - 3] + '%03x' % (batconv.joaat(n) & 0xFFF)
        # (two cut to the same three digits - Route 66's 4,614 tiles, v1.7.43:
        # the next three of the hash, then a count)
        k = 0
        while full in taken:
            k += 1
            h = (batconv.joaat(n) >> (12 * min(k, 2))) & 0xFFF
            full = full[:NAME_LIMIT - 3] + ('%03x' % h if k <= 2 else '%03d' % (k % 1000))
        assert len(full) <= NAME_LIMIT and full not in taken, (n, full)
        taken.add(full)
        out[n] = full
    return out


# ------------------------------------------------------------------ sources
def read_ide(path):
    out = []
    section = None
    for line in open(path, encoding='latin-1'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        low = line.lower()
        if low in ('objs', 'tobj', 'anim', 'txdp', '2dfx', 'mlo', 'amat', 'path', 'tree', 'lodm', 'cars', 'peds'):
            section = low
            continue
        if low == 'end':
            section = None
            continue
        if section == 'objs':
            out.append([f.strip() for f in line.split(',')])
    return out


def read_wpl(path):
    d = open(path, 'rb').read()
    count = struct.unpack_from('<I', d, 4)[0]
    out = []
    for i in range(count):
        x, y, z, qx, qy, qz, qw, h, flags, lod, _, lodd = struct.unpack_from('<7fIIiIf', d, 68 + 48 * i)
        # (stored inverted: the true rotation is the conjugate)
        out.append(dict(pos=(x, y, z), q=(-qx, -qy, -qz, qw), hash=h))
    return out


def read_wpl_cars(path):
    """The .wpl's car generators (its fifth count; 56 bytes each: position,
    the direction it faces (x, y), a length, the model's hash, colours,
    flags): (hash, pos, heading degrees) for each."""
    d = open(path, 'rb').read()
    n_inst, n_cars = struct.unpack_from('<I', d, 4)[0], struct.unpack_from('<I', d, 16)[0]
    off = 68 + n_inst * 48
    out = []
    for k in range(n_cars):
        if off + 56 * (k + 1) > len(d):
            break
        x, y, z, dx, dy, _, h = struct.unpack_from('<6fI', d, off + 56 * k)
        out.append(dict(hash=h, pos=(x, y, z), heading=math.degrees(math.atan2(-dx, dy)) % 360.0))
    return out


def read_names(path):
    return {l.strip().lower() for l in open(path) if l.strip() and not l.startswith('#')}


def model_textures(path):
    """Texture names a PC drawable's shaders reference (lower case, as hashed)."""
    d = rsc5.Resource.pc(path)
    dr = rsc5.read_drawable(d, 0)
    names = set()
    for s in dr['shader_group']['shaders']:
        for p in batconv.shader_params(d, s['off']):
            if p['type'] == 0 and p['value'] is not None:
                np_ = d.vptr(p['value'] + 20)
                if np_ is not None:
                    names.add(d.cstr(np_).split('/')[-1].lower())
    return names


# ------------------------------------------------------------------ collision
def read_piece(path):
    """A .wbn, every mesh checked whole: each polygon's corners (three, or
    four for a quad) index its vertex array."""
    raw = open(path, 'rb').read()
    magic, rtype, flags = struct.unpack_from('<III', raw, 0)
    r = rsc5.Resource(zlib.decompress(raw[12:]), flags, False, rtype, path)
    b = bounds360.read_bound(r, r.vptr(8))

    def check(b):
        if b['type'] == 12:
            for c in b['children']:
                check(c)
            return
        whole = len(b['vertices']) // 6
        n = len(b['polygons']) // 32
        idx = np.frombuffer(b['polygons'], np.uint8).reshape(n, 32)[:, 16:24].copy().view('<u2')
        assert int(idx.max()) < whole, (path, int(idx.max()), whole)
        normal = np.frombuffer(b['polygons'], np.uint8).reshape(n, 32)[:, :12].copy().view('<f4').reshape(-1, 3)
        length = np.linalg.norm(normal, axis=1)
        assert ((np.abs(length - 1.0) < 0.01) | (length < 1e-6)).all(), path  # (unit, or a degenerate one's zero)
    check(b)
    return b


def cut_boxes(b, boxes, place):
    """A piece as a soup with the triangles whose centres lie in one of the
    boxes (the model's space, the placement unrotated) dropped."""
    v, t, m = collide.soup(b)
    cen = v[t].mean(axis=1) - np.array(place['pos'])
    keep = np.ones(len(t), bool)
    for x0, y0, z0, x1, y1, z1 in boxes:
        keep &= ~((cen[:, 0] >= x0) & (cen[:, 0] <= x1) & (cen[:, 1] >= y0) & (cen[:, 1] <= y1)
                  & (cen[:, 2] >= z0) & (cen[:, 2] <= z1))
    print('edit: %s: %d of %d collision triangles dropped (in its boxes)' % (place['model'], int((~keep).sum()), len(t)))
    return dict(type='soup', v=v, t=t[keep], m=m[keep])


def mesh_points(b):
    if b['type'] == 'soup':
        return b['v']
    if b['type'] == 12:
        return np.concatenate([mesh_points(c) for c in b['children']])
    v = np.frombuffer(b['vertices'], '<i2').reshape(-1, 3).astype(np.float64)
    return v * np.array(b['quantum']) + np.array(b['offset'])


def identity_matrix():
    return struct.pack('<16f', 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


def bound_bytes(b):
    """About what a bound takes in a dictionary."""
    if b['type'] == 12:
        return 0x100 + 64 * len(b['children']) + sum(bound_bytes(c) for c in b['children'])
    return 0x100 + len(b['polygons']) + len(b['vertices']) + len(b['tree']['nodes']) + len(b['tree']['subtrees'])


def build_dictionary(entries):
    """bounds360.build_dictionary with pages sized for big meshes: 16 KiB, as
    the stock static collision (.xbn) has them, so the game loads it in chunks
    of 256 KiB (v1.7.30's 15 MiB in 512 KiB chunks never loaded), bigger only
    if an array would not fit in one chunk; no more than 100 chunks."""
    entries = sorted(entries, key=lambda e: bounds360.joaat(e[0]))
    biggest = 0
    total = 0
    for _, b in entries:
        stack = [b]
        while stack:
            x = stack.pop()
            if x['type'] == 12:
                stack += x['children']
                continue
            for k in ('polygons', 'vertices'):
                biggest = max(biggest, len(x[k]))
                total += len(x[k])
            biggest = max(biggest, len(x['tree']['nodes']))
            total += len(x['tree']['nodes']) + len(x['tree']['subtrees'])
    shift = 6
    while 16 * (256 << shift) < biggest + 4096 or (total * 1.1) / (256 << shift) > 1500:
        shift += 1
    page = 256 << shift
    a = bounds360.Arena(16 * page)
    a.put(b'\x00' * 0x20)
    offs = [bounds360.write_bound(a, b) for _, b in entries]
    n = len(entries)
    keys = a.put(struct.pack('>%dI' % n, *[bounds360.joaat(name) for name, _ in entries]))
    vals = a.put(struct.pack('>%dI' % n, *[bounds360.ptr(o) for o in offs]))
    blockmap = a.put(b'\x00\x00\x00\x00' + b'\xCD' * 0x2C)
    root = struct.pack('>IIIIIHHIHH', bounds360.VT_DICT, bounds360.ptr(blockmap), 0, 1, bounds360.ptr(keys), n, n,
                       bounds360.ptr(vals), n, n)
    a.buf[0:0x20] = root
    body, flags = a.segment(shift)
    cut = bounds360.chunk_crossings(body, flags)
    assert not cut, cut[:4]
    return body, flags


def dictionaries(entries, prefix, name=None):
    """The entries in dictionaries of about DICTIONARY_BYTES each: (name,
    entries) - one named `name` (or <prefix>col) when they fit, else
    <prefix>col0, col1..."""
    entries = sorted(entries, key=lambda e: e[0])
    groups, size = [[]], 0
    for e in entries:
        n = bound_bytes(e[1])
        if groups[-1] and size + n > DICTIONARY_BYTES:
            groups.append([])
            size = 0
        groups[-1].append(e)
        size += n
    if len(groups) == 1:
        return [(name or prefix + 'col', groups[0])]
    return [('%scol%d' % (prefix, k), g) for k, g in enumerate(groups)]


# ------------------------------------------------------------------ arrival
def height_map(pieces, cell=2.0, keep=None):
    """The highest walkable collision surface on a grid (NaN: none): every
    upward-facing triangle sampled at least twice a cell along its edges.
    `keep` (x, y, r): only within the disc (v1.7.44: Route 66's mountains
    span kilometres; the whole mod sampled at every 2 m ran out of memory).
    The grid is filled batch by batch, never all the samples at once."""
    tris_all = []
    for b in pieces:
        stack = [b]
        while stack:
            x = stack.pop()
            if x['type'] == 12:
                stack += x['children']
                continue
            if x['type'] == 'soup':  # (a set's own: its upward faces by their winding)
                v, t3 = x['v'], x['t']
                nrm = np.cross(v[t3[:, 1]] - v[t3[:, 0]], v[t3[:, 2]] - v[t3[:, 0]])
                up = nrm[:, 2] > 0.7 * np.maximum(np.linalg.norm(nrm, axis=1), 1e-9)
                tris = [t3[up]]
            else:
                v = np.frombuffer(x['vertices'], '<i2').reshape(-1, 3).astype(np.float64)
                v = v * np.array(x['quantum']) + np.array(x['offset'])
                n = len(x['polygons']) // 32
                raw = np.frombuffer(x['polygons'], np.uint8).reshape(n, 32)
                idx = raw[:, 16:24].copy().view('<u2').astype(np.int64)
                normal = raw[:, :12].copy().view('<f4').reshape(-1, 3)
                up = normal[:, 2] > 0.7  # (walkable faces only)
                quads = up & (idx[:, 3] != 0)
                tris = [idx[up][:, [0, 1, 2]], idx[quads][:, [0, 2, 3]]]
            for t in tris:
                if not len(t):
                    continue
                corners = v[t]  # (n, 3, 3)
                if keep is not None:
                    cen = corners.mean(axis=1)
                    near = np.hypot(cen[:, 0] - keep[0], cen[:, 1] - keep[1]) <= keep[2] + 60.0
                    corners = corners[near]
                    if not len(corners):
                        continue
                tris_all.append(corners)
    if not tris_all:
        return np.full((1, 1), np.nan), np.zeros(2), cell
    lo = np.min([c[:, :, :2].min(axis=(0, 1)) for c in tris_all], axis=0)
    hi = np.max([c[:, :, :2].max(axis=(0, 1)) for c in tris_all], axis=0)
    w, h = (np.ceil((hi - lo) / cell).astype(np.int64) + 1).tolist()
    hm = np.full((h, w), -np.inf)
    for corners in tris_all:
        a, b_, c = corners[:, 0], corners[:, 1], corners[:, 2]
        edge = np.maximum(np.maximum(np.linalg.norm(b_ - a, axis=1), np.linalg.norm(c - a, axis=1)),
                          np.linalg.norm(c - b_, axis=1))
        steps = np.clip(np.ceil(edge / (cell / 2)), 1, 200).astype(np.int64)
        for k in np.unique(steps):
            sel = np.flatnonzero(steps == k)
            g = np.array([(i / k, j / k) for i in range(k + 1) for j in range(k + 1 - i)])
            # (in batches: a 200-step triangle is 20,000 samples)
            batch = max(1, 2000000 // len(g))
            for at in range(0, len(sel), batch):
                part = sel[at:at + batch]
                aa, bb, cc = a[part], b_[part], c[part]
                p_ = (aa[:, None, :] + (bb - aa)[:, None, :] * g[None, :, 0:1] +
                      (cc - aa)[:, None, :] * g[None, :, 1:2]).reshape(-1, 3)
                ij = ((p_[:, :2] - lo) / cell).astype(np.int64)
                ij[:, 0] = np.clip(ij[:, 0], 0, w - 1)
                ij[:, 1] = np.clip(ij[:, 1], 0, h - 1)
                np.maximum.at(hm, (ij[:, 1], ij[:, 0]), p_[:, 2])
    hm[np.isinf(hm)] = np.nan
    return hm, lo, cell


def best_runs(hm, lo, cell, sea, width=6.0, top=6):
    """Straight, flat, clear runs: (length, x, y, z, heading) from each
    start cell along 16 headings, the ground within 0.25 m a cell and 1.2 m
    overall of the start, `width` wide, above the sea."""
    h, w = hm.shape
    found = []
    half = int(round(width / 2 / cell))
    for sy in range(0, h, 3):
        for sx in range(0, w, 3):
            z0 = hm[sy, sx]
            if not np.isfinite(z0) or z0 < sea + 0.8:
                continue
            for k in range(16):
                a = 2 * math.pi * k / 16
                dx, dy = -math.sin(a), math.cos(a)  # heading a: the nose's direction
                px, py = math.cos(a), math.sin(a)
                length = 0
                prev = z0
                while True:
                    step = length + 1
                    cx, cy = sx + dx * step, sy + dy * step
                    good = True
                    zs = []
                    for o in range(-half, half + 1):
                        x, y = int(round(cx + px * o)), int(round(cy + py * o))
                        if not (0 <= x < w and 0 <= y < h) or not np.isfinite(hm[y, x]):
                            good = False
                            break
                        zs.append(hm[y, x])
                    if not good:
                        break
                    zc = zs[len(zs) // 2]
                    if abs(zc - prev) > 0.25 or max(zs) - min(zs) > 0.6 or abs(zc - z0) > 1.2:
                        break
                    prev = zc
                    length = step
                if length * cell >= 60:
                    found.append((length * cell, lo[0] + sx * cell, lo[1] + sy * cell, float(z0), a))
    found.sort(reverse=True)
    out = []
    for f in found:  # (distinct places)
        if all(math.hypot(f[1] - g[1], f[2] - g[2]) > 40 for g in out):
            out.append(f)
        if len(out) >= top:
            break
    return out


# ------------------------------------------------------------------ writing
def pack(xlzx, rtype, flags, body, out_path):
    tmp = out_path + '.bin'
    open(tmp, 'wb').write(body)
    subprocess.run([xlzx, 'rsc', str(rtype), '0x%08X' % flags, tmp, out_path], check=True,
                   stdout=subprocess.DEVNULL)
    os.remove(tmp)


def quat_matrix(q):
    """The rotation (x, y, z, w) as a matrix taking model to world (column
    vectors: world = R @ model)."""
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def to_model(v, place):
    """World points into the placed model's space."""
    return (v - np.array(place['pos'])) @ quat_matrix(place['q'])


def tile_of(points, tiles):
    """For each triangle centre (n x 2) the tile whose cell holds it, or the
    tile with the nearest origin where none does."""
    x0, y0, cell = tiles[0]['grid']
    ix = np.floor((points[:, 0] - x0) / cell).astype(np.int64)
    iy = np.floor((points[:, 1] - y0) / cell).astype(np.int64)
    by_cell = {t['cell']: k for k, t in enumerate(tiles)}
    origins = np.array([t['origin'][:2] for t in tiles])
    out = np.empty(len(points), np.int64)
    for i in range(len(points)):
        k = by_cell.get((int(ix[i]), int(iy[i])))
        if k is None:
            k = int(np.argmin(((origins - points[i]) ** 2).sum(axis=1)))
        out[i] = k
    return out


def check_tile(body, flags, donor, tile, lift=None, added=0):
    """A tile's drawable read back: the triangles planned for it (and the
    back faces two-sided shaders' got), every index inside its buffer, every
    position one of the mod's (lifted as the plants were) moved to the tile."""
    x = rsc5.Resource(body, flags, True, XDR_TYPE, 'tile')
    xd = rsc5.read_drawable(x, 0)
    tri = sum(g['index_count'] // 3 for m in xd['lods'][0] for g in m['geometries'])
    assert tri == tile['triangles'] + added, (tri, tile['triangles'], added)
    pc = np.unique(mapdraw.lifted(mapdraw.pc_triangles(donor), lift).reshape(-1, 3), axis=0)
    ox, oy, oz = tile['origin']
    checked = 0
    for m in xd['lods'][0]:
        for g in m['geometries']:
            n = g['vertex_count'] or g['vb_count']
            idx = struct.unpack_from('>%dH' % g['index_count'], x.phys, g['ib_data'][1])
            assert max(idx) < n
            at = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_360)[0])[0]
            for v in range(0, n, max(1, n // 50)):
                px, py, pz = struct.unpack_from('>3f', x.phys, g['vb_data'][1] + v * g['vb_stride'] + at)
                miss = np.abs(pc - np.array([px + ox, py + oy, pz + oz])).max(axis=1).min()
                assert miss < 2e-3, (px, py, pz, miss)
                checked += 1
    return tri, checked


def check_carrier(body, flags, bounds):
    """A carrier's drawable read back: one triangle, its box the cell's."""
    x = rsc5.Resource(body, flags, True, XDR_TYPE, 'carrier')
    xd = rsc5.read_drawable(x, 0)
    tri = sum(g['index_count'] // 3 for m in xd['lods'][0] for g in m['geometries'])
    assert tri == 1, tri
    lo, hi = struct.unpack_from('>3f', x.virt, 0x20), struct.unpack_from('>3f', x.virt, 0x30)
    assert np.allclose(lo, bounds[0], atol=1e-3) and np.allclose(hi, bounds[1], atol=1e-3), (lo, hi, bounds)


class Drawer:
    """mapdraw's converter set up as the models were converted (--draw)."""
    def __init__(self, draw_arg, cutout=(), edits=None):
        assert draw_arg, 'a set with tiles or carriers needs --draw (what mapdraw.py converted the models with)'
        self.cutout = set(cutout)
        self.edits = edits or {}  # model -> the colour maps whose geometries are left out
        dtmpl_arg, shader_dir = draw_arg.split(',', 1)
        dtmpl_path, dtmpl_flags = dtmpl_arg.rsplit(':', 1)
        self.tmpl = rsc5.Resource(open(dtmpl_path, 'rb').read(), int(dtmpl_flags, 16), True, XDR_TYPE, dtmpl_path)
        self.sources = [mapdraw.load_console(q) for q in sorted(glob.glob(os.path.join(shader_dir, '*.bin')))]
        for k, fb in mapdraw.MAP_FALLBACK.items():
            batconv.SPS_FALLBACK.setdefault(k, fb)

    def tile(self, donor, t, lift=None, name=None):
        conv = mapdraw.MapDrawable(self.tmpl, donor, self.sources, log=lambda msg: None, tile=t, lift=lift,
                                   cutout=self.cutout, drop=self.edits.get(name))
        flags, body = conv.run()
        return flags, body, conv.tile_bounds(), sum(conv.added)

    def model(self, donor, lift, name=None):
        conv = mapdraw.MapDrawable(self.tmpl, donor, self.sources, log=lambda msg: None, lift=lift,
                                   cutout=self.cutout, drop=self.edits.get(name))
        flags, body = conv.run()
        return flags, body, conv.lifted, conv.added, conv.dropped

    def carrier(self, donor, bounds):
        conv = mapdraw.MapDrawable(self.tmpl, donor, self.sources, log=lambda msg: None, tiny=True, bounds=bounds)
        return conv.run()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    draw_arg = next((a[len('--draw='):] for a in sys.argv[1:] if a.startswith('--draw=')), None)
    skip = {a[len('--skip-'):] for a in sys.argv[1:] if a.startswith('--skip-')}  # textures, models
    set_path, src, xdr_dir, tmpl_arg, xlzx, out = args[:6]
    cfg = json.load(open(set_path))
    prefix = cfg['prefix']
    os.makedirs(out, exist_ok=True)
    tmpl_path, tmpl_flags = tmpl_arg.rsplit(':', 1)
    tmpl = maptex.Template(rsc5.Resource(open(tmpl_path, 'rb').read(), int(tmpl_flags, 16), True, XTD_TYPE, tmpl_path))
    cutout = alphamask.cutouts(src)  # (the plants' cut-out masks: as mapdraw.py --cutout-from converted the models)
    edits = {m.lower(): e for m, e in cfg.get('edit', {}).items()}
    drawer = Drawer(draw_arg, cutout, mapdraw.set_edits(set_path)) if draw_arg else None

    # ---- the mod's model lines, the game's own names, the placements
    ide_files = cfg['ide'] if isinstance(cfg['ide'], list) else [cfg['ide']]
    models = {}
    for f in ide_files:
        for fields in read_ide(os.path.join(src, f)):
            models.setdefault(fields[0].lower(), fields)
    stock = set()
    if cfg.get('stock'):
        p = cfg['stock'] if os.path.isabs(cfg['stock']) else os.path.join(os.path.dirname(set_path), cfg['stock'])
        stock = read_names(p)
    by_hash = {batconv.joaat(m): m for m in models}
    by_hash.update({batconv.joaat(s): s for s in stock})  # (the game's own model stands where the names clash)
    placed, unknown = [], collections.Counter()
    for w in cfg['wpl']:
        for inst in read_wpl(os.path.join(src, w)):
            name = by_hash.get(inst['hash'])
            if name is None:
                unknown[inst['hash']] += 1
                continue
            placed.append(dict(inst, model=name, stock=name in stock))
    # the part of the map kept (cfg['keep']: centre x, y and a radius, the
    # mod's coordinates; v1.7.43, Route 66: the whole map is 1,500 models
    # and 4,600 tiles, more than the game's model table has room for): a
    # placement whose model's sphere lies wholly beyond the radius goes,
    # and the tiles and collision cells beyond it below
    keep = cfg.get('keep')
    if keep:
        kx, ky, kr = float(keep[0]), float(keep[1]), float(keep[2])
        before = len(placed)

        def inside(p, margin=50.0):
            m = models.get(p['model'])
            if m is not None and len(m) > 14:
                cx, cy = float(m[11]), float(m[12])
                margin = float(m[14])
            else:
                cx = cy = 0.0
            return math.hypot(p['pos'][0] + cx - kx, p['pos'][1] + cy - ky) <= kr + margin

        placed = [p for p in placed if inside(p)]
        print('keep: %d of %d placements within %.0f m of (%.0f, %.0f) (a model\'s sphere touching the disc stays)' % (
            len(placed), before, kr, kx, ky))
    extra_files = cfg.get('extra') or []
    if isinstance(extra_files, str):
        extra_files = [extra_files]
    if extra_files:  # (the set's own placements: model x y z heading, or model x y z qx qy qz qw - the true rotation)
        n_extra = 0
        for ef in extra_files:
            for line in open(os.path.join(os.path.dirname(set_path), ef)):
                f = line.split('#')[0].split()
                if not f:
                    continue
                name = f[0].lower()
                assert name in models or name in stock, 'extra placement of a model with no line: %s' % name
                x, y, z = (float(v) for v in f[1:4])
                if len(f) >= 8:
                    q = tuple(float(v) for v in f[4:8])
                else:
                    half = math.radians(float(f[4])) / 2
                    q = (0.0, 0.0, math.sin(half), math.cos(half))
                placed.append(dict(pos=(x, y, z), q=q, hash=batconv.joaat(name), model=name,
                                   stock=name in stock and name not in models))
                n_extra += 1
        print('extra: %d placements of the set\'s own (%s)' % (
            n_extra, ', '.join('%s x%d' % kv for kv in collections.Counter(p['model'] for p in placed[-n_extra:]).most_common(12))))
    # the vehicles the mod parks (its car generators): the game's own
    # vehicles by name (cfg['vehicles']: stock_vehicles.txt), a mod's name
    # for another game's car swapped by cfg['vehicle_subs'] (Vice City's
    # sparrow: the game's maverick), the rest left out
    vehicles, vehicles_unknown = [], collections.Counter()
    if cfg.get('vehicles'):
        p = cfg['vehicles'] if os.path.isabs(cfg['vehicles']) else os.path.join(os.path.dirname(set_path), cfg['vehicles'])
        vnames = read_names(p)
        subs = {k.lower(): v.lower() for k, v in cfg.get('vehicle_subs', {}).items()}
        vhash = {batconv.joaat(n): n for n in list(vnames) + list(subs)}
        seen = set()
        for w in cfg['wpl']:
            for car in read_wpl_cars(os.path.join(src, w)):
                name = vhash.get(car['hash'])
                name = subs.get(name, name) if name else None
                if name is None or name not in vnames:
                    vehicles_unknown[car['hash']] += 1
                    continue
                key = (name, round(car['pos'][0]), round(car['pos'][1]))
                if key in seen:  # (the stream .wpl files repeat some)
                    continue
                seen.add(key)
                if keep and math.hypot(car['pos'][0] - float(keep[0]), car['pos'][1] - float(keep[1])) > float(keep[2]):
                    continue
                vehicles.append(dict(car, model=name))
        print('vehicles: %d parked (%s)%s' % (
            len(vehicles), ', '.join('%s x%d' % kv for kv in collections.Counter(v['model'] for v in vehicles).most_common(8)),
            ('; %d of models the game has not: %s' % (sum(vehicles_unknown.values()), ', '.join('%08X' % h for h in vehicles_unknown)))
            if vehicles_unknown else ''))
    used = sorted({p['model'] for p in placed if not p['stock']})
    # the creatures (v1.7.39: creature.py's creature.txt in the source
    # directory, cfg['life']['creatures'] saying how many of each): their
    # part models are made like placed ones, the trainer makes and moves them
    creatures = []  # (name, lines, count)
    wanted_creatures = cfg.get('life', {}).get('creatures', {})
    if os.path.exists(os.path.join(src, 'creature.txt')) and wanted_creatures:
        block = None
        for line in open(os.path.join(src, 'creature.txt')):
            f = line.split()
            if not f:
                continue
            if f[0] == 'creature':
                block = (f[1].lower(), [line.rstrip('\n')], int(wanted_creatures.get(f[1].lower(), 0)))
                if block[2] > 0:
                    creatures.append(block)
            elif f[0] == 'part' and block is not None:
                block[1].append(line.rstrip('\n'))
                if block[2] > 0 and f[2].lower() not in used:
                    used.append(f[2].lower())
        used = sorted(used)
        print('creatures: %s' % ', '.join('%s x%d (%d parts)' % (n, c, len(ls) - 1) for n, ls, c in creatures))
    missing = [m for m in used if m not in models or not os.path.exists(os.path.join(src, m + '.wdr'))]
    assert not missing, 'placed models without a .wdr (or a line): %s' % ', '.join(missing)
    count_of = collections.Counter(p['model'] for p in placed)
    stock_placed = sorted({p['model'] for p in placed if p['stock']})
    clashes = sorted(m for m in stock_placed if m in models)
    print('%d models in %d .ide files, %d of them placed (%d placements); %d of the game\'s own models placed '
          '(%d placements%s); %d placements of unknown models (%s)' % (
              len(models), len(ide_files), len(used), sum(count_of[m] for m in used), len(stock_placed),
              sum(count_of[m] for m in stock_placed),
              ('; the mod has lines for %d: %s' % (len(clashes), ', '.join(clashes[:6]))) if clashes else '',
              sum(unknown.values()), ', '.join('%08X x%d' % kv for kv in unknown.most_common(4))))
    # ---- collision, read first: its cells name the carriers
    col = cfg.get('collision', {})
    mode = col.get('mode', 'model')
    floor_z = cfg['sea'] - col.get('seabed', 3.0)
    pieces_world, piece_files, dropped = [], {}, []
    where = {}
    for p in placed:
        where.setdefault(p['model'], p)
    for f in sorted(os.listdir(src)):
        if not f.endswith('.wbn'):
            continue
        name = f[:-4].lower()
        b = read_piece(os.path.join(src, f))
        if name in edits and edits[name].get('drop_box') and name in where:  # (the set's edits: boxes cut out)
            b = cut_boxes(b, edits[name]['drop_box'], where[name])
        pieces_world.append(b)
        if any(fnmatch.fnmatch(name, pat) for pat in col.get('drop', [])):
            dropped.append(name)
            continue
        piece_files[name] = b
    # the set's own props' collision (props.py: triangles in the model's space), in the mod's usual material
    soup_material = collections.Counter()
    for b in piece_files.values():
        soup_material.update(np.unique(collide.soup(b)[2]).tolist())
    own_local = {}  # the set's own props placed more than once: collision in the model's space
    for f in sorted(os.listdir(src)):
        if not f.endswith('.colmesh.npz'):
            continue
        name = f[:-len('.colmesh.npz')].lower()
        if name not in where:
            continue
        z = np.load(os.path.join(src, f))
        if not len(z['t']):
            print('collision: %s has none (an empty mesh), skipped' % name)
            continue
        m = np.full(len(z['t']), soup_material.most_common(1)[0][0] if soup_material else 0, np.uint32)
        if mode == 'model' and count_of[name] != 1:
            # (placed more than once: its collision stays in the model's
            # space as it came, the bound the game instances with each
            # placement - vprops.py's palms and the lab's bricks, v1.7.42)
            own_local[name] = (z['v'].astype(np.float64), z['t'].astype(np.int64), m)
            continue
        v = z['v'].astype(np.float64) @ quat_matrix(where[name]['q']).T + np.array(where[name]['pos'])
        b = dict(type='soup', v=v, t=z['t'].astype(np.int64), m=m)
        pieces_world.append(b)
        piece_files[name] = b
    allpts = np.concatenate([mesh_points(b) for b in pieces_world])
    lo, hi = allpts[:, :2].min(0), allpts[:, :2].max(0)
    carriers = []  # (key, origin, cell box): the cells with collision, in mode "world"
    if mode == 'world':
        ccell = col.get('cell', 150.0)
        gx0, gy0 = math.floor(lo[0] / ccell) * ccell, math.floor(lo[1] / ccell) * ccell
        cells = set()
        for name, b in piece_files.items():
            v, t, m = collide.soup(b)
            cen = v[t].mean(axis=1)
            above = v[t][:, :, 2].max(axis=1) >= floor_z
            ix = np.floor((cen[above, 0] - gx0) / ccell).astype(np.int64)
            iy = np.floor((cen[above, 1] - gy0) / ccell).astype(np.int64)
            cells |= set(zip(ix.tolist(), iy.tolist()))
        if keep:  # (cfg['keep']: the cells wholly beyond the kept disc go)
            cells = {(ix, iy) for ix, iy in cells
                     if math.hypot(gx0 + (ix + 0.5) * ccell - float(keep[0]), gy0 + (iy + 0.5) * ccell - float(keep[1]))
                     <= float(keep[2]) + ccell * 0.71}
        for k, (ix, iy) in enumerate(sorted(cells)):
            carriers.append(dict(key='c%d' % k, cell=(ix, iy), grid=(gx0, gy0, ccell),
                                 origin=(gx0 + (ix + 0.5) * ccell, gy0 + (iy + 0.5) * ccell, 0.0)))
        print('collision by area: %d cells of %.0f m from (%.0f, %.0f)' % (len(carriers), ccell, gx0, gy0))

    # ---- planting (plant.py): the vegetation models' plants moved to the
    # ground the collision has under them; their moves lift the vertices
    # when the model is converted (here: tiles, and the planted models whole)
    lifts, lift_moves = {}, {}
    heights = None
    if cfg.get('plant'):
        layers = plant.Layers([collide.soup(b)[:2] for b in piece_files.values()])
        report = []
        for m in used:
            if not any(fnmatch.fnmatch(m, pat) for pat in cfg['plant']):
                continue
            places = [p for p in placed if p['model'] == m]
            if len(places) != 1 or abs(places[0]['q'][3]) < 0.99999:
                print('planting: %s left as it is (placed %d times%s)' % (
                    m, len(places), '' if abs(places[0]['q'][3]) > 0.99999 else ', rotated'))
                continue
            donor = rsc5.Resource.pc(os.path.join(src, m + '.wdr'))
            plants = plant.group(plant.plants(mapdraw.pc_triangles(donor)))
            table, moves, left = plant.shifts(plants, places[0], layers)
            if not moves:
                continue
            lifts[m] = table
            lift_moves[m] = (moves, places[0])
            tall = sorted(x['dz'] for x in moves if x['height'] >= plant.TALL)
            report.append('%s %d plants (%d tall: moved %s)%s' % (
                m, len(moves), len(tall),
                ('%+.1f..%+.1f m, median %+.1f' % (tall[0], tall[-1], tall[len(tall) // 2])) if tall else '-',
                (', left: %s' % ', '.join('%s %d' % kv for kv in left.items() if kv[1])) if any(left.values()) else ''))
        print('planting: %d models, %d plants moved to the ground: %s' % (
            len(lift_moves), sum(len(mv) for mv, _ in lift_moves.values()), '; '.join(report)))

    # ---- tiles: the big models, placed once and unrotated, cut up
    tcfg = cfg.get('tiles')
    tiles_of = {}
    turned_models = {}
    tiles_dropped, tiles_gone = 0, []
    if tcfg:
        for m in used:
            if float(models[m][14]) <= tcfg['radius']:
                continue
            places = [p for p in placed if p['model'] == m]
            assert len(places) == 1, 'a model over %.0f m (%s) placed %d times: tiles need it once' % (
                tcfg['radius'], m, len(places))
            p = places[0]
            donor = rsc5.Resource.pc(os.path.join(src, m + '.wdr'))
            if abs(p['q'][3]) < 0.99999:
                # (placed turned: the turn baked into the model, its tiles placed
                # unturned - Route 66's connection roads, v1.7.43)
                donor = mapdraw.turned(donor, p['q'])
                turned_models[m] = tuple(p['q'])
                p['q'] = (0.0, 0.0, 0.0, 1.0)
            zmin = cfg['sea'] - tcfg.get('below_sea', 6.0) - p['pos'][2]
            ts = mapdraw.plan_tiles(donor, tcfg['cell'], zmin, lifts.get(m))
            x0 = ts[0]['lo'][0] - ts[0]['cell'][0] * tcfg['cell']
            y0 = ts[0]['lo'][1] - ts[0]['cell'][1] * tcfg['cell']
            if keep:  # (the tiles wholly beyond the kept disc go; a model with none left is dropped from the set)
                far = [t for t in ts if math.hypot(p['pos'][0] + t['origin'][0] - float(keep[0]),
                                                   p['pos'][1] + t['origin'][1] - float(keep[1]))
                       > float(keep[2]) + tcfg['cell'] * 0.71]
                tiles_dropped += len(far)
                ts = [t for t in ts if t not in far]
                if not ts:
                    tiles_gone.append(m)
                    continue
            for k, t in enumerate(ts):
                t['key'] = '%s_%d' % (m, k)
                t['grid'] = (x0, y0, tcfg['cell'])
            tiles_of[m] = (donor, ts)
        print('tiles: %d models over %.0f m in %d tiles of %.0f m: %s%s' % (
            len(tiles_of), tcfg['radius'], sum(len(ts) for _, ts in tiles_of.values()), tcfg['cell'],
            ', '.join('%s %d' % (m, len(ts)) for m, (_, ts) in sorted(tiles_of.items())),
            ('; placed turned, the turn baked in: ' + ', '.join(sorted(turned_models))) if turned_models else ''))
        if keep:
            print('keep: %d tiles beyond the disc dropped; %d models with none left: %s' % (
                tiles_dropped, len(tiles_gone), ', '.join(sorted(tiles_gone)) or 'none'))
            if tiles_gone:
                placed = [p for p in placed if p['model'] not in tiles_gone]
                used = [m for m in used if m not in tiles_gone]

    # the game's names for the models (a tiled one's tiles), the carriers and the texture dictionaries
    game_names = [m for m in used if m not in tiles_of] + [t['key'] for _, ts in tiles_of.values() for t in ts] + \
        [c['key'] for c in carriers]
    mname = short_names(game_names, prefix, cfg.get('rename'))
    tname = short_names({models[m][1].lower() for m in used}, prefix, cfg.get('rename'))
    for k in sorted(set(mname) | set(tname)):
        if mname.get(k, prefix + k) not in (prefix + k, prefix + k.replace('_', '')) or \
                tname.get(k, prefix + k) != prefix + k:
            print('renamed %s -> model %s, dictionary %s' % (k, mname.get(k, '-'), tname.get(k, '-')))

    # ---- textures: each dictionary holds what its models reference
    pc_dicts = {}
    for f in sorted(os.listdir(src)):
        if f.endswith('.wtd'):
            pc_dicts[f[:-4].lower()] = batconv.pc_textures(rsc5.Resource.pc(os.path.join(src, f)), 0)
    wanted = {}
    for m in used:
        txd = models[m][1].lower()
        wanted.setdefault(txd, set()).update(model_textures(os.path.join(src, m + '.wdr')))
    subs = {k.lower(): v.lower() for k, v in cfg.get('substitutes', {}).items()}
    stand_ins = cfg.get('stand_ins', {})
    total_tex = 0
    tex_report = []
    for txd, names in sorted(wanted.items()):
        if 'textures' in skip:
            break
        own = pc_dicts.get(txd, {})
        chosen = {}
        notes = []
        for n in sorted(names):
            key = n[:-4] if n.endswith('.dds') else n
            src_tex = own.get(key)
            how = ''
            if src_tex is None:
                for k in sorted(pc_dicts):
                    if key in pc_dicts[k]:
                        src_tex, how = pc_dicts[k][key], 'from ' + k
                        break
            if src_tex is None and key in subs:
                s = subs[key]
                for k in sorted(pc_dicts):
                    if s in pc_dicts[k]:
                        src_tex, how = pc_dicts[k][s], 'stand-in ' + s
                        break
            if src_tex is None:
                rgba = stand_ins.get(key, stand_ins.get('*_bump' if 'bump' in key or 'normal' in key else '*',
                                                        [128, 128, 128, 255]))
                px = bytes(np.tile(np.array([rgba[2], rgba[1], rgba[0], rgba[3]], np.uint8), 32 * 32))
                src_tex = dict(name=key, w=32, h=32, fmt=batconv.A8R8G8B8, levels=1,
                               res=batconv._Data(px), data=0)
                how = 'flat %s' % (rgba,)
            chosen[n] = src_tex
            if how:
                notes.append('%s (%s)' % (n, how))
        side, detail = cfg.get('tex', maptex.MAP_SIDE), cfg.get('detail', maptex.DETAIL_SIDE)
        for pat, s_side, s_detail in cfg.get('tex_of', []):  # (a dictionary's own cap, by name pattern)
            if fnmatch.fnmatch(txd, pat):
                side, detail = s_side, s_detail
                break
        flags, body, report = maptex.build(tmpl, chosen, side=side, detail=detail)
        verdict, worst = maptex.check(body, flags, chosen)
        # (a level laid out wrong decodes to noise, 40 and up; a translucent
        # gradient's smallest level differs from its parent's box filter by
        # 14 - Wake Island's transition.dds; a leaf cut-out's alpha mask by
        # 27 - Route 66's ag_elm_leaf, its levels re-encoded as DXT)
        assert worst < 32.0, (txd, worst)
        name = tname[txd]
        pack(xlzx, XTD_TYPE, flags, body, os.path.join(out, name + '.xtd'))
        total_tex += len(body)
        tex_report.append('%s.xtd: %s, %d KiB%s' % (name, verdict, len(body) >> 10,
                                                    ('; ' + ', '.join(notes)) if notes else ''))
        print(tex_report[-1])
    print('textures: %.1f MiB in %d dictionaries' % (total_tex / 1048576.0, len(wanted)))

    # ---- models
    flags_of = {}
    for line in open(os.path.join(xdr_dir, 'flags.txt')):
        p = line.split()
        if len(p) == 3:
            flags_of[p[0][:-4]] = int(p[2], 16)
    total_xdr = 0
    replanted = 0
    for m in used:
        if 'models' in skip:
            break
        if m in tiles_of:
            continue
        if m in lifts:  # (converted again here, its plants moved)
            donor = rsc5.Resource.pc(os.path.join(src, m + '.wdr'))
            flags, body, moved, added, gone = drawer.model(donor, lifts[m], m)
            assert moved, m
            mapdraw.check(body, flags, donor, lifts[m], added, gone)
            replanted += 1
        else:
            flags, body = flags_of[m], open(os.path.join(xdr_dir, m + '.xdr.bin'), 'rb').read()
        pack(xlzx, XDR_TYPE, flags, body, os.path.join(out, mname[m] + '.xdr'))
        total_xdr += len(body)
    if replanted:
        print('models: %d converted again with their plants moved, every one read back' % replanted)
    if tiles_of:
        tiles_done = 0
        for m, (donor, ts) in sorted(tiles_of.items()):
            for t in ts:
                flags, body, t['bounds'], added = drawer.tile(donor, t, lifts.get(m), m)
                check_tile(body, flags, donor, t, lifts.get(m), added)
                if 'models' not in skip:
                    pack(xlzx, XDR_TYPE, flags, body, os.path.join(out, mname[t['key']] + '.xdr'))
                total_xdr += len(body)
                tiles_done += 1
        print('tiles: %d drawables, every one read back (its triangles, indices and positions)' % tiles_done)
    print('models: %.1f MiB, %d drawables' % (total_xdr / 1048576.0, len(game_names) - len(carriers)))

    # ---- collision: each .wbn (world space) welded, cut below the shallows,
    # decimated within the set's error, then moved into its model's space (a
    # tiled model's: each triangle into its tile's) or cut into the carriers'
    # cells, and split into BVH meshes
    def error_for(name):
        for pat, e in col.get('error', {}).items():
            if pat != '*' and fnmatch.fnmatch(name, pat):
                return e
        return col.get('error', {}).get('*', 0.2)

    gathered, unplaced = {}, []
    tri_in = tri_out = 0
    materials = collections.Counter()

    def route(name, v, t, m):
        """World-space triangles into the bounds they belong to: the carriers'
        cells (mode "world"), or the model's space (its tiles' each)."""
        if mode == 'world':
            gx0, gy0, ccell = carriers[0]['grid']
            cen = v[t].mean(axis=1)
            ix = np.floor((cen[:, 0] - gx0) / ccell).astype(np.int64)
            iy = np.floor((cen[:, 1] - gy0) / ccell).astype(np.int64)
            by_cell = {c['cell']: c for c in carriers}
            ids = ix * 100000 + iy
            for cid in np.unique(ids):
                sel = ids == cid
                c = by_cell.get((int(ix[sel][0]), int(iy[sel][0])))
                if c is None:  # (a cell beyond the kept disc)
                    continue
                tv, tt, tm = collide.compact(v, t[sel], m[sel])
                gathered.setdefault(mname[c['key']], []).append((tv - np.array(c['origin']), tt, tm))
            return
        local = to_model(v, where[name])
        if name in tiles_of:
            ts = tiles_of[name][1]
            which = tile_of(local[t].mean(axis=1)[:, :2], ts)
            for k in np.unique(which):
                sel = which == k
                tv, tt, tm = collide.compact(local, t[sel], m[sel])
                gathered.setdefault(mname[ts[k]['key']], []).append((tv - np.array(ts[k]['origin']), tt, tm))
        else:
            gathered.setdefault(mname[name], []).append((local, t, m))

    for name, b in sorted(piece_files.items()):
        if mode == 'model':
            p = where.get(name)
            if p is None or name not in used:
                unplaced.append(name)
                continue
            assert count_of[name] == 1, 'collision per model needs %s placed once (it is %d times)' % (name, count_of[name])
        v, t, m = collide.soup(b)
        tri_in += len(t)
        v, t, m = collide.weld(v, t, m, col.get('weld', 0.02))
        v, t, m = collide.cut_below(v, t, m, floor_z)
        v, t, m = collide.decimate(v, t, m, error_for(name))
        tri_out += len(t)
        materials.update(np.unique(m).tolist())
        route(name, v, t, m)
    # the planted trunks (plant.trunks): a box round each, in the set's most
    # usual material
    trunk_tris = 0
    if lift_moves:
        material = materials.most_common(1)[0][0] if materials else 0
        for m, (moves, place) in sorted(lift_moves.items()):
            v, t, mt = plant.trunks(moves, place, material)
            if len(t):
                route(m, v, t, mt)
                trunk_tris += len(t)
        print('planting: %d triangles of trunk collision (material %08X)' % (trunk_tris, material))
    for name, (v, t, m) in sorted(own_local.items()):
        if name not in used:
            continue
        tri_in += len(t)
        v, t, m = collide.weld(v, t, m, col.get('weld', 0.02))
        tri_out += len(t)
        materials.update(np.unique(m).tolist())
        gathered.setdefault(mname[name], []).append((v, t, m))
    if own_local:
        print('collision: %d of the set\'s own props placed more than once, their bounds in the model\'s space: %s' % (
            len(own_local), ', '.join('%s x%d' % (n, count_of[n]) for n in sorted(own_local))))
    assert not unplaced, 'collision with no placed model: %s (drop it)' % ', '.join(unplaced)
    if carriers:
        # (a cell named from the raw triangles can end with none: welding and
        # decimation move the centres of its few slivers over its edge)
        empty = [c for c in carriers if mname[c['key']] not in gathered]
        if empty:
            carriers = [c for c in carriers if mname[c['key']] in gathered]
            print('collision by area: %d cells with no triangles left dropped (%s)' % (
                len(empty), ', '.join(mname[c['key']] for c in empty)))
    entries = []
    worst = 0.0
    carrier_bounds = {}
    for key in sorted(gathered):
        v, t, m = collide.join(gathered[key])
        bound = collide.bound_for(v, t, m, identity_matrix(), col.get('max_polys', collide.MAX_POLYS))
        for mesh in (bound['children'] if bound['type'] == 12 else [bound]):
            worst = max(worst, collide.check_bvh(mesh, rays=40))
        entries.append((key, bound))
        blo, bhi = v.min(axis=0), v.max(axis=0)
        centre = (blo + bhi) / 2
        carrier_bounds[key] = (blo, bhi, centre, float(np.linalg.norm(v - centre, axis=1).max()))
    with_bounds = {name for name, _ in entries}
    dict_names = []
    total_col = 0
    for dname, group in dictionaries(entries, prefix, cfg.get('bounds')):
        assert len(dname) + 4 <= 24, dname
        body, xflags = build_dictionary(group)
        n = bounds360.check(body, xflags, group)
        pack(xlzx, XBD_TYPE, xflags, body, os.path.join(out, dname + '.xbd'))
        dict_names.append(dname)
        total_col += len(body)
        print('  %s.xbd: %d bounds, %.2f MiB, flags %08X' % (dname, n, len(body) / 1048576.0, xflags))
    print('collision: %d bounds in %d dictionaries, %d triangles from %d (no collision: %s), %.2f MiB, trees '
          'walked: worst %.4f m' % (len(entries), len(dict_names), tri_out, tri_in, ', '.join(dropped) or 'nothing',
                                    total_col / 1048576.0, worst))
    # the carriers' drawables (one triangle far below their cells)
    if carriers:
        donor_name = min(used, key=lambda m: os.path.getsize(os.path.join(src, m + '.wdr')))
        donor = rsc5.Resource.pc(os.path.join(src, donor_name + '.wdr'))
        for c in carriers:
            key = mname[c['key']]
            assert key in carrier_bounds, key
            flags, body = drawer.carrier(donor, carrier_bounds[key])
            check_carrier(body, flags, carrier_bounds[key])
            if 'models' not in skip:
                pack(xlzx, XDR_TYPE, flags, body, os.path.join(out, key + '.xdr'))
        print('carriers: %d drawables (from %s), every one read back' % (len(carriers), donor_name))

    # ---- the models' lines, the pieces, the destination
    # (flag 0x20000 - a movable object, as the stock props with dictionary
    # bounds have it, rubbish.ide's BM_plank01 0xC20000 - is cleared from a
    # model with collision (v1.7.42). With it the game gives the bound's
    # archetype an object's type flags (SetPhysics, sub_82360060: +16 0x80),
    # which its ground probes do not look for (GET_GROUND_Z_FOR_3D_COORD asks
    # for the types 6): v1.7.34-41's pieces were driven on while no probe
    # found them, the teleport waited for ground that never came, the parked
    # cars stood in the air. Without it the archetype is a building's (+16
    # 0x4), as the mods' PC lines have them. It does not get a model its
    # bound either way: the dictionary's loading does (see bounds above).
    # The trainer sets the kind again before the dictionaries load (its menu
    # can ask for movable objects, the old way).
    with_flag = 0
    with open(os.path.join(out, 'objs.ide'), 'w', newline='\r\n') as f:
        def write(line, name):
            nonlocal with_flag
            line = list(line)
            line[0] = name
            if name in with_bounds:
                with_flag += int(line[3]) & DYNAMIC_FLAG != 0
                line[3] = str(int(line[3]) & ~DYNAMIC_FLAG)
            f.write(', '.join(line) + '\n')

        for m in used:
            line = list(models[m])
            line[1] = tname[models[m][1].lower()]
            line[-1] = 'null'
            if m in tiles_of:
                for t in tiles_of[m][1]:
                    lo_, hi_, centre, radius = t['bounds']
                    line[5:15] = ['%.4f' % x for x in (*lo_, *hi_, *centre, radius)]
                    write(line, mname[t['key']])
                continue
            write(line, mname[m])
        carrier_txd = tname[models[used[0]][1].lower()]
        for c in carriers:
            key = mname[c['key']]
            lo_, hi_, centre, radius = carrier_bounds[key]
            line = [key, carrier_txd, str(CARRIER_DRAW), str(CARRIER_FLAGS), '0'] + \
                ['%.4f' % x for x in (*lo_, *hi_, *centre, radius)] + ['null']
            write(line, key)
    print('model lines: %d, %d with collision as buildings (%d had the movable flag 0x%X in the mod; their '
          'collision from %s)' % (len(game_names), len(with_bounds), with_flag, DYNAMIC_FLAG,
                                  ', '.join(d + '.xbd' for d in dict_names)))
    with open(os.path.join(out, 'place.txt'), 'w') as f:
        f.write('# model qx qy qz qw x y z collision (the mod\'s coordinates; collision 2: the game\'s own model)\n')
        count = collections.Counter()
        for p in placed:
            q, pos = p['q'], p['pos']
            if p['stock']:
                parts, kind = [(p['model'], (0.0, 0.0, 0.0), 2)], 'stock'
            elif p['model'] in tiles_of:
                parts = [(mname[t['key']], t['origin'], 1 if mname[t['key']] in with_bounds else 0)
                         for t in tiles_of[p['model']][1]]
                kind = 'tiles'
            else:
                name = mname[p['model']]
                parts, kind = [(name, (0.0, 0.0, 0.0), 1 if name in with_bounds else 0)], 'models'
            for name, o, c in parts:
                f.write('%s %.6f %.6f %.6f %.6f %.3f %.3f %.3f %d\n' % (
                    name, q[0], q[1], q[2], q[3], pos[0] + o[0], pos[1] + o[1], pos[2] + o[2], c))
                count[kind] += 1
        for c in carriers:
            o = c['origin']
            f.write('%s 0.000000 0.000000 0.000000 1.000000 %.3f %.3f %.3f 1\n' % (mname[c['key']], o[0], o[1], o[2]))
            count['carriers'] += 1
    print('pieces: %s' % ', '.join('%d %s' % (n, k) for k, n in sorted(count.items())))
    hm, hlo, cell = heights or height_map(list(piece_files.values()), keep=keep)
    runs = best_runs(hm, hlo, cell, cfg['sea'])
    print('arrival runs (length m, x, y, z, heading deg):')
    for r in runs:
        print('  %.0f  %.1f %.1f %.1f  %.0f' % (r[0], r[1], r[2], r[3], math.degrees(r[4])))
    arrive = cfg.get('arrive') or [runs[0][1], runs[0][2], runs[0][3], math.degrees(runs[0][4]), runs[0][0]]
    life = cfg.get('life', {})
    with open(os.path.join(out, 'map.txt'), 'w') as f:
        f.write('name %s\nlabel %s\ndate %s\n' % (cfg['name'], cfg['label'], ' '.join(str(v) for v in cfg['date'])))
        f.write('shift %.1f %.1f\nsea %.2f\n' % (cfg['to'][0] - cfg['from'][0], cfg['to'][1] - cfg['from'][1], cfg['sea']))
        f.write('arrive %.2f %.2f %.2f %.1f %.0f\n' % tuple(arrive))
        f.write('area %.1f %.1f %.1f %.1f\n' % (lo[0], lo[1], hi[0], hi[1]))
        f.write('pieces %d\n' % sum(count.values()))
        f.write('prefix %s\n' % prefix)  # (the installer drops files with it this set no longer has)
        f.write('bounds %s\n' % ' '.join(dict_names))  # (the trainer asks for them with the models)
        # streamed by distance (v1.7.38): the trainer makes the pieces within
        # this of the player and deletes those farther, their models let go
        # (Wake Island's 1,600 pieces at once filled the game's streaming
        # memory, 260 of 268 MB, and left the car in the sea)
        if cfg.get('stream'):
            f.write('stream %.0f\n' % cfg['stream'])
        # its life (v1.7.37): the game's ped models that belong there, how
        # many to keep about the player, the weather (the game's id), the
        # trainer's ambience by name
        if life.get('peds'):
            f.write('peds %s\n' % ' '.join(n.lower() for n in life['peds']))
            f.write('wander %d\n' % life.get('wander', 8))
        if 'weather' in life:
            f.write('weather %d\n' % life['weather'])
        if life.get('ambience'):
            f.write('ambience %s\n' % life['ambience'])
        # the spots the menu teleports to (v1.7.40: the mod's coordinates, a
        # heading, a label), one of them perhaps the player's safehouse there
        # (a blip at its door, a save point at its bed)
        for sp in cfg.get('spots', []):
            x, y, z, heading = sp['stand']
            f.write('spot %s %.2f %.2f %.2f %.1f %s\n' % (sp['name'], x, y, z, heading, sp.get('label', sp['name'])))
            if sp.get('door') and sp.get('bed'):
                f.write('safehouse %s %.2f %.2f %.2f %.2f %.2f %.2f\n' % ((sp['name'],) + tuple(sp['door']) + tuple(sp['bed'])))
    if creatures:
        with open(os.path.join(out, 'creatures.txt'), 'w') as f:
            f.write('# the place\'s creatures (creature.py): kind, size, how many about; each part\'s model, pivot, axis, swing\n')
            for cname, lines_, count in creatures:
                for line in lines_:
                    w = line.split()
                    if w[0] == 'creature':
                        w += ['count', str(count)]
                    elif w[0] == 'part':
                        w[2] = mname.get(w[2].lower(), w[2])
                    f.write(('  ' if w[0] == 'part' else '') + ' '.join(w) + '\n')
    if vehicles:
        with open(os.path.join(out, 'vehicles.txt'), 'w') as f:
            f.write('# model x y z heading (the mod\'s coordinates): the vehicles the mod parks\n')
            for v in vehicles:
                f.write('%s %.3f %.3f %.3f %.1f\n' % (v['model'], v['pos'][0], v['pos'][1], v['pos'][2], v['heading']))
    # the ground (ground.bin: the collision's walkable surface on a 4 m grid,
    # for the trainer's own probe - the game's ground probe does not see the
    # pieces (log 101: "the ground under the arrival not found"); centimetres,
    # -32768 none)
    step = 2
    gh = hm[::step, ::step].copy()
    for dy in range(step):
        for dx in range(step):
            part = hm[dy::step, dx::step]
            gh[:part.shape[0], :part.shape[1]] = np.fmax(gh[:part.shape[0], :part.shape[1]], part)
    cm = np.where(np.isfinite(gh), np.clip(np.round(gh * 100), -32767, 32767), -32768).astype('<i2')
    with open(os.path.join(out, 'ground.bin'), 'wb') as f:
        f.write(b'GRND' + struct.pack('<ffIII', float(hlo[0]), float(hlo[1]), int(cell * step), gh.shape[1], gh.shape[0]))
        f.write(cm.tobytes())
    print('ground: %dx%d cells of %d m (%d KiB)' % (gh.shape[1], gh.shape[0], cell * step, (16 + cm.size * 2) >> 10))
    print('wrote %s' % out)


if __name__ == '__main__':
    main()
