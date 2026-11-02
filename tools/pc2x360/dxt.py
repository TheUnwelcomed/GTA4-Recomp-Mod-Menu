"""Small DXT5 (BC3) encoder and decoder for the converter.

PC car textures are almost always DXT already; the odd uncompressed one
(D3DFMT_A8R8G8B8) is compressed here so it goes through the same, verified
360 path as every other texture. Range-fit per 4x4 block: colour endpoints
are the block's extremes along its principal axis, alpha endpoints its
min/max. Output is the PC (little-endian) block layout.
"""
import numpy as np


def _to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)


def _from565(v):
    v = v.astype(np.int32)
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float64)


def encode_dxt5(bgra, w, h):
    """bgra: bytes of w*h texels, PC A8R8G8B8 memory order (B, G, R, A)."""
    img = np.frombuffer(bgra, np.uint8).reshape(h, w, 4).astype(np.float64)
    rgba = img[..., [2, 1, 0, 3]]
    bw, bh = max(1, w // 4), max(1, h // 4)
    blocks = rgba[:bh * 4, :bw * 4].reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    rgb, alpha = blocks[..., :3], blocks[..., 3]
    out = np.zeros((blocks.shape[0], 16), np.uint8)

    # alpha: a0 = max, a1 = min, 8-value mode
    a0 = alpha.max(1)
    a1 = alpha.min(1)
    a0i, a1i = np.rint(a0).astype(np.int32), np.rint(a1).astype(np.int32)
    pal = np.stack([a0i, a1i] + [((7 - k) * a0i + k * a1i) // 7 for k in range(1, 7)], 1)
    aidx = np.abs(alpha[:, :, None] - pal[:, None, :]).argmin(2)
    aidx[a0i == a1i] = 0
    out[:, 0] = a0i
    out[:, 1] = a1i
    bits = np.zeros(blocks.shape[0], np.uint64)
    for k in range(16):
        bits |= aidx[:, k].astype(np.uint64) << np.uint64(3 * k)
    for k in range(6):
        out[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(0xFF)).astype(np.uint8)

    # colour: endpoints along the principal axis
    mean = rgb.mean(1, keepdims=True)
    centered = rgb - mean
    cov = np.einsum('bki,bkj->bij', centered, centered)
    axis = np.ones((blocks.shape[0], 3))
    for _ in range(8):  # power iteration
        axis = np.einsum('bij,bj->bi', cov, axis)
        norm = np.linalg.norm(axis, axis=1, keepdims=True)
        axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), np.array([0.577, 0.577, 0.577]))
    proj = np.einsum('bki,bi->bk', centered, axis)
    hi = rgb[np.arange(len(rgb)), proj.argmax(1)]
    lo = rgb[np.arange(len(rgb)), proj.argmin(1)]
    c0, c1 = _to565(hi), _to565(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    e0, e1 = _from565(c0), _from565(c1)
    cpal = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)
    cidx = ((rgb[:, :, None, :] - cpal[:, None, :, :]) ** 2).sum(3).argmin(2)
    cidx[c0 == c1] = 0
    out[:, 8] = c0 & 0xFF
    out[:, 9] = c0 >> 8
    out[:, 10] = c1 & 0xFF
    out[:, 11] = c1 >> 8
    cbits = np.zeros(blocks.shape[0], np.uint32)
    for k in range(16):
        cbits |= cidx[:, k].astype(np.uint32) << np.uint32(2 * k)
    for k in range(4):
        out[:, 12 + k] = ((cbits >> np.uint32(8 * k)) & np.uint32(0xFF)).astype(np.uint8)
    return out.tobytes()


def decode_dxt1(data, w, h):
    """-> (h, w, 4) uint8 RGBA (BC1, its 1-bit alpha mode included)."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    b = np.frombuffer(data, np.uint8)[:bw * bh * 8].reshape(-1, 8)
    c0 = b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8)
    c1 = b[:, 2].astype(np.int32) | (b[:, 3].astype(np.int32) << 8)
    e0, e1 = _from565(c0), _from565(c1)
    four = (c0 > c1)[:, None, None]
    cpal = np.where(four, np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1),
                    np.stack([e0, e1, (e0 + e1) / 2, np.zeros_like(e0)], 1))
    apal = np.where(four[:, :, 0], np.full((len(b), 4), 255.0), np.array([[255.0, 255.0, 255.0, 0.0]]))
    cb = np.zeros(len(b), np.uint32)
    for k in range(4):
        cb |= b[:, 4 + k].astype(np.uint32) << np.uint32(8 * k)
    cidx = np.stack([(cb >> np.uint32(2 * k)) & np.uint32(3) for k in range(16)], 1).astype(np.int64)
    rgb = np.take_along_axis(cpal, cidx[:, :, None].repeat(3, 2), 1)
    alpha = np.take_along_axis(apal, cidx, 1)
    px = np.concatenate([rgb, alpha[:, :, None]], 2)
    return np.clip(np.rint(px), 0, 255).astype(np.uint8).reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)


def decode_dxt5(data, w, h):
    """-> (h, w, 4) uint8 RGBA, for checking the encoder."""
    bw, bh = max(1, w // 4), max(1, h // 4)
    b = np.frombuffer(data, np.uint8).reshape(-1, 16)
    a0, a1 = b[:, 0].astype(np.int32), b[:, 1].astype(np.int32)
    pal = np.where((a0 > a1)[:, None],
                   np.stack([a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)], 1),
                   np.stack([a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)]
                            + [np.zeros_like(a0), np.full_like(a0, 255)], 1))
    bits = np.zeros(len(b), np.uint64)
    for k in range(6):
        bits |= b[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    aidx = np.stack([(bits >> np.uint64(3 * k)) & np.uint64(7) for k in range(16)], 1).astype(np.int64)
    alpha = np.take_along_axis(pal, aidx, 1)
    c0 = b[:, 8].astype(np.int32) | (b[:, 9].astype(np.int32) << 8)
    c1 = b[:, 10].astype(np.int32) | (b[:, 11].astype(np.int32) << 8)
    e0, e1 = _from565(c0), _from565(c1)
    cpal = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)
    cb = np.zeros(len(b), np.uint32)
    for k in range(4):
        cb |= b[:, 12 + k].astype(np.uint32) << np.uint32(8 * k)
    cidx = np.stack([(cb >> np.uint32(2 * k)) & np.uint32(3) for k in range(16)], 1).astype(np.int64)
    rgb = np.take_along_axis(cpal, cidx[:, :, None].repeat(3, 2), 1)
    px = np.concatenate([rgb, alpha[:, :, None]], 2)
    return np.clip(np.rint(px), 0, 255).astype(np.uint8).reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
