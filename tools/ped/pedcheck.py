"""Read a converted player component back (its .xdr decoded with xlzx) and
draw it (v1.7.46): three views of its triangles coloured by the bone that
weighs most on each, the skeleton's joints marked, so a suit converted from
another game can be seen to sit on the player's bones - and the structure
checked: the vertex layout, every bone table's size and entries, the
weights' sums, the embedded textures, the bounds.

usage: python3 -I pedcheck.py <out_dir> <component name> <out.png> <xlzx>
  e.g. pedcheck.py out uppr_017_u uppr.png ../conv/xlzx
"""
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
import batconv  # noqa: E402
import rsc5  # noqa: E402

PED_LAYOUT = {0: 0, 1: 12, 2: 16, 3: 20, 4: 24, 6: 28, 14: 32}


def load(out_dir, name, xlzx):
    path = os.path.join(out_dir, name + '.xdr')
    raw = bytearray(open(path, 'rb').read())
    magic, rtype, flags = struct.unpack_from('<III', raw, 0)
    if magic != rsc5.RSC_MAGIC:
        magic, rtype, flags = struct.unpack_from('>III', raw, 0)
    assert magic == rsc5.RSC_MAGIC, path
    struct.pack_into('>III', raw, 0, magic, rtype, flags)  # (xlzx reads the header big-endian)
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp.write(raw)
    subprocess.run([xlzx, 'unrsc', tmp.name, tmp.name], check=True, capture_output=True)
    body = open(tmp.name, 'rb').read()
    os.remove(tmp.name)
    return rsc5.Resource(body, flags, True, rtype, path)


def main():
    out_dir, name, png, xlzx = sys.argv[1:5]
    r = load(out_dir, name, xlzx)
    d = rsc5.read_drawable(r, 0)
    _, bones = batconv.read_skeleton(r, d['skeleton_off'])
    names = [b['name'] for b in bones]
    jpos, _ = batconv.pose(bones, [b['t'] for b in bones])
    print('%s: %d bones in the skeleton, bounds %s .. %s, radius %.3f, LODs %s' % (
        name, len(bones), tuple(round(x, 3) for x in d['bmin'][:3]), tuple(round(x, 3) for x in d['bmax'][:3]),
        r.f32(0x70), [len(l) for l in d['lods']]))
    sg = d['shader_group']
    for s in sg['shaders']:
        texs = []
        for p in batconv.shader_params(r, s['off']):
            if p['type'] == 0 and p['value'] is not None:
                np_ = r.vptr(p['value'] + 20)
                texs.append(r.cstr(np_) if np_ is not None else '?')
        print('  shader %s: %s' % (batconv.shader_sps(r, s['off']), ', '.join(texs)))
    td = r.vptr(sg['off'] + 4)
    if td is not None:
        tl, n = r.vptr(td + 24), r.u16(td + 20)
        for i in range(n):
            info = r.vptr(tl + 4 * i)
            d3d = r.vptr(info + 24)
            f = struct.unpack_from('>6I', r.virt, d3d + 28)
            print('  embedded %s %dx%d levels %d fmt %d base %08X mip %08X' % (
                r.cstr(r.vptr(info + 20)), r.u16(info + 28), r.u16(info + 30), r.u32(info + 32), f[1] & 0x3F,
                f[1] & 0x0FFFF000, f[5] & 0x0FFFF000))
    tris = {}
    for m in d['lods'][0]:
        for gi, g in enumerate(m['geometries']):
            lay = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_360)[0])
            assert lay == PED_LAYOUT and g['vb_stride'] == 36, (lay, g['vb_stride'])
            bp = r.vptr(g['off'] + 56)
            table = [r.u16(bp + 2 * k) for k in range(r.u16(g['off'] + 62))] if bp else []
            n = g['vertex_count']
            raw = np.frombuffer(r.phys[g['vb_data'][1]:g['vb_data'][1] + n * 36], np.uint8).reshape(n, 36)
            pos = raw[:, 0:12].copy().view('>f4').reshape(n, 3)
            w = raw[:, 12:16][:, ::-1].astype(int)
            ix = raw[:, 16:20][:, ::-1].astype(int)
            assert (w.sum(1) == 255).all(), 'weights not summing to 255'
            assert ix[w > 0].max() < len(table), 'a bone index past the table'
            idx = np.frombuffer(r.phys[g['ib_data'][1]:g['ib_data'][1] + 2 * g['ib_count']], '>u2').astype(int)
            print('  geometry %d: %d vertices, %d triangles, %d bones: %s' % (
                gi, n, len(idx) // 3, len(table), ' '.join(names[b] for b in table)))
            heavy = ix[np.arange(n), w.argmax(1)]
            for t in range(0, len(idx) - 2, 3):
                a, b_, c = idx[t:t + 3]
                tris.setdefault(names[table[heavy[a]]], []).append((pos[a], pos[b_], pos[c]))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    fig, axes = plt.subplots(1, 3, figsize=(24, 9))
    views = [('front (x right, z up)', 0, 2), ('side (y forward, z up)', 1, 2), ('top (x right, y forward)', 0, 1)]
    keys = sorted(tris)
    cmap = plt.get_cmap('tab20')
    for ax, (title, i, j) in zip(axes, views):
        for k, bn in enumerate(keys):
            ax.add_collection(PolyCollection([[(p[i], p[j]) for p in t] for t in tris[bn]],
                                             facecolors=cmap(k % 20), edgecolors='none', alpha=0.85, label=bn))
        ax.plot([p[i] for p in jpos], [p[j] for p in jpos], 'k.', ms=4)
        ax.autoscale()
        ax.set_aspect('equal')
        ax.set_title(title)
        ax.grid(True, alpha=0.3)
    axes[0].legend(loc='upper left', fontsize=6, ncol=2)
    plt.tight_layout()
    plt.savefig(png, dpi=60)
    print('  drawn: %d bone groups, %d triangles' % (len(tris), sum(len(v) for v in tris.values())))


if __name__ == '__main__':
    main()
