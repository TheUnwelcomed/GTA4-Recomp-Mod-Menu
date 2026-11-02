"""Xenos (Xbox 360 GPU) texture layout: 2D tiling and endian swaps.

Tiling follows the public XGAddress2DTiledOffset reconstruction used by
Xenia (texture_conversion.cc): coordinates are in blocks for compressed
formats, the pitch is aligned to 32 blocks, log2_bpp is log2(bytes/block).
"""


def tiled_outer(y, width, log2_bpp):
    macro = ((y >> 5) * (width >> 5)) << (log2_bpp + 7)
    micro = ((y & 6) << 2) << log2_bpp
    return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)


def tiled_inner(x, y, log2_bpp, base):
    macro = (x >> 5) << (log2_bpp + 7)
    micro = (x & 7) << log2_bpp
    offset = base + (macro + ((micro & ~0xF) << 1) + (micro & 0xF))
    return (((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def align(v, a):
    return (v + a - 1) // a * a


def tile_level(linear, bw, bh, bpp):
    """Linear block-rows (bw x bh blocks, bpp bytes each) -> tiled level
    padded to 32x32 blocks."""
    log2_bpp = bpp.bit_length() - 1
    pw, ph = align(bw, 32), align(bh, 32)
    out = bytearray(pw * ph * bpp)
    for y in range(bh):
        outer = tiled_outer(y, pw, log2_bpp)
        row = y * bw * bpp
        for x in range(bw):
            off = tiled_inner(x, y, log2_bpp, outer) >> log2_bpp
            out[off * bpp:off * bpp + bpp] = linear[row + x * bpp:row + x * bpp + bpp]
    return bytes(out)


def untile_level(tiled, bw, bh, bpp):
    log2_bpp = bpp.bit_length() - 1
    pw = align(bw, 32)
    out = bytearray(bw * bh * bpp)
    for y in range(bh):
        outer = tiled_outer(y, pw, log2_bpp)
        for x in range(bw):
            off = tiled_inner(x, y, log2_bpp, outer) >> log2_bpp
            out[(y * bw + x) * bpp:(y * bw + x) * bpp + bpp] = tiled[off * bpp:off * bpp + bpp]
    return bytes(out)


def swap16(data):
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]
    return bytes(b)


# ---------------------------------------------------------------- mip chains
# A texture with mips keeps level 0 at its base address and levels 1.. at its
# mip address, one after another, each tiled on its own (pitch and rows
# aligned to 32 blocks, the level's bytes to 4 KiB), its size worked out from
# the power-of-two-ceiled size of level 0. Once a level's shorter side is 16
# texels or less it and every level after it share one "packed" tile (that
# level's slot), each at its own offset inside it (Xenia's
# GetPackedMipOffset, texture_util.cc). Checked against the game's own
# textures (tools/maps/mipcheck.py): every stock level decodes to its parent
# level scaled down.

def log2_ceil(v):
    return max(0, (int(v) - 1).bit_length())


def next_pow2(v):
    return 1 << log2_ceil(v)


def packed_level(w, h):
    """The first level kept in the packed tail."""
    lg = log2_ceil(min(w, h))
    return lg - 4 if lg > 4 else 0


def packed_offset(w, h, level):
    """(x, y) in texels of `level` inside the packed tail, or None when the
    level is stored on its own."""
    lw, lh = log2_ceil(w), log2_ceil(h)
    if min(lw, lh) > 4 + level:
        return None
    base = max(0, min(lw, lh) - 4)
    pm = level - base
    if pm < 3:
        return (0, 16 >> pm) if lw > lh else (16 >> pm, 0)
    if lw > lh:
        return ((1 << (lw - base)) >> (pm - 2), 0)
    return (0, (1 << (lh - base)) >> (pm - 2))


def level_size(w, h, level):
    """A level's texels as the layout reckons them (levels past 0 from the
    power-of-two size of level 0)."""
    if level == 0:
        return w, h
    return max(1, next_pow2(w) >> level), max(1, next_pow2(h) >> level)


def mip_layout(w, h, levels, bpp, block=4):
    """Where levels 1..levels-1 sit in the mip region: a list of dicts
    (level, offset: the byte offset of the tile region it is in, pitch and
    rows of that region in blocks, x/y: the level's block offset inside it,
    bw/bh: the level's own blocks), and the region's total bytes."""
    out, off = [], 0
    first_packed = packed_level(w, h)
    tail = None
    for lv in range(1, levels):
        tw, th = level_size(w, h, lv)
        bw, bh = (tw + block - 1) // block, (th + block - 1) // block
        if tail is not None:
            px, py = packed_offset(w, h, lv)
            out.append(dict(level=lv, offset=tail[0], pitch=tail[1], rows=tail[2], x=px // block, y=py // block,
                            bw=bw, bh=bh))
            continue
        pitch, rows = align(bw, 32), align(bh, 32)
        x = y = 0
        if lv >= first_packed:
            px, py = packed_offset(w, h, lv)
            x, y = px // block, py // block
            tail = (off, pitch, rows)
        out.append(dict(level=lv, offset=off, pitch=pitch, rows=rows, x=x, y=y, bw=bw, bh=bh))
        off += align(pitch * rows * bpp, 4096)
    return out, off


def _tiled_offsets(xs, ys, pitch, log2_bpp):
    """Block indexes (in units of whole blocks) of blocks (xs, ys) in a tiled
    surface `pitch` blocks wide: tiled_outer/tiled_inner over numpy arrays."""
    import numpy as np
    xs = np.asarray(xs, np.int64)
    ys = np.asarray(ys, np.int64)
    macro = ((ys >> 5) * (pitch >> 5)) << (log2_bpp + 7)
    micro = ((ys & 6) << 2) << log2_bpp
    outer = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((ys & 8) << (3 + log2_bpp)) + ((ys & 1) << 4)
    macro = (xs >> 5) << (log2_bpp + 7)
    micro = (xs & 7) << log2_bpp
    offset = outer + (macro + ((micro & ~0xF) << 1) + (micro & 0xF))
    full = (((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) +
            ((ys & 16) << 7) + (((((ys & 8) >> 2) + (xs >> 3)) & 3) << 6))
    return full >> log2_bpp


def tile_into(region, linear, bw, bh, bpp, pitch, x0=0, y0=0):
    """Linear block rows (bw x bh) written tiled into `region` (a bytearray,
    a surface `pitch` blocks wide) at block offset (x0, y0)."""
    import numpy as np
    log2_bpp = bpp.bit_length() - 1
    ys, xs = np.mgrid[0:bh, 0:bw]
    idx = _tiled_offsets(xs.ravel() + x0, ys.ravel() + y0, pitch, log2_bpp)
    dst = np.frombuffer(region, np.uint8).reshape(-1, bpp)
    dst[idx] = np.frombuffer(linear, np.uint8)[:bw * bh * bpp].reshape(-1, bpp)


def untile_from(region, bw, bh, bpp, pitch, x0=0, y0=0):
    """The inverse of tile_into: bw x bh blocks read out of a tiled surface."""
    import numpy as np
    log2_bpp = bpp.bit_length() - 1
    ys, xs = np.mgrid[0:bh, 0:bw]
    idx = _tiled_offsets(xs.ravel() + x0, ys.ravel() + y0, pitch, log2_bpp)
    src = np.frombuffer(region, np.uint8).reshape(-1, bpp)
    return src[idx].tobytes()


FORMATS = {  # PC fourcc -> (xenos format, bytes per 4x4 block)
    'DXT1': (18, 8),
    'DXT3': (19, 16),
    'DXT5': (20, 16),
}
