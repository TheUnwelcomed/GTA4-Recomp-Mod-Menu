"""GTA V map resources as a map set's PC pieces (v1.7.38): the drawables
(.ydr), texture dictionaries (.ytd) and placements (.ymap, through the
archetypes of the .ytyp files) of a V mod, written as GTA IV PC files
(pc2x360/pcwrite.py: .wdr, .wtd) with their lines (an .ide) and placements
(a set's "extra" file, the model's full rotation), so that mapset.py takes
them like a IV mod's own pieces - the way the Scarface mansion joins the
Gator Keys. Collision comes from each model's render mesh (<model>.colmesh.npz,
cut below the set's shallows and decimated by mapset.py as a .wbn would be).

usage: python3 -I vimport.py <V dir> <out dir> [--shift=dx,dy,dz | --anchor=vx,vy,vz:tx,ty,tz --turn=deg]
                             [--skip=pat,pat...] [--no-collision=pat,...] [--samples=<dir of IV .wdr>,...]
  V dir      the mod's stream folder (.ydr, .ytd, .ymap, .ytyp)
  out dir    where the .wdr / .wtd / .colmesh.npz / vmodels.ide / vplace.txt go
  --shift    added to every placement (the mod's V coordinates to the set's)
  --anchor   the V point (vx, vy, vz) put at the set's (tx, ty, tz), the whole
             turned by --turn degrees about it (counter-clockwise seen from above)
  --skip     models left out (fnmatch patterns): lights with no mesh, cables
  --samples  directories of IV .wdr files for the shader samples (the set's own)
  --txd      texture dictionaries shared by groups of models, "name:pat,pat;name:pat"
             (fnmatch on the model name, the first match; the rest keep one of
             their own): the set makes one dictionary per name, so textures
             shared by the group's models are held once

What is not here: the mod's entities whose archetypes are the game's own
(GTA V's props: palms, furniture) - no file of theirs is in the pack; they
are listed in the log with their hashes. Vertex colours, normals and the
first two texture coordinate sets carry over; a V shader becomes the IV
effect nearest it (V_SHADERS), its diffuse, bump and specular maps by their
sampler names; terrain blends keep their first layer.
"""
import collections
import fnmatch
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

TEX = batconv.SAMPLER_TEXTURE   # IV TextureSampler
BUMP, SPEC = 0x46B7C64F, 0x608799C6
V_DIFFUSE = v_reader.joaat('DiffuseSampler')
V_LAYER0 = v_reader.joaat('TextureSampler_layer0')
V_BUMP, V_SPEC = v_reader.joaat('BumpSampler'), v_reader.joaat('SpecSampler')
FLAGS = 0xC00000

