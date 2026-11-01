"""Render a converted 360 car (its decoded .xft body, from run_vehicle.py's
out dir) with some bones turned about their rest position, the way the game
turns a joint (a door about its hinge) or the trainer turns a pivot, in
three views; turned parts coloured, the hinges marked. Row-vector rotations,
as RAGE stores them (a left door opens with a negative angle about z, a
part in front of its hinge rises with a positive one about x).

usage: render_posed.py <out_dir> <name> <out.png> bone:axis:degrees ...
  e.g. render_posed.py out/deimos deimos doors.png door_dside_f:z:-60 door_pside_f:z:60"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
import rsc5, batconv

out_dir, name, png = sys.argv[1:4]
poses = {}
for a in sys.argv[4:]:
    b, ax, deg = a.split(':')
    poses[b] = ('xyz'.index(ax), float(deg))
fl = dict(l.split()[0:3:2] for l in open('%s/flags.txt' % out_dir))
r = rsc5.Resource(open('%s/%s.xft.bin' % (out_dir, name), 'rb').read(), int(fl[name + '.xft'], 16), True, 112, name)
f = rsc5.read_fragtype(r)
d = f['drawable']
_, bones = batconv.read_skeleton(r, d['skeleton_off'])
names = [b['name'] for b in bones]

def rot(axis, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [[(1, 0, 0), (0, c, s), (0, -s, c)], [(c, 0, -s), (0, 1, 0), (s, 0, c)], [(c, s, 0), (-s, c, 0), (0, 0, 1)]][axis]

mats = {}
for bn, (axis, deg) in poses.items():
    i = names.index(bn)
    mats[i] = (bones[i]['pos'], rot(axis, deg))

tris = {}
for m in d['lods'][0]:
    for g in m['geometries']:
        lay = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_360)[0])
        bp = r.vptr(g['off'] + 56)
        table = [r.u16(bp + 2 * k) for k in range(r.u16(g['off'] + 62))] if bp else []
        pos, grp = [], []
        for v in range(g['vertex_count']):
            o = g['vb_data'][1] + v * g['vb_stride']
            p = struct.unpack_from('>3f', r.phys, o + lay[0])
            w = r.phys[o + lay[1]:o + lay[1] + 4][::-1] if 1 in lay else b'\xff\0\0\0'
            ix = r.phys[o + lay[2]:o + lay[2] + 4][::-1] if 2 in lay else b'\0\0\0\0'
            k = max(range(4), key=lambda q: w[q])
            bone = table[ix[k]] if table else 0
            if bone in mats:
                h, M = mats[bone]
                rel = batconv.v_sub(p, h)
                p = batconv.v_add(batconv.v_mat(rel, M), h)
            pos.append(p)
            grp.append(names[bone] if bone in mats else 'rest')
        idx = struct.unpack_from('>%dH' % g['ib_count'], r.phys, g['ib_data'][1])
        for t in range(0, len(idx) - 2, 3):
            a, b_, c = idx[t:t + 3]
            tris.setdefault(grp[a], []).append((pos[a], pos[b_], pos[c]))
fig, axes = plt.subplots(1, 3, figsize=(30, 10))
views = [('side (y forward, z up)', 1, 2), ('top (x right, y forward)', 0, 1), ('rear (x right, z up)', 0, 2)]
colors = {'rest': (0.6, 0.6, 0.6, 0.25)}
palette = [(0.85, 0.2, 0.2, 0.7), (0.2, 0.4, 0.9, 0.7), (0.1, 0.7, 0.3, 0.7), (0.9, 0.6, 0.1, 0.7)]
for k, bn in enumerate(sorted(x for x in tris if x != 'rest')):
    colors[bn] = palette[k % 4]
for ax, (title, i, j) in zip(axes, views):
    for bn, ts in tris.items():
        ax.add_collection(PolyCollection([[(p[i], p[j]) for p in t] for t in ts], facecolors=colors[bn], edgecolors='none', label=bn))
    for bi, (h, _) in mats.items():
        ax.plot([h[i]], [h[j]], 'k+', ms=20, mew=3)
    ax.autoscale(); ax.set_aspect('equal'); ax.set_title(title); ax.grid(True, alpha=0.3)
axes[0].legend(loc='upper left')
plt.tight_layout(); plt.savefig(png, dpi=55)
print({k: len(v) for k, v in tris.items()})
