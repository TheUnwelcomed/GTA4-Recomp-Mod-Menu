"""A V ped mod's textures touched up for the console (v1.7.46), the way the
Batmobile's were (pc2x360/remaster.py): nothing painted, every detail read
from the mod's own maps.

  colour    the mod's map brought to 1024 (Lanczos), its dark tones opened a
            touch (the console's lighting and tone curve crush a near-black
            albedo flat), a fine cloth grain laid over it (a blurred noise at
            +-2%: the matte weave of the game's own ped textures, which are
            photographs) and lightly sharpened;
  normal    the mod's map at 1024, the colour map's fine detail (its
            lightness high-passed into a height field) and the grain added
            as relief at low strength, renormalised;
  specular  the mod's own map when it carries any detail, else made from the
            colour map's detail at the level of the game's own ped specular
            maps (uppr_spec_000: a flat 58/255), seams and dark wear duller.

usage: python3 -I pedtex.py <mod .ydd> <mod .ytd> <out.wtd> <png dir> <diffuse> <normal> <spec>
  the three names are the mod's texture names (embedded in the .ydd or in
  the .ytd); the .wtd holds suit_d, suit_n, suit_s and ph_d (a 64x64 grey
  for the placeholder components).
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, os.path.join(HERE, '..', 'maps'))
import dxt  # noqa: E402
import maptex  # noqa: E402
import pcwrite  # noqa: E402
import remaster  # noqa: E402
import v_reader  # noqa: E402

IV_SPEC_LEVEL = (58, 58, 58)


def to_rgba(t):
    w, h, fmt, data = t['w'], t['h'], t['fmt'], t['data']
    lv = data[:v_reader.level_size(fmt, w, h)]
    if fmt == 'DXT1':
        return dxt.decode_dxt1(lv, w, h)[:h, :w]
    if fmt == 'DXT5':
        return dxt.decode_dxt5(lv, w, h)[:h, :w]
    if fmt in ('A8R8G8B8', 'X8R8G8B8'):
        a = np.frombuffer(lv, np.uint8).reshape(h, w, 4)
        rgba = a[..., [2, 1, 0, 3]].copy()
        if fmt == 'X8R8G8B8':
            rgba[..., 3] = 255
        return rgba
    if fmt == 'L8':
        a = np.frombuffer(lv, np.uint8).reshape(h, w)
        return np.dstack([a, a, a, np.full_like(a, 255)])
    raise ValueError('texture format %s' % fmt)


def mod_textures(ydd_path, ytd_path):
    out = {}
    for d in v_reader.read_ydd(ydd_path).values():
        for name, t in d['embedded_textures'].items():
            if t.get('data') is not None:
                out.setdefault(name.lower(), t)
    for name, t in v_reader.read_ytd(ytd_path).items():
        if t.get('data') is not None:
            out.setdefault(name.lower(), t)
    return out


def grain(side, seed=7, amount=0.02, radius=0.7):
    """A blurred noise field in -amount..amount (a cloth weave's scale)."""
    rng = np.random.default_rng(seed)
    n = rng.standard_normal((side, side)).astype(np.float32)
    img = Image.fromarray(np.clip(n * 40 + 128, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius))
    n = (np.asarray(img, np.float32) - 128.0) / 40.0
    n /= max(1e-6, float(n.std()))
    return np.clip(n, -2.5, 2.5) * amount


def colour_map(rgba, side=1024):
    img = Image.fromarray(np.ascontiguousarray(rgba[..., :3]), 'RGB').resize((side, side), Image.LANCZOS)
    rgb = np.asarray(img, np.float32) / 255.0
    # the dark tones opened (a curve that leaves the mid-tones and highlights where they are)
    rgb = np.power(rgb, 0.86)
    g = grain(side)
    rgb = np.clip(rgb * (1.0 + g[..., None]), 0.0, 1.0)
    sharp = Image.fromarray(np.clip(np.rint(rgb * 255.0), 0, 255).astype(np.uint8)).filter(
        ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))
    alpha = Image.fromarray(np.ascontiguousarray(rgba[..., 3])).resize((side, side), Image.LANCZOS)
    return np.dstack([np.asarray(sharp), np.asarray(alpha)])


