"""A map's texture dictionaries (PC .wtd) as console ones (.xtd), with mips.

The vehicle converter's dictionaries (pc2x360/batconv.py build_xtd) keep a
stock car's dictionary whole and add single-level textures to it: right for a
car seen close up, wasteful and shimmering for scenery seen from far off. A
map dictionary is built fresh instead (a stock dictionary's header, texture
record and D3D header cloned), each texture with its mip chain:

  - level 0 no bigger than `side` (512: the size of the game's own map
    textures; detail maps 256), taken from the PC texture's own levels when
    it has them, so no texel is re-encoded;
  - levels down to 32 texels on the shorter side, as the game's own textures
    stop (none of them reaches the packed tail), laid out by xenos.mip_layout
    (checked against stock textures by mipcheck.py);
  - a texture that is not a power of two in size, or not DXT, is decoded,
    resized and encoded as DXT5 with its mips made here.

The fetch constant: mip_max_level (dword 4, bits 6-9) = levels - 1, the mip
address and "packed mips" (dword 5, bits 11-31, as stock textures set them).
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
import batconv  # noqa: E402
import dxt  # noqa: E402
import xenos  # noqa: E402

VP, PP = batconv.VP, batconv.PP
MAP_SIDE = 512
DETAIL_SIDE = 256
MIN_LEVEL_SIDE = 32
XENOS = {'DXT1': (18, 8), 'DXT3': (19, 16), 'DXT5': (20, 16)}


def pc_level_bytes(fmt, w, h):
    bpp = XENOS[fmt][1]
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpp


def pow2(v):
    return v > 0 and (v & (v - 1)) == 0


def decode_pc(src):
    """A PC texture's level 0 as (h, w, 4) RGBA."""
    w, h, fmt = src['w'], src['h'], src['fmt']
    raw = src['res'].phys[src['data']:]
    if fmt == 'DXT1':
        return dxt.decode_dxt1(raw, w, h)[:h, :w]
    if fmt in ('DXT3', 'DXT5'):
        if fmt == 'DXT3':  # explicit 4-bit alpha
            return decode_dxt3(raw, w, h)[:h, :w]
        return dxt.decode_dxt5(raw[:pc_level_bytes(fmt, w, h)], w, h)[:h, :w]
    kind, bpt = batconv.UNCOMPRESSED[fmt]
    px = np.frombuffer(raw[:w * h * bpt], np.uint8)
    if kind == 'A8R8G8B8':
        b = px.reshape(h, w, 4)
        return b[..., [2, 1, 0, 3]].copy()
    if kind == 'A8':
        a = px.reshape(h, w)
        return np.stack([np.zeros_like(a)] * 3 + [a], -1)
    l8 = px.reshape(h, w)
    return np.stack([l8, l8, l8, np.full_like(l8, 255)], -1)


def decode_dxt3(data, w, h):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    b = np.frombuffer(data, np.uint8)[:bw * bh * 16].reshape(-1, 16)
    colour = dxt.decode_dxt1(np.ascontiguousarray(b[:, 8:]).tobytes(), w, h)
    nib = np.stack([b[:, :8] & 15, b[:, :8] >> 4], -1).reshape(-1, 16).astype(np.uint8) * 17
    a = nib.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
    colour[..., 3] = a
    return colour


def encode_levels(rgba, side):
    """RGBA -> DXT5 levels (PC block layout), level 0 a power of two no bigger
    than `side`, down to MIN_LEVEL_SIDE on the shorter side."""
    from PIL import Image
    h, w = rgba.shape[:2]
    tw = min(side, 1 << max(2, int(round(np.log2(max(w, 4))))))
    th = min(side, 1 << max(2, int(round(np.log2(max(h, 4))))))
    img = Image.fromarray(np.ascontiguousarray(rgba), 'RGBA')
    levels = []
    while True:
        lv = np.asarray(img.resize((tw, th), Image.LANCZOS if not levels else Image.BOX)).astype(np.uint8)
        bgra = lv[..., [2, 1, 0, 3]].tobytes()
        levels.append((tw, th, dxt.encode_dxt5(bgra, tw, th)))
        if min(tw, th) // 2 < MIN_LEVEL_SIDE or min(tw, th) < 8:
            break
        img = Image.fromarray(lv, 'RGBA')
        tw, th = tw // 2, th // 2
    return 'DXT5', levels


