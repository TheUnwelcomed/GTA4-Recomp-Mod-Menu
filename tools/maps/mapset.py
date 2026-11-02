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
               .wpl files the set names
  xdr dir      the models converted by mapdraw.py (<model>.xdr.bin, flags.txt)
  template     a decompressed stock .xtd with a texture with mips
  xlzx         the resource packer (pc2x360/xlzx.cpp)
  --draw       what mapdraw.py converted the models with: needed for a set
               with tiles (its big models cut up here)

Writes to <out dir>:
  <prefix><model>.xdr, <prefix><txd>.xtd   models and texture dictionaries
  <prefix>col.xbd   the collision: a bound dictionary keyed by model name
               (collide.py: the mod's meshes welded, cut below the shallows,
               decimated and split into BVH meshes with trees of their own;
               16 KiB pages). The game links a model to its bound only when
               the dictionary holding it loads (its key the model's name
               hash), whatever the dictionary is called; at boot it loads
               them all only without a good cache, so the trainer asks for
               the set's with its models (v1.7.34; v1.7.33's was gtxd.xbd,
               named after the .ide, on the wrong idea that the .ide's name
               picks it)
  objs.ide     the models' lines for that .ide
  place.txt    one line per piece for the trainer
  map.txt      the destination: label, date, the shift from the mod's
               coordinates, its sea level, the arrival, its area, the
               prefix (the installer drops image files with it that the set
               no longer has), its collision dictionary's name