# V shader name -> IV preset (the nearest the game has a sample of)
V_SHADERS = {
    'default': 'gta_default.sps', 'default_tnt': 'gta_default.sps', 'default_um': 'gta_default.sps',
    'default_spec': 'gta_spec.sps', 'default_detail': 'gta_default.sps',
    'normal': 'gta_normal.sps', 'normal_tnt': 'gta_normal.sps', 'normal_um': 'gta_normal.sps',
    'normal_detail': 'gta_normal.sps', 'normal_pxm': 'gta_normal.sps', 'normal_decal': 'gta_normal.sps',
    'normal_spec': 'gta_normal_spec.sps', 'normal_spec_pxm': 'gta_normal_spec.sps',
    'normal_spec_detail': 'gta_normal_spec.sps', 'normal_spec_tnt': 'gta_normal_spec.sps',
    'normal_spec_um': 'gta_normal_spec.sps', 'normal_spec_detail_tnt': 'gta_normal_spec.sps',
    'normal_spec_decal': 'gta_normal_spec.sps', 'normal_spec_decal_pxm': 'gta_normal_spec.sps',
    'normal_spec_reflect': 'gta_normal_spec_reflect.sps', 'normal_reflect': 'gta_normal_reflect.sps',
    'normal_spec_cubemap_reflect': 'gta_normal_spec_cubemap_reflect.sps',
    'normal_spec_emissive': 'gta_normal_spec.sps',
    'spec': 'gta_spec.sps', 'spec_tnt': 'gta_spec.sps', 'spec_reflect': 'gta_spec_reflect.sps',
    'spec_decal': 'gta_spec_decal.sps', 'reflect': 'gta_reflect.sps',
    'emissive': 'gta_emissive.sps', 'emissivenight': 'gta_emissivenight.sps',
    'emissivestrong': 'gta_emissivestrong.sps', 'emissive_alpha': 'gta_emissive_alpha.sps',
    'emissivenight_alpha': 'gta_emissivenight_alpha.sps',
    'decal': 'gta_decal.sps', 'decal_glue': 'gta_decal_glue.sps', 'decal_dirt': 'gta_decal.sps',
    'decal_normal_only': 'gta_decal.sps', 'decal_emissive_only': 'gta_emissive.sps',
    'cutout': 'gta_cutout.sps', 'cutout_hard': 'gta_cutout.sps', 'cutout_tnt': 'gta_cutout.sps',
    'cutout_fence': 'gta_cutout_fence.sps', 'cutout_fence_normal': 'gta_cutout_fence.sps',
    'normal_cutout': 'gta_normal_cutout.sps', 'normal_cutout_tnt': 'gta_normal_cutout.sps',
    'normal_spec_cutout': 'gta_normal_cutout.sps', 'normal_spec_cutout_tnt': 'gta_normal_cutout.sps',
    'normal_alpha': 'gta_normal_alpha.sps', 'spec_alpha': 'gta_spec_alpha.sps',
    'normal_spec_alpha': 'gta_normal_spec_alpha.sps',
    'glass': 'gta_glass.sps', 'glass_spec': 'gta_glass.sps', 'glass_reflect': 'gta_glass_reflect.sps',
    'glass_env': 'gta_glass.sps', 'glass_pv': 'gta_glass.sps', 'glass_emissive': 'gta_glass.sps',
    'glass_emissivenight': 'gta_glass.sps', 'glass_normal_spec_reflect': 'gta_glass_reflect.sps',
    'glass_breakable': 'gta_glass.sps', 'glass_displacement': 'gta_glass.sps',
    'water_poolenv': 'gta_glass.sps', 'water_river': 'gta_glass.sps', 'water_fountain': 'gta_glass.sps',
    'water_shallow': 'gta_glass.sps', 'water_terrainfoam': 'gta_glass.sps',
    'trees': 'gta_trees.sps', 'trees_normal': 'gta_trees.sps', 'trees_lod': 'gta_trees.sps',
    'terrain_cb_4lyr': 'gta_default.sps', 'terrain_cb_4lyr_lod': 'gta_default.sps',
    'terrain_cb_w_4lyr': 'gta_default.sps', 'terrain_cb_w_4lyr_lod': 'gta_default.sps',
    'terrain_cb_w_4lyr_2tex': 'gta_default.sps', 'terrain_cb_w_4lyr_cm': 'gta_default.sps',
    'terrain_cb_w_4lyr_spec': 'gta_default.sps', 'terrain_cb_w_4lyr_pxm': 'gta_default.sps',
    'terrain_cb_w_4lyr_cm_tnt': 'gta_default.sps', 'parallax': 'gta_normal.sps',
    'parallax_specmap': 'gta_normal_spec.sps', 'mirror_default': 'gta_spec.sps',
    'cloth_default': 'gta_default.sps', 'ped': 'gta_default.sps', 'ped_default': 'gta_default.sps',
    'cable': None, 'grass': None, 'grass_batch': None, 'radar': None,
}
V_BY_HASH = {v_reader.joaat(k): (k, v) for k, v in V_SHADERS.items()}
# IV presets with no PC sample about: what stands in
IV_FALLBACK = {
    'gta_normal_spec_reflect.sps': 'gta_normal_spec.sps', 'gta_normal_reflect.sps': 'gta_normal.sps',
    'gta_normal_spec_cubemap_reflect.sps': 'gta_normal_spec.sps', 'gta_spec_reflect.sps': 'gta_spec.sps',
    'gta_reflect.sps': 'gta_spec.sps', 'gta_emissivestrong.sps': 'gta_emissive.sps',
    'gta_emissive_alpha.sps': 'gta_emissive.sps', 'gta_emissivenight_alpha.sps': 'gta_emissivenight.sps',
    'gta_decal_glue.sps': 'gta_decal.sps', 'gta_cutout_fence.sps': 'gta_cutout.sps',
    'gta_normal_cutout.sps': 'gta_normal.sps', 'gta_normal_alpha.sps': 'gta_normal.sps',
    'gta_spec_alpha.sps': 'gta_spec.sps', 'gta_normal_spec_alpha.sps': 'gta_normal_spec.sps',
    'gta_glass_reflect.sps': 'gta_glass.sps', 'gta_glass.sps': 'gta_alpha.sps', 'gta_alpha.sps': 'gta_default.sps',
    'gta_spec_decal.sps': 'gta_decal.sps', 'gta_trees.sps': 'gta_cutout.sps', 'gta_cutout.sps': 'gta_default.sps',
    'gta_emissivenight.sps': 'gta_emissive.sps', 'gta_emissive.sps': 'gta_default.sps',
    'gta_normal_spec.sps': 'gta_normal.sps', 'gta_normal.sps': 'gta_default.sps', 'gta_spec.sps': 'gta_default.sps',
    'gta_decal.sps': 'gta_default.sps',
}


