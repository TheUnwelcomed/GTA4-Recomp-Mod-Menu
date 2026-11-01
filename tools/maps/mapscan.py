"""Survey a GTA IV PC map mod: its archives, definitions and placements.

usage: python3 -I mapscan.py <aes_key.bin> <name> <map dir> [<map dir> ...] [--json out.json]

Every .img under the map dirs is read (img3, the TOC decrypted when needed),
with the .ide and binary .wpl files beside it and the stream placements
(.wpl entries) inside the archives. Reported:
  - entries by kind (wdr, wdd, wft, wtd, wbn, wbd, wad, wpl...) and sizes;
  - instances placed (outside and streamed), their extents (x, y, z), and
    the extents grown by each model's bounding sphere (from its ide line);
  - models placed that no ide defines, defined ones never placed;
  - texture dictionaries the ide names that no archive holds;
  - car generators (the wpl's cars section) and their model hashes;
  - which of the game's traffic-path areas (8 x 8, 750 m, x and y in
    -3000..3000) the map covers.
The binary IPL (.wpl, version 3): a 68-byte header of 17 counts (1 inst,
4 cars, the rest unused by these mods), 48-byte instances (position,
rotation quaternion, model hash, flags, lod index...), 56-byte car
generators, and GIMS's trailer.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))  # (img3.py)
import img3  # noqa: E402

PATH_CELL, PATH_MIN = 750.0, -3000.0


def joaat(s):
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def read_wpl(data):
    if len(data) < 68:
        return [], []
    head = struct.unpack_from('<17I', data, 0)
    if head[0] != 3:
        return [], []
    n_inst, n_cars = head[1], head[4]
    inst, cars = [], []
    off = 68
    for i in range(n_inst):
        if off + 48 > len(data):
            break
        x, y, z, qx, qy, qz, qw, h, flags, lod = struct.unpack_from('<7fIIi', data, off)
        inst.append(dict(pos=(x, y, z), q=(qx, qy, qz, qw), hash=h, flags=flags, lod=lod))
        off += 48
    for i in range(n_cars):
        if off + 56 > len(data):
            break
        x, y, z = struct.unpack_from('<3f', data, off)
        h = struct.unpack_from('<I', data, off + 24)[0]
        cars.append(dict(pos=(x, y, z), hash=h))
        off += 56
    return inst, cars


def read_ide(text):
    models, sec = {}, None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        low = line.lower()
        if low in ('objs', 'tobj', 'tree', 'anim', 'mlo', '2dfx', 'amat', 'txdp', 'path', 'hier', 'cars',
                   'peds', 'weap', 'end'):
            sec = None if low == 'end' else low
            continue
        if sec in ('objs', 'tobj', 'tree', 'anim'):
            f = [p.strip() for p in line.split(',')]
            try:
                if sec == 'anim':  # name, txd, anim dict, draw dist, flags, ..., bbox, sphere
                    name, txd = f[0], f[1]
                    radius = float(f[-2])
                else:
                    name, txd = f[0], f[1]
                    radius = float(f[-2])
            except (ValueError, IndexError):
                continue
            models[name.lower()] = dict(txd=txd.lower(), radius=radius, section=sec)
    return models


def scan(key, dirs):
    archives, ides, wpls = [], [], []
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in sorted(files):
                low = f.lower()
                p = os.path.join(root, f)
                if low.endswith('.img'):
                    archives.append(p)
                elif low.endswith('.ide'):
                    ides.append(p)
                elif low.endswith('.wpl'):
                    wpls.append(p)
    report = dict(archives=[], kinds={}, sizes={}, entries=set())
    models = {}
    for p in ides:
        for name, m in read_ide(open(p, 'rb').read().decode('latin-1')).items():
            m['ide'] = os.path.basename(p)
            models[name] = m
    inst, cars = [], []
    for p in wpls:
        i, c = read_wpl(open(p, 'rb').read())
        for x in i:
            x['src'] = os.path.basename(p)
        inst += i
        cars += c
    for p in archives:
        with open(p, 'rb') as fh:
            try:
                items = img3.read_toc(fh, key)
            except SystemExit as e:
                report['archives'].append((os.path.basename(p), 'unreadable: %s' % e))
                continue
            report['archives'].append((os.path.basename(p), len(items)))
            for it in items:
                name = it['name'].lower()
                ext = name.rsplit('.', 1)[-1] if '.' in name else ''
                report['kinds'][ext] = report['kinds'].get(ext, 0) + 1
                report['sizes'][ext] = report['sizes'].get(ext, 0) + it['size']
                report['entries'].add(name)
                if ext == 'wpl':
                    fh.seek(it['offset'])
                    i, c = read_wpl(fh.read(it['size']))
                    for x in i:
                        x['src'] = os.path.basename(p) + ':' + name
                    inst += i
                    cars += c
    by_hash = {joaat(n): n for n in models}
    placed = {}
    unknown = {}
    lo, hi = [1e9] * 3, [-1e9] * 3
    glo, ghi = [1e9] * 2, [-1e9] * 2
    for x in inst:
        name = by_hash.get(x['hash'])
        if name is None:
            unknown[x['hash']] = unknown.get(x['hash'], 0) + 1
        else:
            placed[name] = placed.get(name, 0) + 1
        for a in range(3):
            lo[a] = min(lo[a], x['pos'][a])
            hi[a] = max(hi[a], x['pos'][a])
        r = models[name]['radius'] if name else 0.0
        for a in range(2):
            glo[a] = min(glo[a], x['pos'][a] - r)
            ghi[a] = max(ghi[a], x['pos'][a] + r)
    cells = set()
    for x in inst:
        cx = int((x['pos'][0] - PATH_MIN) // PATH_CELL)
        cy = int((x['pos'][1] - PATH_MIN) // PATH_CELL)
        cells.add(cy * 8 + cx if 0 <= cx < 8 and 0 <= cy < 8 else -1)
    txd_missing = sorted({m['txd'] for m in models.values()} - {e[:-4] for e in report['entries'] if e.endswith('.wtd')})
    model_missing = sorted(n for n in models if n + '.wdr' not in report['entries'] and n + '.wdd' not in report['entries']
                           and n + '.wft' not in report['entries'])
    return dict(
        archives=report['archives'],
        kinds={k: [report['kinds'][k], round(report['sizes'][k] / 1048576.0, 1)] for k in sorted(report['kinds'])},
        instances=len(inst), models_defined=len(models), models_placed=len(placed),
        never_placed=sorted(set(models) - set(placed))[:40],
        placed_but_undefined={'%08X' % h: c for h, c in sorted(unknown.items(), key=lambda kv: -kv[1])[:40]},
        extents=dict(min=[round(v) for v in lo], max=[round(v) for v in hi]),
        extents_with_radius=dict(min=[round(v) for v in glo], max=[round(v) for v in ghi]),
        path_areas=sorted(cells), car_generators=len(cars),
        car_generator_models=sorted({'%08X' % c['hash'] for c in cars}),
        txds_missing=txd_missing, models_without_drawable=model_missing[:40],
    )


def main():
    args = sys.argv[1:]
    out_json = None
    if '--json' in args:
        i = args.index('--json')
        out_json = args[i + 1]
        del args[i:i + 2]
    key = open(args[0], 'rb').read()
    name, dirs = args[1], args[2:]
    r = scan(key, dirs)
    print('== %s' % name)
    for k in ('archives', 'kinds', 'instances', 'models_defined', 'models_placed', 'extents', 'extents_with_radius',
              'path_areas', 'car_generators', 'car_generator_models', 'txds_missing', 'models_without_drawable',
              'placed_but_undefined', 'never_placed'):
        v = r[k]
        if k == 'archives':
            v = '%d archives' % len(v)
        print('  %-24s %s' % (k, v))
    if out_json:
        json.dump(r, open(out_json, 'w'), indent=1)


if __name__ == '__main__':
    main()
