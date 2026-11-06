"""A GTA V vehicle mod as a console add-on car (v1.7.47): the mod's own
model and textures, written as an IV PC vehicle (pcwrite.Fragment) and put
through the console converter on a stock car's template like any PC mod
(run_vehicle). The 1969 Dodge Charger pack (OhiOcinu et al.): specs/charger69.json.

What is taken from the V files:
  - the fragment's main drawable (the LOD the spec asks for: the non-hi
    .yft's LOD 0 is a console-sized mesh), its skeleton (V's vehicle bones
    carry IV's names: chassis, door_dside_f, bonnet, boot, wheel_lf,
    steeringwheel, headlight_l...), each vertex's bone (V vehicles are
    rigid parts: one bone a vertex) - the geometries cut by bone, so the
    converter hangs each part on the stock bone of that name, swings the
    doors on their hinges and draws the rest rigid on the chassis;
  - the wheel mesh from the fragment's physics children (the child on
    wheel_lf, in the wheel's own space), as the fragment's wheel child;
  - the shaders by V preset: vehicle_paint* -> paint (the car's colour),
    vehicle_vehglass* -> glass, vehicle_tire -> tire, vehicle_mesh and
    vehicle_badges -> mesh, vehicle_interior* -> interior,
    vehicle_lightsemissive -> lights, vehicle_licenseplate -> generic (a
    plate drawn on the spec's plate texture), the rest generic; the colour,
    normal and specular maps by their sampler names, from the mod's .ytd
    (V's shared vehicle textures the mod names - black, chrome, alu,
    blank_normal... - stand in as flat colours: SHARED);
  - the doors' travel from the stock template's doors (the limits the
    converter needs to swing them).

usage: python3 -I v_car.py [--assets=<dir>] <spec.json> <model.yft> <textures.ytd> <pattern.wft> <export_dir> <work_dir> <out_dir>
  --assets     the folder the spec's PNGs (plate, livery) are in when their
               paths are relative (default: the spec's own folder); the
               General Lee's are the user's livery pack's, not in this
               repository
  spec.json    name, template, lod, texture_side, seats {bone: [x, y, z]},
               colours "r,g,b", plate (a PNG for the plate), livery {V
               texture name: PNG} (a texture of the mod's replaced by a
               file: the General Lee's decal sheets), kinds {V shader index
               or sampler texture name: kind} overrides, options
  pattern.wft  a PC car mod's file: the fragment header's pattern and the
               shader samples
  export_dir   the stock 360 files (the trainer build's logs/export)
  work_dir     their decompressed bodies (<car>.xft.bin, .xtd.bin)
  out_dir      <name>.wft, <name>.wtd, the converted <name>.xft.bin,
               <name>.xtd.bin and flags.txt (for xlzx rsc)
"""
import collections
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, os.path.join(HERE, '..', 'maps'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import dxt  # noqa: E402
import gltf_car  # noqa: E402
import maptex  # noqa: E402
import pcwrite  # noqa: E402
import rsc5  # noqa: E402
import v_reader  # noqa: E402

DIFFUSE, DIRT, NORMAL, SPEC = gltf_car.DIFFUSE, gltf_car.DIRT, gltf_car.NORMAL, gltf_car.SPEC
V_DIFFUSE, V_BUMP, V_SPEC, V_DIRT = 0xF1FE2B71, 0x46B7C64F, 0x608799C6, 0x7E9A27FE
V_DIFFUSE2, V_PLATE_BG = 0x0AD3A268, 0x50022388
LIVERY = 0x05645204   # IV paint3's livery sampler (the police's livery, the taxi's detail)
SHADER_OF = dict(gltf_car.SHADER_OF)
SHADER_OF.update({
    'tire': ('gta_vehicle_tire.sps', 'gta_vehicle_generic.sps'),
    'interior': ('gta_vehicle_interior.sps', 'gta_vehicle_generic.sps'),
    'badges': ('gta_vehicle_badges.sps', 'gta_vehicle_mesh.sps'),
})
# V shader name (its hash) -> kind
KIND_OF_V = {}
for _n, _k in (('vehicle_paint1', 'paint'), ('vehicle_paint2', 'paint'), ('vehicle_paint3', 'paint'),
               ('vehicle_paint4', 'paint'), ('vehicle_paint5', 'paint'), ('vehicle_paint6', 'paint'),
               ('vehicle_paint7', 'paint'), ('vehicle_paint8', 'paint'), ('vehicle_paint9', 'paint'),
               ('vehicle_paint1_enveff', 'paint'), ('vehicle_paint2_enveff', 'paint'), ('vehicle_paint3_enveff', 'paint'),
               ('vehicle_paint4_enveff', 'paint'), ('vehicle_paint3_lvr', 'paint'), ('vehicle_paint4_emissive', 'paint'),
               ('vehicle_paint1_lvr', 'paint'), ('vehicle_paint2_lvr', 'paint'),
               ('vehicle_vehglass', 'glass'), ('vehicle_vehglass_inner', 'glass'),
               ('vehicle_tire', 'tire'), ('vehicle_tire_emissive', 'tire'),
               ('vehicle_mesh', 'mesh'), ('vehicle_mesh2_enveff', 'mesh'), ('vehicle_mesh_enveff', 'mesh'),
               ('vehicle_badges', 'badges'), ('vehicle_decal', 'badges'), ('vehicle_decal2', 'badges'),
               ('vehicle_interior', 'interior'), ('vehicle_interior2', 'interior'),
               ('vehicle_lightsemissive', 'lights'), ('vehicle_dash_emissive', 'lights'),
               ('vehicle_dash_emissive_opaque', 'lights'), ('vehicle_emissive_opaque', 'lights'),
               ('vehicle_licenseplate', 'plate'), ('vehicle_generic', 'generic'), ('vehicle_detail', 'generic'),
               ('vehicle_detail2', 'generic'), ('vehicle_shuts', 'generic'), ('vehicle_basic', 'generic'),
               ('vehicle_cloth', 'generic'), ('vehicle_cloth2', 'generic'), ('vehicle_cutout', 'mesh')):
    KIND_OF_V[v_reader.joaat(_n)] = _k
# V's shared vehicle textures (vehshare) the mods name: flat stand-ins (R, G, B, A)
SHARED = {
    'black': (12, 12, 12, 255), 'dinghyblack': (10, 10, 10, 255), 'chrome': (205, 205, 210, 255),
    'alu': (150, 150, 152, 255), 'alu_satin': (140, 140, 142, 255), 'plastic': (28, 28, 28, 255),
    'blank_normal': (128, 128, 255, 255), 'vehicle_generic_smallspecmap': (90, 90, 90, 255),
    'vehicle_generic_glasswindows2': (20, 24, 28, 90), 'glass': (20, 24, 28, 90), 'blk_tint': (8, 8, 10, 160),
    'mud': (60, 50, 40, 0), 'windshield_dirt2': (60, 50, 40, 0), 'charger_dirt': (60, 50, 40, 0),
    'emis': (255, 230, 200, 255), 'plate01': (200, 200, 200, 255), 'plate01_n': (128, 128, 255, 255),
    's2': (70, 70, 70, 255), 's3': (90, 90, 90, 255), 's4': (50, 50, 50, 255), 'spec_l': (60, 60, 60, 255),
    'spec_m': (80, 80, 80, 255), 'leather_s': (40, 40, 40, 255), 'alu_s': (120, 120, 120, 255),
    'plastic_spec': (40, 40, 40, 255), 'carbon_mesh_shader_spec': (60, 60, 60, 255),
    'vehicle_generic_plate_font': (0, 0, 0, 0), 'vehicle_generic_plate_font_n': (128, 128, 255, 0),
}
WHEEL_BONES = ('wheel_lf', 'wheel_rf', 'wheel_lr', 'wheel_rr')
DOOR_NAMES = ('door_dside_f', 'door_pside_f', 'door_dside_r', 'door_pside_r')


# ---------------------------------------------------------------- the V files
def to_rgba(t):
    """A V texture's level 0 as (h, w, 4) RGBA."""
    w, h, fmt, data = t['w'], t['h'], t['fmt'], t['data']
    lv = data[:v_reader.level_size(fmt, w, h)]
    if fmt == 'DXT1':
        return np.ascontiguousarray(dxt.decode_dxt1(lv, w, h)[:h, :w])
    if fmt in ('DXT3', 'DXT5'):
        return np.ascontiguousarray(dxt.decode_dxt5(lv, w, h)[:h, :w])
    if fmt in ('A8R8G8B8', 'X8R8G8B8'):
        a = np.frombuffer(lv, np.uint8).reshape(h, w, 4)
        rgba = a[..., [2, 1, 0, 3]].copy()
        if fmt == 'X8R8G8B8':
            rgba[..., 3] = 255
        return rgba
    if fmt == 'A8B8G8R8':
        return np.frombuffer(lv, np.uint8).reshape(h, w, 4).copy()
    if fmt == 'L8':
        a = np.frombuffer(lv, np.uint8).reshape(h, w)
        return np.dstack([a, a, a, np.full_like(a, 255)])
    raise ValueError('texture format %s' % fmt)


def fragment_children(r):
    """The fragment's physics children (the first physics LOD): [(bone tag,
    drawable pointer)] for those with a drawable."""
    out = []
    lodg = r.u64(0x500000F0)
    if (lodg >> 28) != 5:
        return out
    lod1 = r.u64(lodg + 0x10)
    if (lod1 >> 28) != 5:
        return out
    children = r.u64(lod1 + 0xD0)
    count = r.u8(lod1 + 0x11A)
    if (children >> 28) != 5:
        return out
    for i in range(count):
        c = r.u64(children + 8 * i)
        if (c >> 28) != 5:
            continue
        tag = r.u16(c + 0x12)  # (+0x10 is the child's index; the bone tag follows)
        for off in (0xA0, 0xA8):
            dp = r.u64(c + off)
            if (dp >> 28) == 5:
                out.append((tag, dp))
                break
    return out


def read_vehicle(path):
    r = v_reader.Resource.open(path)
    dr = r.u64(0x50000030)
    main = v_reader.read_drawable(r, dr, with_name=False)
    bones = v_reader.read_skeleton(r, dr)
    tag_index = {b['tag']: i for i, b in enumerate(bones)}
    children = []
    for tag, dp in fragment_children(r):
        try:
            d = v_reader.read_drawable(r, dp, with_name=False)
        except Exception:
            continue
        n = sum(g['vertex_count'] for lod in d['lods'] for m in lod for g in m['geometries'])
        if n:
            children.append(dict(bone=tag_index.get(tag), tag=tag, drawable=d))
    return r, main, bones, children


# ---------------------------------------------------------------- the build
class Builder:
    def __init__(self, spec, yft, ytd, pattern):
        self.spec = spec
        self.name = spec['name']
        self.r, self.main, self.bones, self.children = read_vehicle(yft)
        self.ytd = v_reader.read_ytd(ytd)
        self.lib = pcwrite.ShaderLibrary([pattern])
        self.pattern = pattern
        self.textures = {}
        self.tex_of = {}
        self.log = []
        self.missing = collections.Counter()

    # -- textures
    def flat(self, rgba):
        key = ('flat', tuple(int(x) for x in rgba))
        if key in self.tex_of:
            return self.tex_of[key]
        name = '%s_f%d' % (self.name[:6], len(self.textures))
        self.textures[name] = gltf_car.encode(gltf_car.flat_rgba(rgba), 8)
        self.tex_of[key] = name
        return name

    def texture(self, vname, kind='c', side=None):
        """The mod's texture under its own name (a safe one), at the spec's
        size; a shared V texture as a flat stand-in; a file from the spec's
        livery table in place of the mod's."""
        if vname is None:
            return None
        key = (vname.lower(), kind)
        if key in self.tex_of:
            return self.tex_of[key]
        side = side or int(self.spec.get('texture_side', 1024))
        livery = self.spec.get('livery', {})
        name = None
        if vname in livery:
            rgba = gltf_car.load_png(livery[vname], int(self.spec.get('livery_side', 2048)))
            name = '%s_%s' % (self.name[:6], os.path.splitext(os.path.basename(livery[vname]))[0][:16].lower())
            self.textures[name] = gltf_car.encode(rgba, int(self.spec.get('livery_side', 2048)))
        elif vname.lower() in self.ytd and self.ytd[vname.lower()].get('data') is not None:
            t = self.ytd[vname.lower()]
            rgba = to_rgba(t)
            if max(rgba.shape[:2]) > side:
                from PIL import Image
                img = Image.fromarray(rgba, 'RGBA')
                f = side / max(rgba.shape[:2])
                rgba = np.asarray(img.resize((max(4, int(round(rgba.shape[1] * f))), max(4, int(round(rgba.shape[0] * f)))),
                                             Image.LANCZOS)).astype(np.uint8)
            safe = ''.join(c if c.isalnum() else '_' for c in vname.lower())[:24]
            name = '%s_%s' % (self.name[:6], safe)
            self.textures[name] = gltf_car.encode(rgba, side)
        elif vname.lower() in SHARED:
            name = self.flat(SHARED[vname.lower()])
        else:
            self.missing[vname] += 1
            name = self.flat((128, 128, 255, 255) if kind == 'n' else (110, 110, 110, 255))
        self.tex_of[key] = name
        return name

    # -- shaders
    def kind_of(self, si, s):
        kinds = self.spec.get('kinds', {})
        if str(si) in kinds:
            return kinds[str(si)]
        diff = s['params'].get(V_DIFFUSE)
        diff_name = diff[1]['name'] if diff and diff[0] == 'texture' and diff[1] else ''
        if diff_name and diff_name.lower() in kinds:
            return kinds[diff_name.lower()]
        return KIND_OF_V.get(s['name_hash'], 'generic')

    def shaders(self, d):
        """One IV shader per V shader used; returns {V shader index: (IV index, kind)}."""
        used = sorted({g['shader'] for m in self.main['lods'][self.lod] for g in m['geometries']} |
                      {g['shader'] for c in self.children for m in c['drawable']['lods'][0] for g in m['geometries']})
        out = {}
        for si in used:
            s = self.main['shaders'][si]
            kind = self.kind_of(si, s)
            sps, fallback = SHADER_OF.get(kind, SHADER_OF['generic'])

            def tex(hash_):
                p = s['params'].get(hash_)
                return p[1]['name'] if p and p[0] == 'texture' and p[1] and p[1].get('name') else None
            textures = {}
            if kind == 'glass':
                textures[DIFFUSE] = 'vehicle_generic_glasswindows2'
            elif kind == 'paint':
                # the colour comes from the car's colour slots; a livery (DiffuseSampler2) rides on paint3
                if tex(V_DIFFUSE2):
                    # (IV's paint3 draws its livery sampler over the colour; the
                    # diffuse slot stays the sample's specular placeholder)
                    sps, fallback = 'gta_vehicle_paint3.sps', 'gta_vehicle_paint1.sps'
                    textures[LIVERY] = self.texture(tex(V_DIFFUSE2))
                else:
                    # (paint3 standing in for paint1 gets a clear livery: nothing over the colour)
                    textures[LIVERY] = self.flat((0, 0, 0, 0))
                if tex(V_BUMP) and tex(V_BUMP).lower() != 'blank_normal':
                    textures[NORMAL] = self.texture(tex(V_BUMP), 'n')
            elif kind == 'plate':
                sps, fallback = SHADER_OF['generic']
                plate = self.spec.get('plate')
                if plate:
                    self.spec.setdefault('livery', {})[os.path.basename(plate)] = plate
                    textures[DIFFUSE] = self.texture(os.path.basename(plate))
                else:
                    textures[DIFFUSE] = self.texture(tex(V_PLATE_BG) or 'plate01')
                textures[NORMAL] = self.flat((128, 128, 255, 255))
            else:
                textures[DIFFUSE] = self.texture(tex(V_DIFFUSE)) or self.flat((110, 110, 110, 255))
                if kind in ('mesh', 'generic', 'badges', 'interior', 'tire'):
                    textures[NORMAL] = self.texture(tex(V_BUMP), 'n') if tex(V_BUMP) else self.flat((128, 128, 255, 255))
                if tex(V_SPEC) and kind in ('mesh', 'generic', 'tire', 'interior'):
                    textures[SPEC] = self.texture(tex(V_SPEC), 's')
            idx = d.shader(sps, textures, fallback=fallback)
            out[si] = (idx, kind)
            self.log.append('shader %d %08X -> %s (%s)%s' % (
                si, s['name_hash'], kind, sps, ''.join(', %08X %s' % (k, v) for k, v in textures.items())))
        return out

    # -- geometry
    def geometries(self, d, drawable, lod_index, shaders, bone_of_vertex, offset=None):
        """The drawable's LOD as geometries of `d`, each on one bone (the
        vertices cut by bone) when `bone_of_vertex`, else rigid."""
        total = 0
        for m in drawable['lods'][lod_index]:
            for g in m['geometries']:
                c = g['components']
                pos = c['position'][:, :3].astype(np.float64)
                if offset is not None:
                    pos = pos - offset
                nrm = c['normal'][:, :3].astype(np.float64) if 'normal' in c else np.tile([0.0, 0.0, 1.0], (len(pos), 1))
                uv = c['texcoord0'][:, :2].astype(np.float32) if 'texcoord0' in c else np.zeros((len(pos), 2), np.float32)
                col = np.full((len(pos), 4), 255, np.uint8)
                if 'colour0' in c and c['colour0'].shape[1] == 4:
                    col = np.clip(c['colour0'] * 255.0 + 0.5, 0, 255).astype(np.uint8)
                tris = g['indices'].reshape(-1, 3).astype(np.int64)
                shader = shaders[g['shader']][0] if g['shader'] in shaders else 0
                if 'tangent' in c and c['tangent'].shape[1] >= 4 and np.isfinite(c['tangent']).all():
                    tan = c['tangent'][:, :4].astype(np.float32)
                else:
                    tan = gltf_car.tangents(pos, nrm, uv, tris)
                groups = {None: np.arange(len(pos))}
                if bone_of_vertex:
                    bi = np.rint(c['blendindices'] * 255.0).astype(np.int64)
                    ids = g['bone_ids']
                    if ids:
                        bi = np.array(ids, np.int64)[np.clip(bi, 0, len(ids) - 1)]
                    main = bi[:, 0]
                    if 'blendweights' in c:
                        main = bi[np.arange(len(bi)), c['blendweights'].argmax(1)]
                    groups = {int(b): np.nonzero(main == b)[0] for b in np.unique(main)}
                for bone, verts in groups.items():
                    keep = np.zeros(len(pos), bool)
                    keep[verts] = True
                    tsel = tris[keep[tris].all(1)]
                    if not len(tsel):
                        continue
                    used = np.unique(tsel)
                    remap = np.full(len(pos), -1, np.int64)
                    remap[used] = np.arange(len(used))
                    d.geometry(0, shader, pos[used].astype(np.float32), nrm[used].astype(np.float32), col[used],
                               [uv[used]], remap[tsel], tan=tan[used], bones=[bone] if bone is not None else None)
                    total += len(tsel)
        return total

    def door_limits(self, export, work):
        """The template's door joints' travel, for the donor's doors."""
        tmpl = self.spec.get('template', 'banshee')
        fl = dict(l.split()[0:3:2] for l in open(os.path.join(work, 'flags.txt'))) if os.path.exists(os.path.join(work, 'flags.txt')) else {}
        body = os.path.join(work, tmpl + '.xft.bin')
        hdr = os.path.join(export, tmpl + '.xft')
        if not (os.path.exists(body) and os.path.exists(hdr)):
            return {}
        raw = open(hdr, 'rb').read(12)
        flags = struct.unpack('>I', raw[8:12])[0]
        r = rsc5.Resource(open(body, 'rb').read(), flags, True, 112, tmpl)
        f = rsc5.read_fragtype(r)
        _, tb = batconv.read_skeleton(r, f['drawable']['skeleton_off'])
        out = {}
        for b in tb:
            if b['name'] in DOOR_NAMES:
                out[b['name']] = batconv.bone_limits(r, b['rec'])
        return out

    def build(self, out, export, work):
        os.makedirs(out, exist_ok=True)
        self.lod = int(self.spec.get('lod', 0))
        body = pcwrite.Drawable(self.lib)
        shaders = self.shaders(body)
        skeleton = [(b['name'], b['parent'], tuple(float(x) for x in b['pos'])) for b in self.bones]
        n_body = self.geometries(body, self.main, self.lod, shaders, True)
        # the wheel: the child on wheel_lf (or the first wheel child)
        wheel, wheel_bone = None, None
        names = [b['name'] for b in self.bones]
        for want in ('wheel_lf', 'wheel_rf', 'wheel_lr', 'wheel_rr'):
            for c in self.children:
                if c['bone'] is not None and names[c['bone']] == want:
                    wheel_bone = c['bone']
                    wheel = pcwrite.Drawable(self.lib)
                    wheel.shaders = body.shaders
                    n_wheel = self.geometries(wheel, c['drawable'], 0, shaders, False)
                    pts = np.concatenate([g['pos'] for g in wheel.lods[0]])
                    self.wheel_radius = float((pts[:, 2].max() - pts[:, 2].min()) / 2)
                    self.log.append('wheel mesh from %s: %d triangles, radius %.3f, width %.3f' % (
                        want, n_wheel, self.wheel_radius, pts[:, 0].max() - pts[:, 0].min()))
                    break
            if wheel is not None:
                break
        limits = self.door_limits(export, work)
        if limits:
            self.log.append('door limits from the template: %s' % ', '.join(sorted(limits)))
        frag = pcwrite.Fragment(body, skeleton, wheel, wheel_bone, limits)
        wft = os.path.join(out, self.name + '.wft')
        info = frag.write(wft, self.pattern)
        self.log.append('%s.wft: %d body triangles in %d geometries, %d shaders, %d bones, radius %.2f, virt %d phys %d' % (
            self.name, n_body, len(body.lods[0]), len(body.shaders), len(skeleton), info['radius'], info['vsize'],
            info['psize']))
        pcwrite.Dictionary(self.textures).write(os.path.join(out, self.name + '.wtd'))
        self.log.append('%s.wtd: %d textures' % (self.name, len(self.textures)))
        if self.missing:
            self.log.append('textures not in the mod\'s dictionary (flat stand-ins): %s' % ', '.join(
                '%s x%d' % kv for kv in self.missing.most_common()))
        r = rsc5.Resource.pc(wft)
        f = rsc5.read_fragtype(r)
        assert f['drawable'] and f['drawable']['lods'][0], 'the fragment does not read back'
        return wft, os.path.join(out, self.name + '.wtd')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    spec_path, yft, ytd, pattern, export, work, out = args[:7]
    spec = json.load(open(spec_path))
    # (the spec's PNGs by their paths, relative ones under --assets or beside the spec)
    assets = opts.get('assets') or os.path.dirname(os.path.abspath(spec_path))
    if spec.get('plate') and not os.path.isabs(spec['plate']):
        spec['plate'] = os.path.join(assets, spec['plate'])
    for k, v in list(spec.get('livery', {}).items()):
        if not os.path.isabs(v):
            spec['livery'][k] = os.path.join(assets, v)
    b = Builder(spec, yft, ytd, pattern)
    wft, wtd = b.build(out, export, work)
    for line in b.log:
        print(line)
    options = []
    seats = spec.get('seats', {})
    if seats:
        options.append('points=' + ';'.join('%s=%.3f:%.3f:%.3f' % (k, *v) for k, v in seats.items()))
    radius = spec.get('wheel_radius', getattr(b, 'wheel_radius', None))
    if radius:
        options.append('radii=' + ';'.join('%s:%.3f' % (k, radius) for k in WHEEL_BONES))
    if spec.get('options'):
        options.append(spec['options'])
    sys.argv = ['run_vehicle.py', export, work, wft, wtd, out, spec['name'], spec.get('template', 'banshee'),
                spec.get('pivots', '-'), spec.get('sources', '-'), ','.join(options) or '-']
    print('run_vehicle:', ' '.join(sys.argv[1:]))
    import run_vehicle
    run_vehicle.main()


if __name__ == '__main__':
    main()