def texture_levels(src, side):
    """(fmt, [(w, h, PC block data)]) for a PC texture: its own levels from
    the first no bigger than `side`, down to MIN_LEVEL_SIDE; or made here."""
    w, h, fmt, n = src['w'], src['h'], src['fmt'], max(1, src['levels'])
    if fmt not in XENOS or not (pow2(w) and pow2(h)) or src['data'] is None:
        if src['data'] is None:
            rgba = np.full((16, 16, 4), 128, np.uint8)
        else:
            rgba = decode_pc(src)
        return encode_levels(rgba, side) + ('made',)
    offs, o = [], src['data']
    for k in range(n):
        lw, lh = max(1, w >> k), max(1, h >> k)
        offs.append((lw, lh, o))
        o += pc_level_bytes(fmt, lw, lh)
    start = 0
    while start < n - 1 and max(offs[start][0], offs[start][1]) > side:
        start += 1
    if max(offs[start][0], offs[start][1]) > side:  # (no level small enough: made here)
        return encode_levels(decode_pc(src), side) + ('made',)
    out = []
    for lw, lh, o in offs[start:]:
        if out and min(lw, lh) < MIN_LEVEL_SIDE:
            break
        if lw < 4 or lh < 4:
            break
        out.append((lw, lh, bytes(src['res'].phys[o:o + pc_level_bytes(fmt, lw, lh)])))
    return fmt, out, 'own'


class Template:
    """A stock dictionary's header and a stock texture with mips (its record
    and D3D header) to clone."""
    def __init__(self, r):
        self.header = bytes(r.virt[:0x20])
        tl, n = r.vptr(24), r.u16(20)
        self.info = self.d3d = None
        for i in range(n):
            info = r.vptr(tl + 4 * i)
            d3d = r.vptr(info + 24)
            if r.u32(info + 32) > 1:
                self.info, self.d3d = bytes(r.virt[info:info + 0x40]), bytes(r.virt[d3d:d3d + 0x34])
                break
        assert self.info, 'the template dictionary has no texture with mips'
        self.flags = r.flags