Every model is renamed <prefix><name> (no clash with the game's own names),
shortened where that is over 20 characters: the game registers an image's
files by the first 24 characters of the name, the extension among them (a
longer one is never registered: v1.7.30's gk_gtr_deco_polgrnda018).
WPL rotations are stored inverted (checked: Gator Keys' rotated pad lines up
with its collision only that way), so place.txt carries the true rotation.
Collision comes in world space, one .wbn per model; each moves into its
model's space (its placement's rotation undone). Tiles (v1.7.32): a model
over the set's tiles.radius across, placed once and unrotated, is cut into
tiles of tiles.cell metres (mapdraw.plan_tiles), each a model of its own
named <model>_<n>, its drawable converted here, its collision the piece's
triangles over its cell, all in its own space round its origin; the game
neither draws nor collides with a model several hundred metres across placed
as an object (v1.7.31's island: its terrain and roads missing, its
buildings there).
"""
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
import maptex  # noqa: E402
import rsc5  # noqa: E402

XTD_TYPE, XDR_TYPE, XBD_TYPE = 7, 109, 32
DYNAMIC_FLAG = 0x20000  # (an IDE flag: a movable object's archetype for the model's bound)
NAME_LIMIT = 20  # a file's name with its 4-character extension within the game's 24


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


def mesh_points(b):
    if b['type'] == 12:
        return np.concatenate([mesh_points(c) for c in b['children']])
    v = np.frombuffer(b['vertices'], '<i2').reshape(-1, 3).astype(np.float64)
    return v * np.array(b['quantum']) + np.array(b['offset'])


def identity_matrix():
    return struct.pack('<16f', 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


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
    size = len(a.buf)
    count = (size + page - 1) // page
    assert count <= 0x7FF, (size, page)
    assert len(batconv.load_chunks(count, page)) <= 100
    body = bytes(a.buf) + b'\xCD' * (count * page - size)
    flags = 0xC0000000 | count | (shift << 11)
    assert rsc5.seg_sizes(flags) == (count * page, 0)
    return body, flags


# ------------------------------------------------------------------ arrival
def height_map(pieces, cell=2.0):
    """The highest walkable collision surface on a grid (NaN: none): every
    upward-facing triangle sampled at least twice a cell along its edges."""
    pts = []
    for b in pieces:
        stack = [b]
        while stack:
            x = stack.pop()
            if x['type'] == 12:
                stack += x['children']
                continue
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
                a, b_, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
                edge = np.maximum(np.maximum(np.linalg.norm(b_ - a, axis=1), np.linalg.norm(c - a, axis=1)),
                                  np.linalg.norm(c - b_, axis=1))
                steps = np.clip(np.ceil(edge / (cell / 2)), 1, 200).astype(np.int64)
                for k in np.unique(steps):
                    sel = steps == k
                    g = np.array([(i / k, j / k) for i in range(k + 1) for j in range(k + 1 - i)])
                    aa, bb, cc = a[sel], b_[sel], c[sel]
                    p_ = aa[:, None, :] + (bb - aa)[:, None, :] * g[None, :, 0:1] + (cc - aa)[:, None, :] * g[None, :, 1:2]
                    pts.append(p_.reshape(-1, 3))
    p = np.concatenate(pts)
    lo = p[:, :2].min(0)
    ij = ((p[:, :2] - lo) / cell).astype(np.int64)
    w, h = ij.max(0) + 1
    hm = np.full((h, w), -np.inf)
    np.maximum.at(hm, (ij[:, 1], ij[:, 0]), p[:, 2])
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


def check_tile(body, flags, donor, tile):
    """A tile's drawable read back: the triangles planned for it, every index
    inside its buffer, every position one of the mod's moved to the tile."""
    x = rsc5.Resource(body, flags, True, XDR_TYPE, 'tile')
    xd = rsc5.read_drawable(x, 0)
    tri = sum(g['index_count'] // 3 for m in xd['lods'][0] for g in m['geometries'])
    assert tri == tile['triangles'], (tri, tile['triangles'])
    pc = np.unique(mapdraw.pc_triangles(donor).reshape(-1, 3), axis=0)
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


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    draw_arg = next((a[len('--draw='):] for a in sys.argv[1:] if a.startswith('--draw=')), None)
    skip = {a[len('--skip-'):] for a in sys.argv[1:] if a.startswith('--skip-')}  # textures, models
    set_path, src, xdr_dir, tmpl_arg, xlzx, out = args[:6]
    cfg = json.load(open(set_path))
    prefix = cfg['prefix']
    bounds_name = cfg.get('bounds') or prefix + 'col'  # (within the 24 characters an image name may have)
    assert len(bounds_name) + 4 <= 24, bounds_name
    os.makedirs(out, exist_ok=True)
    tmpl_path, tmpl_flags = tmpl_arg.rsplit(':', 1)
    tmpl = maptex.Template(rsc5.Resource(open(tmpl_path, 'rb').read(), int(tmpl_flags, 16), True, XTD_TYPE, tmpl_path))
    ide = read_ide(os.path.join(src, cfg['ide']))
    models = {f[0].lower(): f for f in ide}
    by_hash = {batconv.joaat(m): m for m in models}
    placed = []
    for w in cfg['wpl']:
        for inst in read_wpl(os.path.join(src, w)):
            if inst['hash'] in by_hash:
                placed.append(dict(inst, model=by_hash[inst['hash']]))
    print('%d models, %d pieces placed (%s)' % (len(models), len(placed), ', '.join(cfg['wpl'])))
    where = {p['model']: p for p in placed}
    # ---- tiles: the big models, placed once and unrotated, cut up
    tcfg = cfg.get('tiles')
    tiles_of = {}
    if tcfg:
        for m, fields in sorted(models.items()):
            p = where.get(m)
            if float(fields[14]) <= tcfg['radius'] or p is None or abs(p['q'][3]) < 0.99999:
                continue
            donor = rsc5.Resource.pc(os.path.join(src, m + '.wdr'))
            zmin = cfg['sea'] - tcfg.get('below_sea', 6.0) - p['pos'][2]
            ts = mapdraw.plan_tiles(donor, tcfg['cell'], zmin)
            x0 = ts[0]['lo'][0] - ts[0]['cell'][0] * tcfg['cell']
            y0 = ts[0]['lo'][1] - ts[0]['cell'][1] * tcfg['cell']
            for k, t in enumerate(ts):
                t['key'] = '%s_%d' % (m, k)
                t['grid'] = (x0, y0, tcfg['cell'])
            tiles_of[m] = (donor, ts)
        print('tiles: %s' % ', '.join('%s %d' % (m, len(ts)) for m, (_, ts) in sorted(tiles_of.items())))
    # the game's names for the models (a tiled one's tiles) and the texture dictionaries
    game_names = [m for m in models if m not in tiles_of] + [t['key'] for _, ts in tiles_of.values() for t in ts]
    mname = short_names(game_names, prefix, cfg.get('rename'))
    tname = short_names({f[1].lower() for f in models.values()}, prefix, cfg.get('rename'))
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
    for m, fields in models.items():
        txd = fields[1].lower()
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
        flags, body, report = maptex.build(tmpl, chosen, side=cfg.get('tex', maptex.MAP_SIDE),
                                           detail=cfg.get('detail', maptex.DETAIL_SIDE))
        verdict, worst = maptex.check(body, flags, chosen)
        assert worst < 12.0, (txd, worst)
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
    for m in sorted(models):
        if 'models' in skip:
            break
        if m in tiles_of:
            continue
        body = open(os.path.join(xdr_dir, m + '.xdr.bin'), 'rb').read()
        pack(xlzx, XDR_TYPE, flags_of[m], body, os.path.join(out, mname[m] + '.xdr'))
        total_xdr += len(body)
    if tiles_of:
        assert draw_arg, 'a set with tiles needs --draw (what mapdraw.py converted the models with)'
        dtmpl_arg, shader_dir = draw_arg.split(',', 1)
        dtmpl_path, dtmpl_flags = dtmpl_arg.rsplit(':', 1)
        dtmpl = rsc5.Resource(open(dtmpl_path, 'rb').read(), int(dtmpl_flags, 16), True, XDR_TYPE, dtmpl_path)
        sources = [mapdraw.load_console(q) for q in sorted(glob.glob(os.path.join(shader_dir, '*.bin')))]
        for k, fb in mapdraw.MAP_FALLBACK.items():
            batconv.SPS_FALLBACK.setdefault(k, fb)
        tiles_done = 0
        for m, (donor, ts) in sorted(tiles_of.items()):
            for t in ts:
                conv = mapdraw.MapDrawable(dtmpl, donor, sources, log=lambda msg: None, tile=t)
                flags, body = conv.run()
                t['bounds'] = conv.tile_bounds()
                check_tile(body, flags, donor, t)
                if 'models' not in skip:
                    pack(xlzx, XDR_TYPE, flags, body, os.path.join(out, mname[t['key']] + '.xdr'))
                total_xdr += len(body)
                tiles_done += 1
        print('tiles: %d drawables, every one read back (its triangles, indices and positions)' % tiles_done)
    print('models: %.1f MiB, %d drawables' % (total_xdr / 1048576.0, len(game_names)))

    # ---- collision: each .wbn (world space) welded, cut below the shallows,
    # decimated within the set's error, moved into its model's space (a tiled
    # model's: each triangle into its tile's) and split into BVH meshes
    col = cfg.get('collision', {})
    floor_z = cfg['sea'] - col.get('seabed', 3.0)

    def error_for(name):
        for pat, e in col.get('error', {}).items():
            if pat != '*' and fnmatch.fnmatch(name, pat):
                return e
        return col.get('error', {}).get('*', 0.2)

    gathered, pieces_world, dropped, unplaced = {}, [], [], []
    tri_in = tri_out = 0
    for f in sorted(os.listdir(src)):
        if not f.endswith('.wbn'):
            continue
        name = f[:-4].lower()
        b = read_piece(os.path.join(src, f))
        pieces_world.append(b)
        if any(fnmatch.fnmatch(name, pat) for pat in col.get('drop', [])):
            dropped.append(name)
            continue
        p = where.get(name)
        if p is None:
            unplaced.append(name)
            continue
        v, t, m = collide.soup(b)
        tri_in += len(t)
        v, t, m = collide.weld(v, t, m, col.get('weld', 0.02))
        v, t, m = collide.cut_below(v, t, m, floor_z)
        v, t, m = collide.decimate(v, t, m, error_for(name))
        tri_out += len(t)
        local = to_model(v, p)
        if name in tiles_of:
            ts = tiles_of[name][1]
            which = tile_of(local[t].mean(axis=1)[:, :2], ts)
            for k in np.unique(which):
                sel = which == k
                tv, tt, tm = collide.compact(local, t[sel], m[sel])
                gathered.setdefault(mname[ts[k]['key']], []).append((tv - np.array(ts[k]['origin']), tt, tm))
        else:
            gathered.setdefault(mname[name], []).append((local, t, m))
    assert not unplaced, 'collision with no placed model: %s (drop it)' % ', '.join(unplaced)
    entries = []
    worst = 0.0
    for key in sorted(gathered):
        v, t, m = collide.join(gathered[key])
        bound = collide.bound_for(v, t, m, identity_matrix(), col.get('max_polys', collide.MAX_POLYS))
        for mesh in (bound['children'] if bound['type'] == 12 else [bound]):
            worst = max(worst, collide.check_bvh(mesh, rays=40))
        entries.append((key, bound))
    body, xflags = build_dictionary(entries)
    n = bounds360.check(body, xflags, entries)
    pack(xlzx, XBD_TYPE, xflags, body, os.path.join(out, bounds_name + '.xbd'))
    print('collision: %d bounds, %d triangles from %d (no collision: %s), %.2f MiB, flags %08X, trees walked: '
          'worst %.4f m' % (n, tri_out, tri_in, ', '.join(dropped), len(body) / 1048576.0, xflags, worst))
    with_bounds = {name for name, _ in entries}

    # ---- the models' lines, the pieces, the destination
    # (flag 0x20000 on a model with collision, as the stock props with
    # dictionary bounds have it - rubbish.ide's BM_plank01 0xC20000: the game
    # makes its archetype a movable object's, its mass from the bound's
    # volume (SetPhysics, sub_82360060), where without it a fixed one; the
    # pieces are frozen either way. It does not get a model its bound: the
    # dictionary's loading does (see bounds above; v1.7.33's island had the
    # flag and still no collision, its dictionary never loaded)
    with_flag = 0
    with open(os.path.join(out, 'objs.ide'), 'w', newline='\r\n') as f:
        def write(line, name):
            nonlocal with_flag
            line = list(line)
            line[0] = name
            if name in with_bounds:
                line[3] = str(int(line[3]) | DYNAMIC_FLAG)
                with_flag += 1
            f.write(', '.join(line) + '\n')

        for m, fields in sorted(models.items()):
            line = list(fields)
            line[1] = tname[fields[1].lower()]
            line[-1] = 'null'
            if m in tiles_of:
                for t in tiles_of[m][1]:
                    lo, hi, centre, radius = t['bounds']
                    line[5:15] = ['%.4f' % x for x in (*lo, *hi, *centre, radius)]
                    write(line, mname[t['key']])
                continue
            write(line, mname[m])
    print('model lines: %d with flag 0x%X (their collision from %s.xbd)' % (with_flag, DYNAMIC_FLAG, bounds_name))
    with open(os.path.join(out, 'place.txt'), 'w') as f:
        f.write('# model qx qy qz qw x y z collision (the mod\'s coordinates)\n')
        count = 0
        for p in placed:
            q, pos = p['q'], p['pos']
            parts = [(mname[p['model']], (0.0, 0.0, 0.0))] if p['model'] not in tiles_of else \
                [(mname[t['key']], t['origin']) for t in tiles_of[p['model']][1]]
            for name, o in parts:
                f.write('%s %.6f %.6f %.6f %.6f %.3f %.3f %.3f %d\n' % (
                    name, q[0], q[1], q[2], q[3], pos[0] + o[0], pos[1] + o[1], pos[2] + o[2],
                    1 if name in with_bounds else 0))
                count += 1
    allpts = np.concatenate([mesh_points(b) for b in pieces_world])
    lo, hi = allpts[:, :2].min(0), allpts[:, :2].max(0)
    hm, hlo, cell = height_map(pieces_world)
    runs = best_runs(hm, hlo, cell, cfg['sea'])
    print('arrival runs (length m, x, y, z, heading deg):')
    for r in runs:
        print('  %.0f  %.1f %.1f %.1f  %.0f' % (r[0], r[1], r[2], r[3], math.degrees(r[4])))
    arrive = cfg.get('arrive') or [runs[0][1], runs[0][2], runs[0][3], math.degrees(runs[0][4]), runs[0][0]]
    with open(os.path.join(out, 'map.txt'), 'w') as f:
        f.write('name %s\nlabel %s\ndate %s\n' % (cfg['name'], cfg['label'], ' '.join(str(v) for v in cfg['date'])))
        f.write('shift %.1f %.1f\nsea %.2f\n' % (cfg['to'][0] - cfg['from'][0], cfg['to'][1] - cfg['from'][1], cfg['sea']))
        f.write('arrive %.2f %.2f %.2f %.1f %.0f\n' % tuple(arrive))
        f.write('area %.1f %.1f %.1f %.1f\n' % (lo[0], lo[1], hi[0], hi[1]))
        f.write('pieces %d\n' % count)
        f.write('prefix %s\n' % prefix)  # (the installer drops files with it this set no longer has)
        f.write('bounds %s\n' % bounds_name)  # (the trainer asks for it with the models)
    print('wrote %s' % out)


if __name__ == '__main__':
    main()
