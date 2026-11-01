"""Remaster a car's textures from its own colour map, for the converter's
textures= option (run_vehicle.py): a sharper colour map at the console's
size, and real normal and specular maps where a mod ships flat placeholders.

usage: remaster.py <pc.wtd> <colour map> <out_dir> [normal map name] [specular map name]
  e.g. remaster.py batwing.wtd batwing_d remaster batwing_n batwing_s

Writes <out_dir>/<name>.png for each map:
  colour    the mod's map cleaned of its DXT block edges (an edge-keeping
            bilateral filter at its own size), brought to 1024 (Lanczos),
            its mid-tones lifted a touch and lightly sharpened. (v1 raised
            the local contrast with CLAHE: it brought the compression's
            4x4 blocks out with the detail, and looked worse than the mod.)
  normal    1024, from the colour map's detail: its lightness high-passed
            into a height field (panel lines and seams sink, wear and grain
            stay fine), filled across the UV gaps so island edges do not
            bevel, then turned into normals (RGB = x, y, z, as the mod's own);
  specular  512, the paint's shine broken up by the same detail: clean
            panels brighter, seams, grime and dark wear duller.
Nothing is invented beyond the mod's own colour map: every detail in the
new maps is read from it.
"""
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batconv  # noqa: E402
import dxt  # noqa: E402
import rsc5  # noqa: E402


def load(wtd_path):
    raw = open(wtd_path, 'rb').read()
    _, rt, rf = struct.unpack_from('<III', raw, 0)
    r = rsc5.Resource(zlib.decompress(raw[12:]), rf, False, rt, 'wtd')
    return r, batconv.pc_textures(r, 0)


def decode(r, s):
    w, h = s['w'], s['h']
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if s['fmt'].strip('\x00') != 'DXT5':
        raise SystemExit('%s: only DXT5 colour maps are handled (%r)' % (s['name'], s['fmt']))
    data = bytes(r.phys[s['data']:s['data'] + bw * bh * 16])
    return dxt.decode_dxt5(data, bw * 4, bh * 4)[:h, :w]


def blur(a, radius):
    img = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
    return np.asarray(img.filter(ImageFilter.GaussianBlur(radius)), np.float32)


def deblock(rgba):
    """The colour map with its DXT blocks' edges smoothed and the edges that
    belong to the picture (panel lines, decals) kept: a bilateral filter at
    the map's own size."""
    import cv2
    rgb = cv2.bilateralFilter(np.ascontiguousarray(rgba[..., :3]), d=5, sigmaColor=16, sigmaSpace=2.0)
    return np.dstack([rgb, rgba[..., 3]])


def colour_map(rgba, side=1024):
    clean = deblock(rgba)
    img = Image.fromarray(np.ascontiguousarray(clean[..., :3]), 'RGB').resize((side, side), Image.LANCZOS)
    rgb = np.asarray(img, np.float32) / 255.0
    rgb = np.power(rgb, 0.94)  # mid-tones up a touch: dark paint, not flat black
    sharp = Image.fromarray(np.clip(np.rint(rgb * 255.0), 0, 255).astype(np.uint8)).filter(
        ImageFilter.UnsharpMask(radius=1.0, percent=45, threshold=3))
    alpha = Image.fromarray(np.ascontiguousarray(rgba[..., 3])).resize((side, side), Image.LANCZOS)
    return np.dstack([np.asarray(sharp), np.asarray(alpha)])


def height_field(rgba, side):
    lum = (0.299 * rgba[..., 0] + 0.587 * rgba[..., 1] + 0.114 * rgba[..., 2]).astype(np.float32)
    lum = np.asarray(Image.fromarray(lum.astype(np.uint8)).resize((side, side), Image.LANCZOS), np.float32)
    inside = lum > 3.0
    # fill the UV gaps from the islands (a normalised blur), so the edges stay flat
    filled = lum.copy()
    weight = inside.astype(np.float32)
    for radius in (2, 6, 16):
        num = blur(filled * weight, radius)
        den = blur(weight * 255.0, radius) / 255.0
        fill = np.where(den > 1e-3, num / np.maximum(den, 1e-3), 0.0)
        filled = np.where(inside, lum, fill)
        weight = np.maximum(weight, (den > 0.05).astype(np.float32))
    # detail only: the broad shading (baked light) goes, lines and grain stay
    detail = filled - blur(filled, 10.0)
    return detail, inside


def normal_map(rgba, side=1024, strength=0.06):
    h, inside = height_field(rgba, side)
    h = blur(h + 128.0, 0.6) - 128.0
    gx = np.zeros_like(h)
    gy = np.zeros_like(h)
    gx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) * 0.5
    gy[1:-1, :] = (h[2:, :] - h[:-2, :]) * 0.5
    nx, ny, nz = -gx * strength, -gy * strength, np.ones_like(h)
    n = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx / n, ny / n, nz / n
    out = np.dstack([(nx * 0.5 + 0.5) * 255, (ny * 0.5 + 0.5) * 255, (nz * 0.5 + 0.5) * 255,
                     np.full_like(nx, 255)])
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def specular_map(rgba, base=(49, 44, 49), side=512):
    h, inside = height_field(rgba, side)
    lum = (0.299 * rgba[..., 0] + 0.587 * rgba[..., 1] + 0.114 * rgba[..., 2]).astype(np.float32)
    lum = np.asarray(Image.fromarray(lum.astype(np.uint8)).resize((side, side), Image.LANCZOS), np.float32)
    rel = lum / max(1.0, float(np.percentile(lum[inside], 90)) if inside.any() else 1.0)
    grime = np.clip(-h / 12.0, 0.0, 1.0)  # seams and dirt sink in the detail
    shine = np.clip(0.55 + 0.6 * rel - 0.6 * grime, 0.25, 1.35)
    out = np.dstack([np.clip(base[0] * shine * 1.25, 0, 255), np.clip(base[1] * shine * 1.25, 0, 255),
                     np.clip(base[2] * shine * 1.25, 0, 255), np.full_like(shine, 255)])
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def main():
    wtd, colour, out = sys.argv[1:4]
    normal = sys.argv[4] if len(sys.argv) > 4 else None
    spec = sys.argv[5] if len(sys.argv) > 5 else None
    r, tex = load(wtd)
    rgba = decode(r, tex[colour])
    os.makedirs(out, exist_ok=True)
    Image.fromarray(colour_map(rgba), 'RGBA').save(os.path.join(out, colour + '.png'))
    print('%s: %dx%d -> 1024x1024, deblocked, lifted a touch, lightly sharpened' % (
        colour, tex[colour]['w'], tex[colour]['h']))
    rgba = deblock(rgba)  # (the normal and specular maps from the cleaned map's detail)
    if normal:
        Image.fromarray(normal_map(rgba), 'RGBA').save(os.path.join(out, normal + '.png'))
        print('%s: %dx%d placeholder -> 1024x1024 from the colour map\'s detail' % (
            normal, tex[normal]['w'], tex[normal]['h']) if normal in tex else '%s: new, 1024x1024' % normal)
    if spec:
        base = (49, 44, 49)
        if spec in tex:
            px = decode(r, tex[spec]).reshape(-1, 4).mean(0)
            base = tuple(float(c) for c in px[:3])
        Image.fromarray(specular_map(rgba, base), 'RGBA').save(os.path.join(out, spec + '.png'))
        print('%s: -> 512x512, its level %s broken up by the colour map\'s detail' % (spec, tuple(int(c) for c in base)))


if __name__ == '__main__':
    main()
