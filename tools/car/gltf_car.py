"""A car from a glTF model (v1.7.44): a Sketchfab-style scene (scene.gltf,
scene.bin, textures/) written as a PC vehicle (pcwrite.Fragment: the body
on a chassis bone, the wheel mesh as the fragment's wheel child on
wheel_lf, the other wheels' bones at their centres, the shaders cloned
from a PC car mod's samples, a texture dictionary from the model's PNGs)
and then put through the console converter on a stock car's template like
any PC mod (run_vehicle: the template's physics and wheels moved to the
model's, the seats as points). The Hellcat: specs/hellcat.json.

usage: python3 -I gltf_car.py <spec.json> <scene.gltf> <pattern.wft> <export_dir> <work_dir> <out_dir>
  spec.json    the car: name, template, length (m, nose to tail), axes (the
               game's x y z from the glTF's: e.g. ["-x", "z", "y"]), wheels
               {wheel_lf: node name, ...}, origin {z_over_hubs}, seats {bone:
               [x, y, z]}, materials {name fragment: kind} (kinds: paint,
               glass, generic, mesh), texture_side, radii {wheel_lf: r, ...},
               options (more run_vehicle options, "k=v,...")
  pattern.wft  a PC car mod's file: the fragment header's pattern and the
               shader samples
  export_dir   the stock 360 files (the trainer build's logs/export)
  work_dir     their decompressed bodies (<car>.xft.bin, .xtd.bin)
  out_dir      <name>.wft, <name>.wtd (the PC files), the converted
               <name>.xft.bin, <name>.xtd.bin and flags.txt (for xlzx rsc)
"""
import collections
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, os.path.join(HERE, '..', 'maps'))
sys.path.insert(0, HERE)
import gltf_read  # noqa: E402
import maptex  # noqa: E402
import pcwrite  # noqa: E402
import rsc5  # noqa: E402

DIFFUSE, DIRT, NORMAL, SPEC = 0x2B5170FD, 0x7E9A27FE, 0x46B7C64F, 0x608799C6  # the vehicle shaders' samplers
SHADER_OF = {  # kind -> (.sps, fallback .sps)
    'paint': ('gta_vehicle_paint1.sps', 'gta_vehicle_paint3.sps'),
    'glass': ('gta_vehicle_vehglass.sps', None),
    'generic': ('gta_vehicle_generic.sps', 'gta_vehicle_mesh.sps'),
    'mesh': ('gta_vehicle_mesh.sps', 'gta_vehicle_generic.sps'),
}
WHEEL_BONES = ('wheel_lf', 'wheel_rf', 'wheel_lr', 'wheel_rr')


def axes_matrix(axes):
    """["-x", "z", "y"] -> a 3x3 taking glTF (x, y, z) to the game's."""
    m = np.zeros((3, 3))
    for row, spec in enumerate(axes):
        sign = -1.0 if spec.startswith('-') else 1.0
        m[row, 'xyz'.index(spec.lstrip('+-'))] = sign
    return m


def srgb(linear):
    linear = np.clip(np.asarray(linear, np.float64), 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * np.power(linear, 1 / 2.4) - 0.055)


