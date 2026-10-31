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


FORMATS = {  # PC fourcc -> (xenos format, bytes per 4x4 block)
    'DXT1': (18, 8),
    'DXT3': (19, 16),
    'DXT5': (20, 16),
}
