"""Paint the landing-gear texture gearpatch.py maps the wheels and legs onto
(a mod's bare wheels: the Batwing's took a black corner of another car's
texture). 128 x 128, in texture coordinates (v down):

  u 0-0.5, v 0-0.5    the wheel's face: a disc (centre 0.25, 0.25, radius
                      0.24): hub cap, bolts, the rim, the tyre wall
  u 0.5-1, v 0-0.5    the tread, across its width (u): rubber, three grooves
  v 0.5-1             the strut, top to bottom (v) and around (u): painted
                      metal, a seal, then the chrome oleo

usage: gear_texture.py <out.png> [side]"""
import math
import sys

import numpy as np
from PIL import Image

import gearpatch as G


def paint(side=128):
    rng = np.random.default_rng(7)
    img = np.zeros((side, side, 4), np.float32)
    img[..., 3] = 255
    v, u = (np.mgrid[0:side, 0:side] + 0.5) / side
    noise = rng.normal(0.0, 1.0, (side, side)).astype(np.float32)

    # the face
    du, dv = u - G.FACE_CENTRE[0], v - G.FACE_CENTRE[1]
    r = np.hypot(du, dv) / G.FACE_RADIUS
    ang = np.arctan2(dv, du)
    face = (u < 0.5) & (v < 0.5)
    rubber = np.array([34, 34, 37], np.float32)
    col = np.zeros((side, side, 3), np.float32)
    col[:] = rubber
    hub = G.HUB_EDGE
    # tyre wall: dark, a raised ring, the shoulder a shade lighter
    wall = np.clip((r - hub) / (1 - hub), 0, 1)
    col += (8 * np.exp(-((wall - 0.28) / 0.05) ** 2) + 5 * np.clip((wall - 0.85) / 0.15, 0, 1))[..., None]
    # the metal wheel: rim lip, rim face (light grey), dished towards the hub
    metal = r < hub
    rr = r / hub
    grey = np.where(rr > 0.88, 150, np.where(rr > 0.80, 96, 128 - 22 * (0.80 - rr)))
    cap = rr < 0.30
    grey = np.where(cap, 72 + 25 * (rr / 0.30), grey)
    bolts = np.zeros_like(rr, bool)
    for k in range(5):
        a = 2 * math.pi * k / 5 + 0.3
        bu, bv = G.FACE_CENTRE[0] + math.cos(a) * 0.45 * hub * G.FACE_RADIUS, \
            G.FACE_CENTRE[1] + math.sin(a) * 0.45 * hub * G.FACE_RADIUS
        bolts |= np.hypot(u - bu, v - bv) < 0.016
    grey = np.where(bolts, 175, grey)
    # spokes: five darker slots between the bolts
    slot = (rr > 0.38) & (rr < 0.76) & (np.cos(5 * (ang - 0.3) + math.pi) > 0.72)
    grey = np.where(slot, 52, grey)
    metal_col = np.stack([grey, grey + 2, grey + 6], -1)
    col = np.where((metal & face)[..., None], metal_col, col)
    col = np.where((face & (r > 1.0))[..., None], rubber * 0.8, col)

    # the tread: rubber with three grooves round it
    tread = (u >= 0.5) & (v < 0.5)
    t = (u - G.TREAD_U[0]) / (G.TREAD_U[1] - G.TREAD_U[0])
    groove = np.zeros_like(t, bool)
    for c in (0.25, 0.5, 0.75):
        groove |= np.abs(t - c) < 0.035
    tcol = np.where(groove, 18, 36)
    tcol = np.where((t < 0.06) | (t > 0.94), 42, tcol)
    col = np.where(tread[..., None], np.stack([tcol, tcol, tcol + 3], -1).astype(np.float32), col)

    # the strut: painted metal down to a seal, then chrome
    strut = v >= 0.5
    sv = (v - G.STRUT_V[0]) / (G.STRUT_V[1] - G.STRUT_V[0])
    paint_ = np.stack([np.full_like(u, 58), np.full_like(u, 61), np.full_like(u, 67)], -1)
    paint_ += (6 * noise[..., None])
    chrome_l = 150 + 45 * np.cos(2 * math.pi * (u - 0.3)) ** 8 + 25 * np.cos(2 * math.pi * (u - 0.8)) ** 16
    chrome = np.stack([chrome_l, chrome_l + 2, chrome_l + 6], -1)
    seal = (sv > 0.52) & (sv < 0.58)
    scol = np.where((sv < 0.55)[..., None], paint_, chrome)
    scol = np.where(seal[..., None], np.array([24, 24, 26], np.float32), scol)
    # a band where the strut meets the axle
    scol = np.where(((sv > 0.9) & (sv < 0.94))[..., None], np.array([70, 72, 78], np.float32), scol)
    col = np.where(strut[..., None], scol, col)

    col += (2.5 * noise[..., None]) * (~strut)[..., None]
    img[..., :3] = np.clip(col, 0, 255)
    return img.astype(np.uint8)


if __name__ == '__main__':
    out = sys.argv[1]
    side = int(sys.argv[2]) if len(sys.argv) > 2 else 128
    Image.fromarray(paint(side), 'RGBA').save(out)
    print('wrote', out)