def tangents(pos, nrm, uv, tris):
    """Per-vertex tangents (x, y, z, handedness) from the texture
    coordinates (Lengyel), for the normal-mapped shaders."""
    n = len(pos)
    tan = np.zeros((n, 3))
    bit = np.zeros((n, 3))
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    ua, ub, uc = uv[tris[:, 0]].astype(np.float64), uv[tris[:, 1]].astype(np.float64), uv[tris[:, 2]].astype(np.float64)
    e1, e2 = b - a, c - a
    d1, d2 = ub - ua, uc - ua
    det = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(det) > 1e-12, 1.0 / np.where(np.abs(det) > 1e-12, det, 1.0), 0.0)
    t = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) * r[:, None]
    bt = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) * r[:, None]
    for k in range(3):
        np.add.at(tan, tris[:, k], t)
        np.add.at(bit, tris[:, k], bt)
    # Gram-Schmidt against the normal
    t_ = tan - nrm * (nrm * tan).sum(1, keepdims=True)
    ln = np.linalg.norm(t_, axis=1, keepdims=True)
    bad = ln[:, 0] < 1e-9
    t_ = np.where(bad[:, None], np.cross(nrm, np.array([0.0, 0.0, 1.0])), t_)
    ln = np.linalg.norm(t_, axis=1, keepdims=True)
    bad = ln[:, 0] < 1e-9
    t_ = np.where(bad[:, None], np.cross(nrm, np.array([1.0, 0.0, 0.0])), t_)
    t_ /= np.maximum(np.linalg.norm(t_, axis=1, keepdims=True), 1e-9)
    w = np.where((np.cross(nrm, t_) * bit).sum(1) < 0, -1.0, 1.0)
    return np.concatenate([t_, w[:, None]], axis=1).astype(np.float32)


def load_png(path, side):
    from PIL import Image
    img = Image.open(path).convert('RGBA')
    w, h = img.size
    if max(w, h) > side:
        f = side / max(w, h)
        img = img.resize((max(4, int(round(w * f))), max(4, int(round(h * f)))), Image.LANCZOS)
    return np.asarray(img).astype(np.uint8)


def flat_rgba(rgba, side=8):
    img = np.zeros((side, side, 4), np.uint8)
    img[..., :] = rgba
    return img


def encode(rgba, side):
    fmt, levels = maptex.encode_levels(rgba, side)
    return (levels[0][0], levels[0][1], fmt, [lv[2] for lv in levels])