def iv_preset(lib, sps):
    """The nearest preset the library has a sample of."""
    seen = set()
    while sps not in lib and sps in IV_FALLBACK and sps not in seen:
        seen.add(sps)
        sps = IV_FALLBACK[sps]
    return sps if sps in lib else 'gta_default.sps'


def texture_name(t):
    return (t or {}).get('name', '').split('/')[-1].lower().replace('.dds', '') if t else ''


def convert_drawable(name, dr, lib, log):
    """A V drawable (v_reader.read_drawable) as a pcwrite.Drawable; returns
    (drawable, texture names used, notes)."""
    d = pcwrite.Drawable(lib)
    notes = collections.Counter()
    shader_index = {}
    used_tex = set()
    for si, sh in enumerate(dr['shaders']):
        vname, sps = V_BY_HASH.get(sh['name_hash'], ('%08X' % sh['name_hash'], 'gta_default.sps'))
        if sps is None:
            shader_index[si] = None
            notes['left out: ' + vname] += 1
            continue
        want = sps
        sps = iv_preset(lib, sps)
        if sps != want:
            notes['%s as %s' % (want, sps)] += 1
        params = sh['params']
        textures = {}
        diffuse = params.get(V_DIFFUSE) or params.get(V_LAYER0)
        if diffuse and diffuse[0] == 'texture' and texture_name(diffuse[1]):
            textures[TEX] = texture_name(diffuse[1])
        for vh, ih in ((V_BUMP, BUMP), (V_SPEC, SPEC)):
            p = params.get(vh)
            if p and p[0] == 'texture' and texture_name(p[1]):
                textures[ih] = texture_name(p[1])
        used_tex.update(textures.values())
        shader_index[si] = d.shader(sps, textures)
    tris_in = 0
    for lod_k, lod in enumerate(dr['lods'][:1]):  # (the first LOD: the pieces are drawn at every distance)
        for m in lod:
            for g in m['geometries']:
                si = shader_index.get(g['shader'])
                if si is None:
                    continue
                c = g['components']
                pos = c.get('position')
                if pos is None:
                    continue
                pos = pos[:, :3]
                idx = g['indices'].reshape(-1, 3)
                # (every IV effect reads a normal and a colour: made where V has none)
                nrm = c['normal'][:, :3] if 'normal' in c else face_normals(pos, idx)
                if 'colour0' in c and c['colour0'].shape[1] == 4:
                    # (the bytes as stored: V's vertex colour is laid out as IV's D3DCOLOR)
                    col = np.clip(c['colour0'] * 255.0 + 0.5, 0, 255).astype(np.uint8)
                else:
                    col = np.full((len(pos), 4), 255, np.uint8)
                uvs = [c[k][:, :2] for k in ('texcoord0', 'texcoord1') if k in c]
                if not uvs:
                    uvs = [np.zeros((len(pos), 2), np.float32)]
                tris_in += len(idx)
                tan = None
                if NORMAL_MAPPED.get(d.shaders[si][0]):  # (its tangents, or ones made from the texture coordinates)
                    tan = c['tangent'][:, :4] if 'tangent' in c and c['tangent'].shape[1] >= 4 else None
                    if tan is None or not np.isfinite(tan).all() or np.abs(tan[:, :3]).max() < 1e-6:
                        tan = tangents(pos, nrm if nrm is not None else np.tile([0, 0, 1.0], (len(pos), 1)), uvs[0], idx)
                d.geometry(lod_k, si, pos, nrm, col, uvs, idx, tan)
    return d, used_tex, notes, tris_in


