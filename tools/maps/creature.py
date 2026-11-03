"""A GTA V animal (a ped: its .yft skeleton, .ydd skinned mesh, .ytd
textures) as a creature the trainer moves (v1.7.39: the sharks, the
crocodile, the elephant): its mesh cut into rigid parts by the bones that
drive each vertex - the body, the tail in two, the jaw, the legs - each
written as a PC drawable in the creature's own space (pc2x360/pcwrite.py),
with a pivot and an axis per part, so that the trainer places the body and
turns each part about its pivot as the creature swims or walks. Nothing of
the game's skinning is needed: the game draws each part as a prop.

usage: python3 -I creature.py <out dir> <name> <spec.json> --samples=<dir of IV .wdr>,...
  spec.json: {"yft": path, "ydd": [paths], "ytd": [paths], "kind": "swimmer"|"walker",
              "scale": 1.0, "turn": 180 (degrees about z: a V animal's bind pose faces -y),
              "parts": {"body": {"bones": ["*"]}, "tail1": {"bones": [...], "axis": "z",
                        "pivot": "boundary" | [x, y, z], "swing": [amplitude deg, hz, phase deg]}, ...},
              "size": [length, height] (optional, measured when missing)}
  writes <out>/<name>_<part>.wdr, <name>.wtd, creature.ide (lines to add to the
  set's .ide list) and creature.txt (the trainer's lines: the parts, their
  pivots, axes and swings, the creature's kind and size); a part's pivot
  "boundary" is the midpoint between its vertices and the body's along y.
"""
import collections
import fnmatch
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import pcwrite  # noqa: E402
import v_reader  # noqa: E402
import vimport  # noqa: E402

TEX, BUMP, SPEC = batconv.SAMPLER_TEXTURE, 0x46B7C64F, 0x608799C6
FLAGS = 0xC00000
AXES = {'x': (1.0, 0.0, 0.0), 'y': (0.0, 1.0, 0.0), 'z': (0.0, 0.0, 1.0)}