class Builder:
    def __init__(self, spec, gltf_path, pattern):
        self.spec = spec
        self.name = spec['name']
        self.g = gltf_read.Gltf(gltf_path)
        self.lib = pcwrite.ShaderLibrary([pattern])
        self.textures = {}  # dictionary name -> encoded
        self.tex_of = {}    # (image path, side) -> dictionary name
        self.log = []

    # -- geometry
    def gather(self):
        prims = self.g.primitives()
        m = axes_matrix(self.spec['axes'])
        flip = np.linalg.det(m) < 0
        wheels = self.spec['wheels']  # bone -> node name
        groups = collections.defaultdict(list)
        for p in prims:
            p['pos'] = p['pos'] @ m.T
            if p['nrm'] is not None:
                p['nrm'] = p['nrm'] @ m.T
            if flip:
                p['tris'] = p['tris'][:, ::-1]
            key = 'body'
            for bone, node in wheels.items():
                if any(node == step for step in p['path']):
                    key = bone
                    break
            if any(pat in step for pat in self.spec.get('skip', []) for step in p['path']):
                continue
            groups[key].append(p)
        allpos = np.concatenate([p['pos'] for ps in groups.values() for p in ps])
        length = allpos[:, 1].max() - allpos[:, 1].min()
        scale = self.spec['length'] / length
        for ps in groups.values():
            for p in ps:
                p['pos'] = p['pos'] * scale
        self.log.append('scale %.4f: the model %.3f long' % (scale, length))
        # the wheels' centres, the origin
        centres = {}
        for bone in WHEEL_BONES:
            if bone in groups:
                pos = np.concatenate([p['pos'] for p in groups[bone]])
                lo, hi = pos.min(0), pos.max(0)
                centres[bone] = (lo + hi) / 2
                self.log.append('%s: centre %s, radius %.3f, width %.3f' % (
                    bone, np.round(centres[bone], 3), (hi[2] - lo[2]) / 2, hi[0] - lo[0]))
        assert centres, 'no wheels found by their node names'
        hub_z = float(np.mean([c[2] for c in centres.values()]))
        front = [c[1] for b, c in centres.items() if b.endswith('f')]
        rear = [c[1] for b, c in centres.items() if b.endswith('r')]
        origin = np.array([0.0, (np.mean(front) + np.mean(rear)) / 2 if front and rear else 0.0,
                           hub_z + float(self.spec.get('origin', {}).get('z_over_hubs', 0.15))])
        body = np.concatenate([p['pos'] for p in groups['body']])
        self.log.append('origin at %s (hubs %.3f up); body %s .. %s' % (
            np.round(origin, 3), hub_z, np.round(body.min(0) - origin, 3), np.round(body.max(0) - origin, 3)))
        for ps in groups.values():
            for p in ps:
                p['pos'] = p['pos'] - origin
        for b in centres:
            centres[b] = centres[b] - origin
        self.groups, self.centres = groups, centres

    # -- materials
    def kind_of(self, mat):
        name = mat.get('name', '')
        for frag, kind in self.spec.get('materials', {}).items():
            if frag.lower() in name.lower():
                return kind
        pbr = mat.get('pbrMetallicRoughness', {})
        if 'glass' in name.lower():
            return 'glass'
        if mat.get('alphaMode') == 'BLEND' and 'baseColorTexture' in pbr:
            return 'mesh'
        return 'generic'

    def texture(self, path, side, kind='c'):
        key = (path, side, kind)
        if key in self.tex_of:
            return self.tex_of[key]
        name = '%s_%s%d' % (self.name[:6], kind, len(self.textures))
        self.textures[name] = encode(load_png(path, side), side)
        self.tex_of[key] = name
        return name

    def flat(self, rgba):
        key = ('flat', tuple(int(x) for x in rgba))
        if key in self.tex_of:
            return self.tex_of[key]
        name = '%s_f%d' % (self.name[:6], len(self.textures))
        self.textures[name] = encode(flat_rgba(rgba), 8)
        self.tex_of[key] = name
        return name

    def shaders(self, d):
        """One shader per glTF material used; returns {material index: (shader index, kind)}."""
        used = sorted({p['material'] for ps in self.groups.values() for p in ps if p['material'] is not None})
        side = int(self.spec.get('texture_side', 1024))
        out = {}
        flat_n = None
        for mi in used:
            mat = self.g.material(mi)
            pbr = mat.get('pbrMetallicRoughness', {})
            kind = self.kind_of(mat)
            sps, fallback = SHADER_OF[kind]
            textures = {}
            if kind == 'glass':
                textures[DIFFUSE] = 'vehicle_generic_glasswindows2'
            elif kind in ('generic', 'mesh'):
                base = pbr.get('baseColorTexture')
                if base is not None:
                    textures[DIFFUSE] = self.texture(self.g.image_path(base['index']), side)
                else:
                    f = pbr.get('baseColorFactor', [0.5, 0.5, 0.5, 1.0])
                    rgb = (srgb(f[:3]) * 255).round().astype(int)
                    textures[DIFFUSE] = self.flat((rgb[0], rgb[1], rgb[2], 255))
                nt = mat.get('normalTexture')
                if nt is not None:
                    textures[NORMAL] = self.texture(self.g.image_path(nt['index']), side, 'n')
                else:
                    if flat_n is None:
                        flat_n = self.flat((128, 128, 255, 255))
                    textures[NORMAL] = flat_n
            idx = d.shader(sps, textures, fallback=fallback)
            out[mi] = (idx, kind)
            self.log.append('material %s -> %s (%s)%s' % (
                mat.get('name'), kind, sps, ''.join(', %08X %s' % (k, v) for k, v in textures.items())))
        return out

    # -- the drawables
    def geometry(self, d, prims, shaders, offset, bones):
        """The primitives merged by material into geometries of `d`."""
        by_mat = collections.defaultdict(list)
        for p in prims:
            by_mat[p['material']].append(p)
        total = 0
        for mi, ps in sorted(by_mat.items(), key=lambda kv: -1 if kv[0] is None else kv[0]):
            pos = np.concatenate([p['pos'] for p in ps]) - offset
            nrm = np.concatenate([p['nrm'] if p['nrm'] is not None else np.tile([0, 0, 1.0], (len(p['pos']), 1))
                                  for p in ps])
            uv = np.concatenate([p['uv'] if p['uv'] is not None else np.zeros((len(p['pos']), 2), np.float32)
                                 for p in ps])
            tris, at = [], 0
            for p in ps:
                tris.append(p['tris'] + at)
                at += len(p['pos'])
            tris = np.concatenate(tris)
            col = np.full((len(pos), 4), 255, np.uint8)
            tan = tangents(pos, nrm, uv, tris)
            shader = shaders[mi][0] if mi in shaders else 0
            d.geometry(0, shader, pos.astype(np.float32), nrm.astype(np.float32), col, [uv.astype(np.float32)],
                       tris, tan=tan, bones=bones)
            total += len(tris)
        return total

    def build(self, out):
        os.makedirs(out, exist_ok=True)
        self.gather()
        body = pcwrite.Drawable(self.lib)
        shaders = self.shaders(body)
        skeleton = [('chassis', -1, (0.0, 0.0, 0.0))]
        for bone in WHEEL_BONES:
            if bone in self.centres:
                skeleton.append((bone, 0, tuple(float(x) for x in self.centres[bone])))
        n_body = self.geometry(body, self.groups['body'], shaders, np.zeros(3), [0])
        wheel = None
        wheel_bone = self.spec.get('wheel_mesh', 'wheel_lf')
        if wheel_bone in self.groups:
            wheel = pcwrite.Drawable(self.lib)
            wheel.shaders = body.shaders
            n_wheel = self.geometry(wheel, self.groups[wheel_bone], shaders, self.centres[wheel_bone], None)
            self.log.append('wheel mesh from %s: %d triangles' % (wheel_bone, n_wheel))
        bone_index = [b[0] for b in skeleton].index(wheel_bone) if wheel is not None else None
        frag = pcwrite.Fragment(body, skeleton, wheel, bone_index)
        wft = os.path.join(out, self.name + '.wft')
        info = frag.write(wft, self.pattern)
        self.log.append('%s.wft: %d body triangles in %d geometries, %d shaders, radius %.2f, virt %d phys %d' % (
            self.name, n_body, len(body.lods[0]), len(body.shaders), info['radius'], info['vsize'], info['psize']))
        pcwrite.Dictionary(self.textures).write(os.path.join(out, self.name + '.wtd'))
        self.log.append('%s.wtd: %d textures' % (self.name, len(self.textures)))
        # read back
        r = rsc5.Resource.pc(wft)
        f = rsc5.read_fragtype(r)
        dr = f['drawable']
        assert dr and dr['shader_group'] and dr['lods'][0], 'the fragment does not read back'
        assert f['child_count'] == (1 if wheel is not None else 0), f['child_count']
        if wheel is not None:
            assert f['children'][0]['drawable'] and f['children'][0]['drawable']['lods'], 'the wheel child does not read back'
        return wft, os.path.join(out, self.name + '.wtd')


def main():
    spec_path, gltf_path, pattern, export, work, out = sys.argv[1:7]
    spec = json.load(open(spec_path))
    b = Builder(spec, gltf_path, pattern)
    b.pattern = pattern
    wft, wtd = b.build(out)
    for line in b.log:
        print(line)
    # the console conversion, as run_vehicle.py does it
    options = []
    seats = spec.get('seats', {})
    if seats:
        options.append('points=' + ';'.join('%s=%.3f:%.3f:%.3f' % (k, *v) for k, v in seats.items()))
    radii = spec.get('radii', {})
    if radii:
        options.append('radii=' + ';'.join('%s:%.3f' % (k, v) for k, v in radii.items()))
    if spec.get('options'):
        options.append(spec['options'])
    sys.argv = ['run_vehicle.py', export, work, wft, wtd, out, spec['name'], spec.get('template', 'fbi'),
                spec.get('pivots', '-'), spec.get('sources', '-'), ','.join(options) or '-']
    print('run_vehicle:', ' '.join(sys.argv[1:]))
    import run_vehicle
    run_vehicle.main()


if __name__ == '__main__':
    main()
