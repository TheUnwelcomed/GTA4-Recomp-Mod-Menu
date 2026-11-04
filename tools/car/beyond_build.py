"""The Batman Beyond Batmobile as a vehicle file (v1.7.43): beyond_body.py's
mesh written as a PC vehicle (pcwrite.Fragment: the body, glass, jets, tub
and red lines as skinned geometries on a chassis bone - the glass on a
canopy bone of its own at the bubble's rear edge - the shaders cloned from
a PC car mod's samples, a small texture dictionary of its own flat maps),
then put through the console converter on a stock car's template like any
PC mod (batconv.Converter: the template's physics, the body's meshes, no
wheel meshes so the template's are hidden; the canopy bone's glass bound to
the template's interiorlight bone moved to the hinge, as the Batwing's
canopy is, so the trainer can swing it open; the seats where the pilot fits
under the canopy's crown).

usage: python3 -I beyond_build.py <pattern.wft> <export_dir> <work_dir> <out_dir> [template]
  pattern.wft  a PC car mod's file: the fragment header's pattern and the
               shader samples (its .wtd is not needed: the generic textures
               are the game's own vehshare, the rest are made here)
  export_dir   the stock 360 files (the trainer build's logs/export)
  work_dir     their decompressed bodies (<car>.xft.bin, .xtd.bin)
  out_dir      beyond.wft, beyond.wtd (the PC files), beyond.xft.bin,
               beyond.xtd.bin and flags.txt (for xlzx rsc), beyond.png
  template     the stock car to build on (default: infernus)
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, os.path.join(HERE, '..', 'maps'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import beyond_body  # noqa: E402
import maptex  # noqa: E402
import pcwrite  # noqa: E402
import rsc5  # noqa: E402

DIFFUSE, DIRT, NORMAL, SPEC = 0x2B5170FD, 0x7E9A27FE, 0x46B7C64F, 0x608799C6  # the vehicle shaders' samplers
SHADER_OF = {  # part -> (.sps, fallback .sps, textures)
    'top': ('gta_vehicle_paint1.sps', 'gta_vehicle_paint3.sps', {}),
    'bottom': ('gta_vehicle_paint1.sps', 'gta_vehicle_paint3.sps', {}),
    'notch': ('gta_vehicle_generic.sps', None, {DIFFUSE: 'beyond_dark', NORMAL: 'beyond_flat_n'}),
    'pod': ('gta_vehicle_paint1.sps', 'gta_vehicle_paint3.sps', {}),
    'jet': ('gta_vehicle_generic.sps', None, {DIFFUSE: 'beyond_jet', NORMAL: 'beyond_flat_n'}),
    'accent': ('gta_vehicle_generic.sps', None, {DIFFUSE: 'beyond_red', NORMAL: 'beyond_flat_n'}),
    # (the glass map the stock cars share: a dark tint, in vehshare)
    'glass': ('gta_vehicle_vehglass.sps', None, {DIFFUSE: 'vehicle_generic_glasswindows2'}),
}


def smooth_normals(verts, tris):
    """Per-vertex normals: the area-weighted mean of the faces round each."""
    n = np.zeros_like(verts, dtype=np.float64)
    a, b, c = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
    fn = np.cross(b - a, c - a)
    for k in range(3):
        np.add.at(n, tris[:, k], fn)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    return (n / np.maximum(ln, 1e-9)).astype(np.float32)


def weld(verts, tris, tol=1e-4):
    key = np.round(verts / tol).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inv = inv.reshape(-1)
    v = verts[first]
    t = inv[tris]
    ok = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    return v, t[ok]


def flat_texture(rgba, side=8):
    img = np.zeros((side, side, 4), np.uint8)
    img[..., :] = rgba
    fmt, levels = maptex.encode_levels(img, side)
    return (levels[0][0], levels[0][1], fmt, [lv[2] for lv in levels])


def main():
    pattern, export, work, out = sys.argv[1:5]
    template = sys.argv[5] if len(sys.argv) > 5 else 'infernus'
    os.makedirs(out, exist_ok=True)
    lib = pcwrite.ShaderLibrary([pattern])
    d = pcwrite.Drawable(lib)
    shaders = {}
    for key, (sps, fallback, textures) in SHADER_OF.items():
        shaders[key] = d.shader(sps, textures, fallback=fallback)
    parts = beyond_body.parts()
    total = 0
    for name, (verts, tris) in parts.items():
        key = name.split('_')[0]
        v, t = weld(np.asarray(verts, np.float64), np.asarray(tris, np.int64))
        nrm = smooth_normals(v, t)
        # texture coordinates: the plan, 0.25 m a repeat (the flat maps do not mind)
        uv = np.stack([v[:, 0] * 0.4 + 0.5, 0.5 - v[:, 1] * 0.4], axis=1).astype(np.float32)
        col = np.full((len(v), 4), 255, np.uint8)
        # tangents along x for the normal-mapped shaders (a flat normal map: the handedness is all that matters)
        tan = np.zeros((len(v), 4), np.float32)
        tan[:, 0] = 1.0
        tan[:, 3] = 1.0
        # (the glass rides the canopy bone; everything else the chassis)
        d.geometry(0, shaders[key], v.astype(np.float32), nrm, col, [uv], t, tan=tan, bones=[1 if key == 'glass' else 0])
        total += len(t)
    hinge = beyond_body.hinge()
    skeleton = [('chassis', -1, (0.0, 0.0, 0.0)), ('canopy', 0, hinge)]
    frag = pcwrite.Fragment(d, skeleton)
    info = frag.write(os.path.join(out, 'beyond.wft'), pattern)
    print('beyond.wft: %d triangles in %d parts, radius %.2f, virt %d phys %d' % (
        total, len(parts), info['radius'], info['vsize'], info['psize']))
    textures = {
        'beyond_dark': flat_texture((22, 22, 26, 255)),
        'beyond_jet': flat_texture((70, 28, 24, 255)),
        'beyond_red': flat_texture((225, 20, 12, 255)),
        'beyond_flat_n': flat_texture((128, 128, 255, 255)),
    }
    pcwrite.Dictionary(textures).write(os.path.join(out, 'beyond.wtd'))
    # read back as a mod's file would be
    r = rsc5.Resource.pc(os.path.join(out, 'beyond.wft'))
    f = rsc5.read_fragtype(r)
    dr = f['drawable']
    assert dr and dr['shader_group'] and dr['lods'][0], 'the fragment does not read back'
    print('read back: %d shaders, %d geometries, skeleton at %X, children %d' % (
        len(dr['shader_group']['shaders']), sum(len(m['geometries']) for m in dr['lods'][0]), dr['skeleton_off'],
        f['child_count']))
    # the console conversion, as run_vehicle.py does it
    # (no shader sources beyond the template: the mod sample's paint3, a
    # livery paint, falls back to the template's paint1 - the plain colour)
    pivots = 'interiorlight@%.3f,%.3f,%.3f=canopy' % hinge
    seats = ';'.join('%s=%.2f:%.2f:%.2f' % (name, *at) for name, at in zip(('seat_dside_f', 'seat_pside_f'),
                                                                            beyond_body.SEATS))
    sys.argv = ['run_vehicle.py', export, work, os.path.join(out, 'beyond.wft'), os.path.join(out, 'beyond.wtd'), out,
                'beyond', template, pivots, '-', 'points=' + seats]
    import run_vehicle
    run_vehicle.main()


if __name__ == '__main__':
    main()