# IV effects reading a tangent (vertex usage bit 14)
NORMAL_MAPPED = {s: True for s in ('gta_normal.sps', 'gta_normal_spec.sps', 'gta_normal_cutout.sps', 'gta_normal_alpha.sps',
                                   'gta_normal_spec_alpha.sps', 'gta_normal_reflect.sps', 'gta_normal_spec_reflect.sps',
                                   'gta_normal_spec_cubemap_reflect.sps', 'gta_glass.sps', 'gta_glass_reflect.sps')}


def face_normals(pos, tris):
    """Per-vertex normals from the faces round each vertex."""
    n = np.zeros((len(pos), 3), np.float64)
    p = pos.astype(np.float64)
    fn = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    for k in range(3):
        np.add.at(n, tris[:, k], fn)
    length = np.linalg.norm(n, axis=1, keepdims=True)
    n = np.where(length > 1e-12, n / np.maximum(length, 1e-12), np.array([[0.0, 0.0, 1.0]]))
    return n.astype(np.float32)


def tangents(pos, nrm, uv, tris):
    """Per-vertex tangents (x, y, z, handedness) from the triangles' texture
    coordinate derivatives (Lengyel), orthogonal to the normals."""
    n = len(pos)
    t_acc = np.zeros((n, 3), np.float64)
    b_acc = np.zeros((n, 3), np.float64)
    p0, p1, p2 = pos[tris[:, 0]].astype(np.float64), pos[tris[:, 1]].astype(np.float64), pos[tris[:, 2]].astype(np.float64)
    w0, w1, w2 = uv[tris[:, 0]].astype(np.float64), uv[tris[:, 1]].astype(np.float64), uv[tris[:, 2]].astype(np.float64)
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = w1 - w0, w2 - w0
    det = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(det) > 1e-12, 1.0 / np.where(np.abs(det) > 1e-12, det, 1.0), 0.0)
    sdir = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) * r[:, None]
    tdir = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) * r[:, None]
    for k in range(3):
        np.add.at(t_acc, tris[:, k], sdir)
        np.add.at(b_acc, tris[:, k], tdir)
    nn = nrm[:, :3].astype(np.float64)
    t = t_acc - nn * (nn * t_acc).sum(1, keepdims=True)
    length = np.linalg.norm(t, axis=1, keepdims=True)
    bad = length[:, 0] < 1e-9
    # a vertex with no usable derivative: any direction across its normal
    helper = np.where(np.abs(nn[:, 0:1]) < 0.9, np.array([[1.0, 0, 0]]), np.array([[0, 1.0, 0]]))
    fallback = np.cross(nn, helper)
    t[bad] = fallback[bad]
    t /= np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-9)
    hand = np.where((np.cross(nn, t) * b_acc).sum(1) < 0, -1.0, 1.0)
    return np.concatenate([t, hand[:, None]], 1).astype(np.float32)


def bake_rotation(dr, q):
    """The drawable's positions and normals turned by the quaternion (x, y, z, w)."""
    x, y, z, w = q
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]], np.float32)
    for lod in dr['lods']:
        for m in lod:
            for g in m['geometries']:
                c = g['components']
                for k in ('position', 'normal', 'tangent', 'binormal'):
                    if k in c:
                        a = c[k]
                        a[:, :3] = a[:, :3] @ R.T


