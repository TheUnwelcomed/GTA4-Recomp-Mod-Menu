"""A GTA V prop (.ydr) as one PC piece for a props set (v1.7.47): the
drawable converted as vimport.py converts a map's (its V shaders as the
nearest IV presets, the maps by sampler name), scaled (a mod exported in
centimetres: --scale=0.01), its embedded textures as the piece's own
dictionary - the uncompressed ones encoded as DXT5, all held to --side -
and its objs.ide line. propset.py then makes the console files.

usage: python3 -I vprop.py <model.ydr> <name> <out_dir> [--scale=1] [--side=512] [--samples=<dir of IV .wdr>,...]
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import maptex  # noqa: E402
import pcwrite  # noqa: E402
import v_reader  # noqa: E402
import vimport  # noqa: E402

sys.path.insert(0, os.path.join(HERE, '..', 'car'))
import v_car  # noqa: E402  (to_rgba)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    ydr, name, out = args[:3]
    scale = float(opts.get('scale', '1'))
    side = int(opts.get('side', '512'))
    samples = [p for p in opts.get('samples', '').split(',') if p]
    os.makedirs(out, exist_ok=True)
    lib = pcwrite.ShaderLibrary(pcwrite.glob_wdr(samples)) if samples else pcwrite.ShaderLibrary([])
    dr = v_reader.read_ydr(ydr)
    if scale != 1.0:
        for lod in dr['lods']:
            for m in lod:
                for g in m['geometries']:
                    c = g['components']
                    if 'position' in c:
                        c['position'] = (c['position'].astype(np.float64) * scale).astype(np.float32)
    d, used, notes, tris = vimport.convert_drawable(name, dr, lib, print)
    assert d.lods[0], 'no geometry drawn'
    info = d.write(os.path.join(out, name + '.wdr'))
    tex = {}
    for tname, t in dr['embedded_textures'].items():
        if not t.get('data'):
            continue
        key = tname.lower().replace(' ', '_')
        rgba = v_car.to_rgba(t)
        if max(rgba.shape[:2]) > side:
            from PIL import Image
            f = side / max(rgba.shape[:2])
            rgba = np.asarray(Image.fromarray(rgba, 'RGBA').resize(
                (max(4, int(round(rgba.shape[1] * f))), max(4, int(round(rgba.shape[0] * f)))), Image.LANCZOS)).astype(np.uint8)
        fmt, levels = maptex.encode_levels(rgba, side)
        tex[key] = (levels[0][0], levels[0][1], fmt, [lv[2] for lv in levels])
    # (the shaders name the textures with their spaces; the dictionary's names are hashed, so the same)
    pcwrite.Dictionary(tex).write(os.path.join(out, name + '.wtd'))
    lo, hi = info['bbox']
    line = '%s, %s, 300, %d, 0, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 0.0, 0.0, 0.0, %.4f, null' % (
        name, name, vimport.FLAGS, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], info['radius'])
    open(os.path.join(out, name + '.ide'), 'w').write(line + '\n')
    print('%s: %d triangles, %d shaders, %d textures, bounds %s .. %s, radius %.2f%s' % (
        name, tris, len(d.shaders), len(tex), [round(x, 2) for x in lo], [round(x, 2) for x in hi], info['radius'],
        ('; ' + ', '.join('%s x%d' % kv for kv in notes.items())) if notes else ''))


if __name__ == '__main__':
    main()
