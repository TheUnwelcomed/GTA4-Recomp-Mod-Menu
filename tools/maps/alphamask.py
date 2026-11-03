"""Which of a map mod's textures are cut-out masks (v1.7.37): a colour map
whose alpha is a real mask (some texels clear, more solid) drawn by an opaque
shader comes out as a solid card - the mod's transparent leaf areas filled
with their RGB, often black (the Gator Keys' monstera bushes: dark blobs).
Such a shader is drawn as its cut-out preset instead (mapdraw.AUTO_CUTOUT),
when the texture's name says it is a plant (a window's or a fence's mask
stays as the mod drew it).

  masks(src_dir)            {texture name: (clear fraction, solid fraction)}
                            for every .wtd in the directory
  cutouts(src_dir)          the names to draw cut out: masks with a plant's name

usage: python3 -I alphamask.py <source dir>   (lists them)
"""
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
import batconv  # noqa: E402
import dxt  # noqa: E402
import rsc5  # noqa: E402

CLEAR, SOLID = 64, 192            # alpha below / above
MIN_CLEAR, MIN_SOLID = 0.08, 0.15  # a mask: this much of each
PLANT = re.compile(r'palm|frond|leaf|leaves|bush|hedge|tree|branch|monstera|fern|grass|plant|veg|ivy|flower|'
                   r'banan|coco|coral|foliage|shrub|weed|vine|cactus|reed|arbre|feuill|herbe|buisson')


def alpha_of(t):
    """A PC texture's alpha (h x w uint8) at its smallest level at least 64
    texels across, or None for a format without alpha."""
    w, h, levels = t['w'], t['h'], max(1, t['levels'])
    fmt = t['fmt']
    if t['data'] is None:
        return None
    if fmt in batconv.UNCOMPRESSED:
        kind, bpt = batconv.UNCOMPRESSED[fmt]
        if kind == 'L8':
            return None
        raw = np.frombuffer(t['res'].phys[t['data']:t['data'] + w * h * bpt], np.uint8)
        return raw.reshape(h, w, bpt)[:, :, 3 if kind == 'A8R8G8B8' else 0]
    name = fmt.strip('\x00')
    if name not in ('DXT1', 'DXT3', 'DXT5'):
        return None
    bpb = 8 if name == 'DXT1' else 16
    off = t['data']
    for k in range(levels):  # (levels packed one after another)
        lw, lh = max(1, w >> k), max(1, h >> k)
        size = max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bpb
        if (min(lw, lh) // 2 < 64 or k == levels - 1) and lw >= 4 and lh >= 4:
            data = t['res'].phys[off:off + size]
            if name == 'DXT1':
                return dxt.decode_dxt1(data, lw, lh)[:, :, 3]
            if name == 'DXT5':
                return dxt.decode_dxt5(data, lw, lh)[:, :, 3]
            # DXT3: explicit 4-bit alpha, the first 8 bytes of each block
            b = np.frombuffer(data, np.uint8).reshape(-1, 16)[:, :8]
            a = np.stack([(b >> 0) & 15, (b >> 4) & 15], 2).reshape(-1, 16).astype(np.uint8) * 17
            bw, bh = max(1, lw // 4), max(1, lh // 4)
            return a.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
        off += size
    return None


def masks(src_dir):
    out = {}
    for f in sorted(os.listdir(src_dir)):
        if not f.lower().endswith('.wtd'):
            continue
        for name, t in batconv.pc_textures(rsc5.Resource.pc(os.path.join(src_dir, f)), 0).items():
            a = alpha_of(t)
            if a is None or not a.size:
                continue
            clear, solid = float((a < CLEAR).mean()), float((a > SOLID).mean())
            if clear >= MIN_CLEAR and solid >= MIN_SOLID:
                out.setdefault(name, (clear, solid))
    return out


def colour_maps(path):
    """A PC drawable's colour maps (lower case, no pack:/ or .dds)."""
    d = rsc5.Resource.pc(path)
    out = set()
    for s in rsc5.read_drawable(d, 0)['shader_group']['shaders']:
        for p in batconv.shader_params(d, s['off']):
            if p['hash'] == 0x2B5170FD and p['type'] == 0 and p['value'] is not None and d.vptr(p['value'] + 20) is not None:
                n = d.cstr(d.vptr(p['value'] + 20)).split('/')[-1].lower()
                out.add(n[:-4] if n.endswith('.dds') else n)
    return out


def cutouts(src_dir):
    """The masks with a plant's name, and those a plant model (by its name)
    uses: Wake Island's MW3 trees name theirs by hash (acdae965)."""
    found = masks(src_dir)
    out = {n for n in found if PLANT.search(n)}
    for f in sorted(os.listdir(src_dir)):
        if f.lower().endswith('.wdr') and PLANT.search(f[:-4].lower()):
            out |= {n for n in colour_maps(os.path.join(src_dir, f)) if n in found}
    return out


if __name__ == '__main__':
    found = masks(sys.argv[1])
    for n, (c, s) in sorted(found.items()):
        print('%-48s clear %.2f solid %.2f %s' % (n, c, s, 'PLANT: cut out' if PLANT.search(n) else ''))