def dominant_bones(g):
    """Each vertex's skeleton bone (the heaviest weight), or -1."""
    c = g['components']
    if 'blendindices' not in c or 'blendweights' not in c:
        return np.full(len(c['position']), -1)
    bi = c['blendindices']
    if bi.max() <= 1.0:
        bi = np.round(bi * 255.0)
    bi = bi.astype(int)
    dom = bi[np.arange(len(bi)), np.argmax(c['blendweights'], axis=1)]
    ids = g['bone_ids']
    return np.array([ids[i] if i < len(ids) else -1 for i in dom])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=', 1)[0]: a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--') and '=' in a}
    out, name, spec_path = args[:3]
    spec = json.load(open(spec_path))
    os.makedirs(out, exist_ok=True)
    lib = pcwrite.ShaderLibrary(pcwrite.glob_wdr([p for p in opts.get('--samples', '').split(',') if p]))
    skeleton = v_reader.read_yft(spec['yft'])['skeleton']
    bone_name = [b['name'] for b in skeleton]
    scale = float(spec.get('scale', 1.0))
    turn = math.radians(float(spec.get('turn', 180.0)))
    cs, sn = math.cos(turn), math.sin(turn)
    R = np.array([[cs, -sn, 0.0], [sn, cs, 0.0], [0.0, 0.0, 1.0]], np.float32)

    def part_of(bone):
        """The part a bone belongs to (the first whose patterns match; the body last)."""
        bname = bone_name[bone] if 0 <= bone < len(bone_name) else ''
        for pname, p in spec['parts'].items():
            if pname != 'body' and any(fnmatch.fnmatch(bname, pat) for pat in p['bones']):
                return pname
        return 'body'

    # the geometry, gathered per part: a triangle goes with most of its corners
    parts = {pname: [] for pname in spec['parts']}
    textures = {}
    shaders_seen = {}
    tri_total = 0
    for ydd_path in spec['ydd']:
        for dr in v_reader.read_ydd(ydd_path).values():
            textures.update(dr['embedded_textures'])
            for m in dr['lods'][0]:
                for g in m['geometries']:
                    if len(g['indices']) < 6:
                        continue  # (the dummies: one triangle)
                    sh = dr['shaders'][g['shader']]
                    c = g['components']
                    pos = (c['position'][:, :3] * scale) @ R.T
                    idx = g['indices'].reshape(-1, 3).astype(np.int64)
                    nrm = (c['normal'][:, :3] @ R.T) if 'normal' in c else vimport.face_normals(pos, idx)
                    uv = c['texcoord0'][:, :2] if 'texcoord0' in c else np.zeros((len(pos), 2), np.float32)
                    bones = dominant_bones(g)
                    vpart = np.array([part_of(b) for b in bones])
                    tri_total += len(idx)
                    for pname in parts:
                        votes = (vpart[idx] == pname).sum(axis=1)
                        keep = votes >= 2
                        if not keep.any():
                            continue
                        used = np.unique(idx[keep])
                        remap = -np.ones(len(pos), np.int64)
                        remap[used] = np.arange(len(used))
                        parts[pname].append(dict(shader=sh, pos=pos[used], nrm=nrm[used], uv=uv[used], tris=remap[idx[keep]]))
    for ytd_path in spec.get('ytd', []):
        textures.update(v_reader.read_ytd(ytd_path))
    # the shader: a V ped shader is a normal+spec mapped one; its maps by name
    lines, creature_lines = [], []
    body_pts = np.concatenate([p['pos'] for p in parts['body']])
    lo_all, hi_all = body_pts.min(0), body_pts.max(0)
    for pname, pieces in parts.items():
        if not pieces:
            print('%s: no triangles; left out' % pname)
            continue
        d = pcwrite.Drawable(lib)
        shader_index = {}
        for piece in pieces:
            sh = piece['shader']
            key = id(sh)
            if key not in shader_index:
                params = sh['params']
                tex = {}
                for vh, ih in ((vimport.V_DIFFUSE, TEX), (vimport.V_BUMP, BUMP), (vimport.V_SPEC, SPEC)):
                    p = params.get(vh)
                    if p and p[0] == 'texture' and vimport.texture_name(p[1]):
                        tex[ih] = vimport.texture_name(p[1])
                sps = 'gta_normal_spec.sps' if BUMP in tex and SPEC in tex else 'gta_normal.sps' if BUMP in tex else 'gta_default.sps'
                sps = vimport.iv_preset(lib, sps)
                shader_index[key] = d.shader(sps, tex)
            si = shader_index[key]
            tan = vimport.tangents(piece['pos'], piece['nrm'], piece['uv'], piece['tris']) if vimport.NORMAL_MAPPED.get(d.shaders[si][0]) else None
            d.geometry(0, si, piece['pos'], piece['nrm'], np.full((len(piece['pos']), 4), 255, np.uint8), [piece['uv']], piece['tris'], tan)
        model = '%s_%s' % (name, pname)
        info = d.write(os.path.join(out, model + '.wdr'))
        lo, hi = info['bbox']
        lo_all, hi_all = np.minimum(lo_all, lo), np.maximum(hi_all, hi)
        lines.append('%s, %s, 300, %d, 0, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 0.0, 0.0, 0.0, %.4f, null' % (
            model, name, FLAGS, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], info['radius']))
        p = spec['parts'][pname]
        if pname == 'body':
            creature_lines.append('part body %s' % model)
        else:
            pts = np.concatenate([q['pos'] for q in pieces])
            pivot = p.get('pivot', 'boundary')
            if pivot == 'boundary':
                # the midpoint between this part's reach and the body's along y (the creature's length)
                # (towards the tail (+y after the turn) or the head (-y): whichever side the part lies)
                body_y = body_pts[:, 1]
                if pts[:, 1].mean() > body_y.mean():
                    y = (pts[:, 1].min() + body_y.max()) / 2
                else:
                    y = (pts[:, 1].max() + body_y.min()) / 2
                near = pts[np.abs(pts[:, 1] - y) < max(0.15 * (pts[:, 1].max() - pts[:, 1].min()), 0.05)]
                pivot = [0.0, float(y), float(near[:, 2].mean() if len(near) else pts[:, 2].mean())]
            else:
                pivot = (np.array(pivot, np.float32) * scale) @ R.T
                pivot = [float(v) for v in pivot]
            axis = AXES[p.get('axis', 'z')]
            amp, hz, phase = p.get('swing', [0.0, 0.0, 0.0])
            creature_lines.append('part %s %s pivot %.3f %.3f %.3f axis %.0f %.0f %.0f swing %.1f %.2f %.0f' % (
                pname, model, pivot[0], pivot[1], pivot[2], axis[0], axis[1], axis[2], amp, hz, phase))
        print('%-16s %6d triangles -> %s' % (pname, sum(len(g['tris']) for g in d.lods[0]), model))
    # the textures
    tex = {}
    for tname, t in textures.items():
        if t.get('fmt') in ('DXT1', 'DXT3', 'DXT5') and t.get('data'):
            levels, off, w, h = [], 0, t['w'], t['h']
            for k in range(t['levels']):
                n = pcwrite.dxt_level_bytes(t['fmt'], max(1, w >> k), max(1, h >> k))
                if off + n > len(t['data']):
                    break
                levels.append(t['data'][off:off + n])
                off += n
            if levels:
                tex[tname] = (t['w'], t['h'], t['fmt'], levels)
    pcwrite.Dictionary(tex).write(os.path.join(out, name + '.wtd'))
    size = spec.get('size') or [float(hi_all[1] - lo_all[1]), float(hi_all[2] - lo_all[2])]
    with open(os.path.join(out, 'creature.ide'), 'a') as f:
        f.write('objs\n' + '\n'.join(lines) + '\nend\n')
    with open(os.path.join(out, 'creature.txt'), 'a') as f:
        f.write('creature %s kind %s length %.2f height %.2f bottom %.2f\n' % (name, spec.get('kind', 'walker'), size[0], size[1], float(lo_all[2])))
        for cl in creature_lines:
            f.write('  %s\n' % cl)
    print('%s: %d parts, %d triangles, %d textures; %.1f m long, %.1f m high' % (
        name, len([p for p in parts.values() if p]), tri_total, len(tex), size[0], size[1]))


if __name__ == '__main__':
    main()