def collision_soup(dr, path):
    """The first LOD's triangles (the model's space) as the piece's collision."""
    vs, ts, base = [], [], 0
    for m in dr['lods'][0]:
        for g in m['geometries']:
            pos = g['components'].get('position')
            if pos is None:
                continue
            idx = g['indices'].reshape(-1, 3).astype(np.int64)
            vs.append(pos[:, :3].astype(np.float32))
            ts.append(idx + base)
            base += len(pos)
    if not ts:
        return 0
    v, t = np.concatenate(vs), np.concatenate(ts)
    np.savez(path, v=v, t=t, m=np.zeros(len(t), np.uint32))
    return len(t)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=', 1)[0]: a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--') and '=' in a}
    vdir, out = args[:2]
    shift = [float(v) for v in opts.get('--shift', '0,0,0').split(',')] + [0.0] * 3
    turn = math.radians(float(opts.get('--turn', '0')))
    anchor = target = None
    if '--anchor' in opts:
        a, t = opts['--anchor'].split(':')
        anchor = np.array([float(v) for v in a.split(',')])
        target = np.array([float(v) for v in t.split(',')])
    cs, sn = math.cos(turn), math.sin(turn)
    R = np.array([[cs, -sn, 0.0], [sn, cs, 0.0], [0.0, 0.0, 1.0]])
    q_turn = np.array([0.0, 0.0, math.sin(turn / 2), math.cos(turn / 2)])

    def place(pos, rot):
        """A V placement (its stored rotation) as the set's position and true rotation."""
        p = np.array(pos)
        q = np.array([-rot[0], -rot[1], -rot[2], rot[3]])  # (stored inverted, as GTA IV's .wpl rotations are)
        if anchor is not None:
            p = R @ (p - anchor) + target
        else:
            p = p + np.array(shift[:3])
        if turn:
            x1, y1, z1, w1 = q_turn
            x2, y2, z2, w2 = q
            q = np.array([w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
                          w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2])
        return p, q
    skip = [p for p in opts.get('--skip', '').split(',') if p]
    no_col = [p for p in opts.get('--no-collision', '').split(',') if p]
    sample_dirs = [p for p in opts.get('--samples', '').split(',') if p]
    txd_groups = []
    for grp in opts.get('--txd', '').split(';'):
        if ':' in grp:
            gname, pats = grp.split(':', 1)
            txd_groups.append((gname, [q for q in pats.split(',') if q]))

    def txd_of(model):
        for gname, pats in txd_groups:
            if any(fnmatch.fnmatch(model, q) for q in pats):
                return gname
        return model
    os.makedirs(out, exist_ok=True)
    lib = pcwrite.ShaderLibrary(pcwrite.glob_wdr(sample_dirs))
    print('shader samples: %d presets' % len(lib.samples))
    files = sorted(os.listdir(vdir))
    ydr = {f[:-4].lower(): os.path.join(vdir, f) for f in files if f.lower().endswith('.ydr')}
    hash_of = {v_reader.joaat(n): n for n in ydr}
    # the archetypes: name hash -> the drawable (asset) and the texture dictionary
    arch = {}
    for f in files:
        if f.lower().endswith('.ytyp'):
            for a in v_reader.read_ytyp(os.path.join(vdir, f)):
                arch[a['name']] = a
    # the placements of models in the pack
    placed = collections.Counter()
    rows = []
    missing = collections.Counter()
    for f in files:
        if not f.lower().endswith('.ymap'):
            continue
        for e in v_reader.read_ymap(os.path.join(vdir, f)):
            a = arch.get(e['archetype'])
            name = hash_of.get(a['asset_name']) if a else None
            if name is None:
                name = hash_of.get(e['archetype'])
            if name is None:
                missing[e['archetype']] += 1
                continue
            if any(fnmatch.fnmatch(name, p) for p in skip):
                continue
            placed[name] += 1
            rows.append((name, e))
    print('placements: %d of %d models here; %d of %d archetypes the pack has no model for (GTA V\'s own: %s)' % (
        len(rows), len(placed), sum(missing.values()), len(missing),
        ', '.join('%08X x%d' % kv for kv in missing.most_common(6))))
    # the models
    textures_all = {}
    for f in files:
        if f.lower().endswith('.ytd'):
            td = v_reader.read_ytd(os.path.join(vdir, f))
            dname = f[:-4].lower()
            textures_all[dname] = td
    lines, tri_total, col_total = [], 0, 0
    report = collections.Counter()
    baked = {}  # (a model placed once: its rotation baked into the vertices, the placement unrotated,
                #  so that mapset.py may tile it and take its collision as a .wbn's)
    for name in sorted(placed):
        dr = v_reader.read_ydr(ydr[name])
        if placed[name] == 1:
            e = next(e for n, e in rows if n == name)
            _, q = place(e['pos'], e['rot'])
            if abs(q[3]) < 0.999999:
                bake_rotation(dr, q)
                baked[name] = True
        d, used, notes, tris = convert_drawable(name, dr, lib, print)
        report.update(notes)
        if not d.lods[0]:
            print('%s: no geometry drawn (its shaders left out); skipped' % name)
            continue
        info = d.write(os.path.join(out, name + '.wdr'))
        tri_total += tris
        # its own texture dictionary: the embedded textures (DXT ones; others flat), named like the model
        tex = {}
        for tname, t in dr['embedded_textures'].items():
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
            else:
                report['texture format not taken: %s' % t.get('fmt')] += 1
        if tex:
            pcwrite.Dictionary(tex).write(os.path.join(out, name + '.wtd'))
        if not any(fnmatch.fnmatch(name, p) for p in no_col):
            col_total += collision_soup(dr, os.path.join(out, name + '.colmesh.npz'))
        lo, hi = info['bbox']
        lines.append('%s, %s, 300, %d, 0, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 0.0, 0.0, 0.0, %.4f, null' % (
            name, txd_of(name), FLAGS, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], info['radius']))
        print('%-24s %6d triangles, %2d shaders, %3d textures, placed %d%s' % (
            name, tris, len(d.shaders), len(tex), placed[name],
            ('; ' + ', '.join('%s x%d' % kv for kv in notes.items())) if notes else ''))
    # the pack's texture dictionaries, whole (the models find their textures by name across them)
    for dname, td in textures_all.items():
        tex = {}
        for tname, t in td.items():
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
        if tex:
            pcwrite.Dictionary(tex).write(os.path.join(out, 'v_' + dname + '.wtd'))
            print('%s.ytd: %d textures -> v_%s.wtd' % (dname, len(tex), dname))
    with open(os.path.join(out, 'vmodels.ide'), 'w') as f:
        f.write('objs\n' + '\n'.join(lines) + '\nend\n')
    with open(os.path.join(out, 'vplace.txt'), 'w') as f:
        f.write('# model x y z qx qy qz qw (the set\'s coordinates, the true rotation; the mod\'s V ones %s)\n' % (
            ('turned %.1f degrees about %s, put at %s' % (math.degrees(turn), anchor.tolist(), target.tolist()))
            if anchor is not None else 'shifted by %s' % (shift[:3],)))
        for name, e in rows:
            p, q = place(e['pos'], e['rot'])
            if name in baked:
                q = np.array([0.0, 0.0, 0.0, 1.0])
            f.write('%s %.3f %.3f %.3f %.6f %.6f %.6f %.6f\n' % (name, p[0], p[1], p[2], q[0], q[1], q[2], q[3]))
    if baked:
        print('%d models placed once written turned as placed, their placements unrotated: %s' % (
            len(baked), ', '.join(sorted(baked))))
    print('%d models written, %d triangles (%d of collision); shaders: %s' % (
        len(lines), tri_total, col_total, ', '.join('%s x%d' % kv for kv in report.most_common())))


if __name__ == '__main__':
    main()