def build(tmpl, textures, log=print, side=MAP_SIDE, detail=DETAIL_SIDE):
    """textures: {name: pc_textures entry}. Returns (flags, body, report)."""
    v = batconv.Seg(tmpl.header + b'\x00\x00\x00\x00' + b'\xCD' * 0x0C, batconv.VIRTUAL_PAGE)
    v.w32(4, VP | 0x20)
    v.w32(8, 0)
    p_objs, patches, entries, report = [], [], [], []
    for name in sorted(textures):
        src = textures[name]
        limit = detail if batconv.DETAIL_MAP.search(name) else side
        fmt, levels, how = texture_levels(src, limit)
        xfmt, bpp = XENOS[fmt]
        w0, h0 = levels[0][0], levels[0][1]
        bw0, bh0 = (w0 + 3) // 4, (h0 + 3) // 4
        base = bytearray(xenos.align(bw0, 32) * xenos.align(bh0, 32) * bpp)
        xenos.tile_into(base, xenos.swap16(levels[0][2]), bw0, bh0, bpp, xenos.align(bw0, 32))
        tile_align = 32 * 32 * bpp  # (stock textures start on a 32x32-block tile's bytes)
        k_base = len(p_objs)
        p_objs.append((bytes(base), tile_align))
        k_mip = None
        if len(levels) > 1:
            layout, size = xenos.mip_layout(w0, h0, len(levels), bpp)
            region = bytearray(size)
            for L, (lw, lh, data) in zip(layout, levels[1:]):
                assert (L['bw'], L['bh']) == ((lw + 3) // 4, (lh + 3) // 4), (name, L, lw, lh)
                sub = bytearray(L['pitch'] * L['rows'] * bpp)
                xenos.tile_into(sub, xenos.swap16(data), L['bw'], L['bh'], bpp, L['pitch'], L['x'], L['y'])
                region[L['offset']:L['offset'] + len(sub)] = sub
            k_mip = len(p_objs)
            p_objs.append((bytes(region), tile_align))
        f = list(struct.unpack_from('>6I', tmpl.d3d, 28))
        f[0] = (f[0] & ~(0x1FF << 22)) | ((xenos.align(bw0, 32) * 4 // 32) << 22) | (1 << 31)
        f[1] = (f[1] & 0xFC0) | xfmt  # (endianness and the rest of the low bits as stock; address once placed)
        f[2] = (f[2] & ~0x03FFFFFF) | (w0 - 1) | ((h0 - 1) << 13)
        f[4] = (f[4] & ~(0xFF << 2)) | ((len(levels) - 1) << 6)
        f[5] = (f[5] & 0x1FF) | (1 << 9) | ((1 << 11) if len(levels) > 1 else 0)
        info = v.put(tmpl.info)
        d3d = v.put(tmpl.d3d)
        nm = v.cstr('pack:/%s.dds' % name)
        v.w32(info + 20, VP | nm)
        v.w32(info + 24, VP | d3d)
        v.w16(info + 28, w0)
        v.w16(info + 30, h0)
        v.w32(info + 32, len(levels))
        patches.append((d3d, f, k_base, k_mip))
        entries.append((batconv.joaat(name), info))
        report.append('%s %dx%d %s %d levels (%s%s)' % (name, w0, h0, fmt, len(levels), how,
                                                       '' if (src['w'], src['h']) == (w0, h0)
                                                       else ', from %dx%d' % (src['w'], src['h'])))
    entries.sort()
    assert len({h for h, _ in entries}) == len(entries), 'texture name hash collision'
    hs = v.alloc(4 * len(entries))
    ls = v.alloc(4 * len(entries))
    for k, (h, info) in enumerate(entries):
        v.w32(hs + 4 * k, h)
        v.w32(ls + 4 * k, VP | info)
    v.w32(16, VP | hs)
    v.w16(20, len(entries))
    v.w16(22, len(entries))
    v.w32(24, VP | ls)
    v.w16(28, len(entries))
    v.w16(30, len(entries))
    phys, offs, pcount, pshift = batconv.place_physical(p_objs)
    for d3d, f, kb, km in patches:
        f[1] |= (PP | offs[kb]) & 0xFFFFF000
        if km is not None:
            f[5] |= (PP | offs[km]) & 0xFFFFF000
        struct.pack_into('>6I', v.buf, d3d + 28, *f)
    flags, vtot, _ = batconv.make_flags(tmpl.flags, len(v.buf), pcount, pshift)
    return flags, bytes(v.buf) + b'\0' * (vtot - len(v.buf)) + phys, report


def check(body, flags, sources, log=print):
    """The dictionary read back: every texture found by its name's hash, its
    levels decoding to the PC texture's (level 0 within DXT error of the
    source scaled the same way), each level its parent scaled down."""
    import rsc5
    sys.path.insert(0, HERE)
    import mipcheck
    r = rsc5.Resource(body, flags, True, 7, 'out')
    n, worst = mipcheck.check(r, log=lambda s: None)
    names = {}
    tl, ht, cnt = r.vptr(24), r.vptr(16), r.u16(20)
    hashes = [r.u32(ht + 4 * i) for i in range(cnt)]
    assert hashes == sorted(hashes), 'hash table not sorted'
    for i in range(cnt):
        info = r.vptr(tl + 4 * i)
        nm = r.cstr(r.vptr(info + 20))[len('pack:/'):-len('.dds')]
        assert batconv.joaat(nm) == hashes[i], nm
        names[nm] = info
    missing = sorted(set(sources) - set(names))
    assert not missing, missing
    return '%d textures (%d with mips, worst level error %.1f)' % (cnt, n, worst), worst