def normal_map(nrgba, colour_rgba, side=1024, detail=0.035, grain_relief=0.5):
    """The mod's normal map at `side`, with the colour map's fine detail
    and the grain added as relief."""
    n = np.asarray(Image.fromarray(np.ascontiguousarray(nrgba[..., :3]), 'RGB').resize((side, side), Image.LANCZOS),
                   np.float32) / 255.0 * 2.0 - 1.0
    h, inside = remaster.height_field(colour_rgba, side)
    h = remaster.blur(h + 128.0, 0.6) - 128.0
    h = h + grain(side) * 40.0 * grain_relief
    gx = np.zeros_like(h)
    gy = np.zeros_like(h)
    gx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) * 0.5
    gy[1:-1, :] = (h[2:, :] - h[:-2, :]) * 0.5
    n[..., 0] -= gx * detail
    n[..., 1] -= gy * detail
    n[..., 2] = np.maximum(n[..., 2], 0.05)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    out = np.dstack([(n[..., 0] * 0.5 + 0.5) * 255, (n[..., 1] * 0.5 + 0.5) * 255, (n[..., 2] * 0.5 + 0.5) * 255,
                     np.full(n.shape[:2], 255.0)])
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def specular_map(srgba, colour_rgba, side=512):
    lum = (0.299 * srgba[..., 0] + 0.587 * srgba[..., 1] + 0.114 * srgba[..., 2])
    if float(lum.std()) > 4.0 and float(lum.mean()) > 8.0:
        # the mod's own map, sharpened at the console's size
        img = Image.fromarray(np.ascontiguousarray(srgba[..., :3]), 'RGB').resize((side, side), Image.LANCZOS)
        img = img.filter(ImageFilter.UnsharpMask(radius=1.0, percent=40, threshold=2))
        return np.dstack([np.asarray(img), np.full((side, side), 255, np.uint8)]), 'the mod\'s own'
    return remaster.specular_map(colour_rgba, IV_SPEC_LEVEL, side), 'made from the colour map'


def levels_of(rgba, side):
    fmt, levels = maptex.encode_levels(rgba, side)
    return (levels[0][0], levels[0][1], fmt, [lv[2] for lv in levels])


def main():
    ydd, ytd, out_wtd, png_dir, d_name, n_name, s_name = sys.argv[1:8]
    tex = mod_textures(ydd, ytd)
    os.makedirs(png_dir, exist_ok=True)
    d = to_rgba(tex[d_name.lower()])
    n = to_rgba(tex[n_name.lower()])
    s = to_rgba(tex[s_name.lower()])
    colour = colour_map(d)
    normal = normal_map(n, d)
    spec, how = specular_map(s, d)
    ph = np.full((64, 64, 4), 255, np.uint8)
    ph[..., :3] = 40
    for name, img in (('suit_d', colour), ('suit_n', normal), ('suit_s', spec), ('ph_d', ph)):
        Image.fromarray(img, 'RGBA').save(os.path.join(png_dir, name + '.png'))
    pcwrite.Dictionary({'suit_d': levels_of(colour, 1024), 'suit_n': levels_of(normal, 1024),
                        'suit_s': levels_of(spec, 512), 'ph_d': levels_of(ph, 64)}).write(out_wtd)
    print('suit_d: %dx%d -> 1024, dark tones opened, grain, sharpened' % (d.shape[1], d.shape[0]))
    print('suit_n: %dx%d -> 1024, the colour map\'s detail and the grain added as relief' % (n.shape[1], n.shape[0]))
    print('suit_s: %dx%d -> 512, %s' % (s.shape[1], s.shape[0], how))
    print('%s: %d KiB' % (out_wtd, os.path.getsize(out_wtd) >> 10))


if __name__ == '__main__':
    main()
