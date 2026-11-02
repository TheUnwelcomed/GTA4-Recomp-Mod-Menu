"""A PC map mod as a destination for the DeLorean's time travel: its models,
textures and collision as console resources, ready for the installer to put
into vehicles.img, with the lines and placements the installer and the
trainer need. Nothing here is part of the city: the trainer places the
pieces as frozen objects when the destination is chosen, and deletes them
when the trip home is made (one destination loaded at a time).

usage: python3 -I mapset.py <set.json> <source dir> <xdr dir> <template.xtd.bin:flags> <xlzx> <out dir>
                             [--skip-textures] [--skip-models]
  set.json     the set's settings (tools/maps/sets/<name>.json)
  source dir   the mod's pieces: <model>.wdr / .wtd / .wbn, the .ide and the
               .wpl files the set names
  xdr dir      the models converted by mapdraw.py (<model>.xdr.bin, flags.txt)
  template     a decompressed stock .xtd with a texture with mips
  xlzx         the resource packer (pc2x360/xlzx.cpp)

Writes to <out dir>:
  <prefix><model>.xdr, <prefix><txd>.xtd   models and texture dictionaries
  gtxd.xbd     the collision: a bound dictionary named after the .ide the
               installer adds the models to (common.rpf's gtxd.ide, which
               gta.dat already loads), keyed by model name
  objs.ide     the models' lines for that .ide
  place.txt    one line per piece for the trainer
  map.txt      the destination: label, date, the shift from the mod's
               coordinates, its sea level, the arrival, its area

Every model is renamed <prefix><name> (no clash with the game's own names).
WPL rotations are stored inverted (checked: Gator Keys' rotated pad lines up
with its collision only that way), so place.txt carries the true rotation.
Collision comes in world space, one .wbn per model; a piece whose model is
placed unrotated moves into the model's space, the rest (rotated pieces,
collision with no model) join the set's carrier model's composite.
"""
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
import maptex  # noqa: E402
import rsc5  # noqa: E402

XTD_TYPE, XDR_TYPE, XBD_TYPE = 7, 109, 32


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


def merge(carrier, extra):
    """The carrier's composite with `extra` bounds (already in its space) as
    more children, its box, centroid and radius grown to hold them."""
    if carrier['type'] != 12:
        carrier = dict(type=12, radius=carrier['radius'], max=list(carrier['max']), min=list(carrier['min']),
                       centroid=list(carrier['centroid']), cg=list(carrier['cg']), dist=list(carrier['dist']),
                       margin=list(carrier['margin']), children=[carrier],
                       matrices=[identity_matrix()], boxes=[(list(carrier['min']), list(carrier['max']))])
    for e in extra:
        kids = e['children'] if e['type'] == 12 else [e]
        for k in kids:
            carrier['children'].append(k)
            carrier['matrices'].append(identity_matrix())
            carrier['boxes'].append((list(k['min']), list(k['max'])))
    lo = [min(b[0][i] for b in carrier['boxes']) for i in range(3)]
    hi = [max(b[1][i] for b in carrier['boxes']) for i in range(3)]
    carrier['min'], carrier['max'] = lo, hi
    carrier['centroid'] = [(lo[i] + hi[i]) / 2 for i in range(3)]
    carrier['cg'] = list(carrier['centroid'])
    carrier['radius'] = math.sqrt(sum((hi[i] - lo[i]) ** 2 for i in range(3))) / 2
    return carrier


def identity_matrix():
    return struct.pack('<16f', 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


def build_dictionary(entries):
    """bounds360.build_dictionary with pages sized for big meshes: every
    array inside one 16-page load chunk, no more than 100 chunks."""
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
    shift = 4
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


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    skip = {a[len('--skip-'):] for a in sys.argv[1:] if a.startswith('--skip-')}  # textures, models
    set_path, src, xdr_dir, tmpl_arg, xlzx, out = args[:6]
    cfg = json.load(open(set_path))
    prefix = cfg['prefix']
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
        name = prefix + txd
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
        body = open(os.path.join(xdr_dir, m + '.xdr.bin'), 'rb').read()
        pack(xlzx, XDR_TYPE, flags_of[m], body, os.path.join(out, prefix + m + '.xdr'))
        total_xdr += len(body)
    print('models: %.1f MiB, %d drawables' % (total_xdr / 1048576.0, len(models)))

    # ---- collision
    carrier = cfg['carrier']
    entries, joined, pieces_world = [], [], []
    for f in sorted(os.listdir(src)):
        if not f.endswith('.wbn'):
            continue
        name = f[:-4].lower()
        b = read_piece(os.path.join(src, f))
        pieces_world.append(read_piece(os.path.join(src, f)))
        p = where.get(name)
        if p and abs(p['q'][3]) > 0.99999 and name != carrier:
            if b['type'] == 12 and len(b['children']) == 1:
                b = b['children'][0]
            bounds360.translate(b, [-v for v in p['pos']])
            entries.append((prefix + name, b))
        elif name == carrier:
            entries.append((prefix + name, ('carrier', b)))
        else:
            joined.append((name, b))
    cpos = where[carrier]['pos']
    assert abs(where[carrier]['q'][3]) > 0.99999, 'the carrier must be placed unrotated'
    for i, (name, b) in enumerate(entries):
        if isinstance(b, tuple):
            cb = b[1]
            extra = [x for _, x in joined]
            cb = merge(cb, extra)
            bounds360.translate(cb, [-v for v in cpos])
            entries[i] = (name, cb)
    body, xflags = build_dictionary(entries)
    n = bounds360.check(body, xflags, entries)
    pack(xlzx, XBD_TYPE, xflags, body, os.path.join(out, cfg['bounds'] + '.xbd'))
    print('collision: %d bounds (%s joined to %s), %.1f MiB, flags %08X' % (
        n, ', '.join(nm for nm, _ in joined), carrier, len(body) / 1048576.0, xflags))
    with_bounds = {name for name, _ in entries}

    # ---- the models' lines, the pieces, the destination
    with open(os.path.join(out, 'objs.ide'), 'w', newline='\r\n') as f:
        for m, fields in sorted(models.items()):
            line = list(fields)
            line[0] = prefix + m
            line[1] = prefix + fields[1].lower()
            line[-1] = 'null'
            f.write(', '.join(line) + '\n')
    with open(os.path.join(out, 'place.txt'), 'w') as f:
        f.write('# model qx qy qz qw x y z collision (the mod\'s coordinates)\n')
        for p in placed:
            q, pos = p['q'], p['pos']
            f.write('%s %.6f %.6f %.6f %.6f %.3f %.3f %.3f %d\n' % (
                prefix + p['model'], q[0], q[1], q[2], q[3], pos[0], pos[1], pos[2],
                1 if prefix + p['model'] in with_bounds else 0))
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
        f.write('pieces %d\n' % len(placed))
    print('wrote %s' % out)


if __name__ == '__main__':
    main()
