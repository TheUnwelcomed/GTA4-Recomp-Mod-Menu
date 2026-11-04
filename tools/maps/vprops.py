"""The GTA V props a scene places that Liberty City has nothing like, made
here as the set's own pieces (v1.7.42, after the user's "remake the cacti,
the coke lab, the palms"): palms, cacti, an agave and a joshua tree, and the
Scarface mansion's coke lab - bricks and their stacks, boxes, the glass
mixing tubes, bottles, tins, a scale, a mold, a press, the lab table,
trolley, pallet and rack, a tray of lines. Meshes made from tubes, boxes and
cards (props.py's Mesh), textures drawn here (PIL) or the mod's own by name
(the Keys' gtrveg.wtd: the palm trunk, the fronds, the fan palm's dead-leaf
skirt, a bark), written as PC files the set's pipeline takes like the mod's
(pc2x360/pcwrite.py), with collision from the trunks and the furniture.

usage: python3 -I vprops.py <set source dir> <out dir> [<more .wdr dirs for shader samples>...]
  writes <out dir>/<model>.wdr, vprops.wtd (the textures drawn here),
  <model>.colmesh.npz, vprops.ide (their lines) and vprops.png (a sheet of
  the textures). Link them into the set's source directory and name
  vprops.ide in the set's "ide" list; sets/mansion_subs.json names which V
  prop each stands in for (vdress.py).
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import maptex  # noqa: E402
import pcwrite  # noqa: E402
import props  # noqa: E402

TEX = batconv.SAMPLER_TEXTURE
FLAGS = 0xC00000
# the Keys mod's own textures (gtrveg.wtd, gtrdock.wtd), by name
PALM_TRUNK = 't_prop_palm_trunk_05_bc'       # two trunks side by side: u 0..0.5 the smooth one
PALM_TOP = 't_prop_palm_trunk_top_1a_bc'     # the crown's fibre
FRONDS = 't_palmfronds_bc'                   # a stem (u .05-.16), a dark frond (.23-.64), a light one (.64-.99)
FAN_SKIRT = 't_prop_fan_palm_leaf_im_05_bc'  # a fan palm's hanging dead leaves
BARK = 't_prop_plant_bark_01_bc'
PLANKS = 't_planks01_bc'
FONT = '/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf'
RNG = np.random.RandomState(7)


# ---------------------------------------------------------------- meshes
class VMesh(props.Mesh):
    def tube(self, shader, path, radii, n=10, u_wrap=0.5, u0=0.0, v_per_m=1.0, collide=True, ribs=0.0):
        """A tube through `path` (points, rings in the horizontal plane) with
        a radius per point; u round it (u0..u0+u_wrap), v along it; `ribs`:
        the radius swelling and shrinking round the ring (a cactus)."""
        p = self._part(shader)
        base = len(p['pos'])
        length = 0.0
        for i, (c, r) in enumerate(zip(path, radii)):
            c = np.asarray(c, float)
            if i:
                length += float(np.linalg.norm(c - np.asarray(path[i - 1], float)))
            for k in range(n + 1):
                a = 2 * math.pi * k / n
                rr = r * (1.0 + ribs * math.cos(a * n / 2.0))
                p['pos'].append(c + np.array([rr * math.cos(a), rr * math.sin(a), 0.0]))
                p['nrm'].append(np.array([math.cos(a), math.sin(a), 0.0]))
                p['uv'].append((u0 + u_wrap * k / n, length * v_per_m))
        for i in range(len(path) - 1):
            for k in range(n):
                a = base + i * (n + 1) + k
                b, a2, b2 = a + 1, a + n + 1, a + n + 2
                p['tris'] += [(a, b, b2), (a, b2, a2)]
        if collide:
            for i in range(len(path) - 1):
                c0, c1 = np.asarray(path[i], float), np.asarray(path[i + 1], float)
                r = max(radii[i], radii[i + 1])
                lo = np.minimum(c0, c1) - [r, r, 0]
                hi = np.maximum(c0, c1) + [r, r, 0]
                for face in self._box_faces(lo, hi):
                    self.col += face
        return base

    def col_box(self, lo, hi):
        """Collision alone: a box (a stack of bricks, a pallet's load)."""
        for face in self._box_faces(np.asarray(lo, float), np.asarray(hi, float)):
            self.col += face

    def cap(self, shader, centre, r, n=10, up=True, uv=(0.0, 0.0, 1.0, 1.0)):
        """A disc (a fan) closing a tube's end, facing up or down."""
        p = self._part(shader)
        base = len(p['pos'])
        c = np.asarray(centre, float)
        u0, v0, u1, v1 = uv
        p['pos'].append(c)
        p['nrm'].append(np.array([0.0, 0.0, 1.0 if up else -1.0]))
        p['uv'].append(((u0 + u1) / 2, (v0 + v1) / 2))
        for k in range(n + 1):
            a = 2 * math.pi * k / n
            p['pos'].append(c + np.array([r * math.cos(a), r * math.sin(a), 0.0]))
            p['nrm'].append(np.array([0.0, 0.0, 1.0 if up else -1.0]))
            p['uv'].append((u0 + (u1 - u0) * (0.5 + 0.5 * math.cos(a)), v0 + (v1 - v0) * (0.5 - 0.5 * math.sin(a))))
        for k in range(n):
            if up:
                p['tris'].append((base, base + 1 + k, base + 2 + k))
            else:
                p['tris'].append((base, base + 2 + k, base + 1 + k))

    def strip(self, shader, spine, widths, u_range, v_tip=0.0, v_base=1.0, double=True):
        """A ribbon of quads along `spine` (points) with a width at each,
        across the horizontal perpendicular of the spine's direction: a frond,
        a leaf; the texture's u range across it, v from the base to the tip."""
        n = len(spine)
        pts = [np.asarray(s, float) for s in spine]
        for i in range(n - 1):
            d = pts[min(i + 1, n - 1)] - pts[max(i - 1, 0)]
            side = np.array([-d[1], d[0], 0.0])
            side /= max(np.linalg.norm(side), 1e-9)
            d2 = pts[min(i + 2, n - 1)] - pts[i]
            side2 = np.array([-d2[1], d2[0], 0.0])
            side2 /= max(np.linalg.norm(side2), 1e-9)
            t0, t1 = i / (n - 1), (i + 1) / (n - 1)
            v0, v1 = v_base + (v_tip - v_base) * t0, v_base + (v_tip - v_base) * t1
            w0, w1 = widths[i] / 2, widths[i + 1] / 2
            self.quad(shader, pts[i] - side * w0, pts[i] + side * w0, pts[i + 1] + side2 * w1, pts[i + 1] - side2 * w1,
                      (u_range[0], v0), (u_range[1], v0), (u_range[1], v1), (u_range[0], v1), double=double, collide=False)

    def card(self, shader, centre, width, height, angle, uv=(0.0, 0.0, 1.0, 1.0), double=True, tilt=0.0):
        """An upright cut-out card at `centre` (its bottom middle), facing
        `angle` (its normal; the card across it), leaning back by `tilt`."""
        c = np.asarray(centre, float)
        side = np.array([-math.sin(angle), math.cos(angle), 0.0]) * (width / 2)
        back = np.array([math.cos(angle), math.sin(angle), 0.0]) * (-math.sin(tilt) * height)
        up = np.array([0.0, 0.0, math.cos(tilt) * height])
        u0, v0, u1, v1 = uv
        self.quad(shader, c - side, c + side, c + side + up + back, c - side + up + back,
                  (u0, v1), (u1, v1), (u1, v0), (u0, v0), double=double, collide=False)


# ---------------------------------------------------------------- textures (drawn here)
def noise(w, h, lo, hi, seed, grain=8):
    r = np.random.RandomState(seed)
    small = r.uniform(lo, hi, (h // grain, w // grain))
    return np.kron(small, np.ones((grain, grain)))[:h, :w]


def rgba(base, alpha=None):
    a = np.full(base.shape[:2], 255, np.uint8) if alpha is None else alpha
    return np.dstack([base.astype(np.uint8), a])


def cactus_skin(w=256, h=256):
    """Green with vertical ribs (light and shade) and spines along them."""
    x = np.arange(w)[None, :]
    rib = 0.5 + 0.5 * np.cos(x * 2 * math.pi * 8 / w)  # 8 ribs across the texture (the tube's u 0..1)
    g = (60 + 55 * rib + noise(w, h, -12, 12, 1, 4))
    r = (30 + 35 * rib + noise(w, h, -8, 8, 2, 4))
    b = (25 + 25 * rib) + np.zeros((h, w))
    img = np.dstack([r, g, b]).clip(0, 255)
    pil = Image.fromarray(img.astype(np.uint8))
    d = ImageDraw.Draw(pil)
    for k in range(8):  # the spines on the ribs' crests
        cx = int((k + 0.0) * w / 8)
        for y in range(6, h, 18):
            d.ellipse([cx - 2, y - 2, cx + 2, y + 2], fill=(230, 225, 190))
            d.line([cx - 7, y - 3, cx + 7, y + 3], fill=(240, 235, 200), width=1)
            d.line([cx - 7, y + 3, cx + 7, y - 3], fill=(240, 235, 200), width=1)
    return rgba(np.asarray(pil))


def agave_leaf(w=128, h=256):
    """One pointed leaf, blue-green, a pale stripe, a dark brown tip; cut out."""
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # the leaf from the base (bottom, full width) to the tip (top)
    poly = [(w * 0.18, h - 1), (w * 0.82, h - 1), (w * 0.62, h * 0.35), (w * 0.5, 4), (w * 0.38, h * 0.35)]
    d.polygon(poly, fill=(92, 140, 120, 255))
    d.polygon([(w * 0.42, h - 1), (w * 0.58, h - 1), (w * 0.5, h * 0.15)], fill=(150, 185, 165, 255))
    d.polygon([(w * 0.46, h * 0.2), (w * 0.54, h * 0.2), (w * 0.5, 4)], fill=(120, 80, 50, 255))
    d.line([(w * 0.18, h - 1), (w * 0.38, h * 0.35), (w * 0.5, 4)], fill=(60, 95, 80, 255), width=3)
    d.line([(w * 0.82, h - 1), (w * 0.62, h * 0.35), (w * 0.5, 4)], fill=(60, 95, 80, 255), width=3)
    for y in range(int(h * 0.3), h - 10, 14):  # the teeth
        d.polygon([(w * 0.18 + (0.2 * (h - y) / h) * w, y), (w * 0.14 + (0.2 * (h - y) / h) * w, y - 5), (w * 0.2 + (0.2 * (h - y) / h) * w, y - 8)], fill=(110, 75, 45, 255))
    return np.asarray(img)


def spikes(w=256, h=256):
    """A joshua tree's tuft: spiky leaves radiating from the bottom middle; cut out."""
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = np.random.RandomState(3)
    for k in range(70):
        a = math.radians(r.uniform(15, 165))
        ln = r.uniform(0.55, 0.98) * h
        x1, y1 = w / 2 + math.cos(a) * ln * 0.5, h - math.sin(a) * ln
        shade = int(r.uniform(90, 150))
        d.line([(w / 2, h - 1), (x1, y1)], fill=(shade - 40, shade, shade - 60, 255), width=int(r.uniform(2, 5)))
    return np.asarray(img)


def fan_leaf(w=256, h=256):
    """A fan palm's leaf: a half disc of pleated green segments on a stalk; cut out."""
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = w / 2, h * 0.92
    for k in range(26):
        a0 = math.radians(10 + 160 * k / 26)
        a1 = math.radians(10 + 160 * (k + 1) / 26)
        rr = h * 0.86
        shade = 70 + (k % 2) * 40
        d.polygon([(cx, cy), (cx + math.cos(a0) * rr, cy - math.sin(a0) * rr), (cx + math.cos(a1) * rr, cy - math.sin(a1) * rr)],
                  fill=(shade - 30, shade + 60, shade - 25, 255))
    d.line([(cx, h - 1), (cx, cy)], fill=(120, 110, 70, 255), width=6)
    return np.asarray(img)


def brick(w=256, h=256):
    """A kilo wrapped in plastic over white, two bands of tan tape."""
    base = np.dstack([noise(w, h, 225, 245, 4, 6), noise(w, h, 225, 245, 5, 6), noise(w, h, 228, 248, 6, 6)])
    pil = Image.fromarray(base.astype(np.uint8))
    d = ImageDraw.Draw(pil)
    for y in range(0, h, 9):  # the wrap's creases
        d.line([(0, y + (y * 7) % 5), (w, y)], fill=(205, 208, 215), width=1)
    for x0 in (int(w * 0.22), int(w * 0.62)):
        d.rectangle([x0, 0, x0 + int(w * 0.14), h], fill=(190, 160, 110))
        d.rectangle([x0 + 3, 0, x0 + 6, h], fill=(170, 140, 95))
    return rgba(np.asarray(pil))


def powder(w=128, h=128):
    base = np.dstack([noise(w, h, 232, 252, 7, 4)] * 3)
    return rgba(base)


def cardboard(w=256, h=256):
    base = np.dstack([noise(w, h, 150, 175, 8, 8), noise(w, h, 115, 135, 9, 8), noise(w, h, 75, 95, 10, 8)])
    pil = Image.fromarray(base.astype(np.uint8))
    d = ImageDraw.Draw(pil)
    d.rectangle([0, int(h * 0.42), w, int(h * 0.58)], fill=(165, 150, 120))  # a tape band
    font = ImageFont.truetype(FONT, 30)
    for text, y in (('PREMIUM', 18), ('THIS SIDE UP', h - 50)):
        box = d.textbbox((0, 0), text, font=font)
        d.text(((w - (box[2] - box[0])) / 2 - box[0], y), text, font=font, fill=(60, 45, 30))
    d.polygon([(w / 2, h * 0.22), (w / 2 - 18, h * 0.36), (w / 2 + 18, h * 0.36)], fill=(60, 45, 30))
    return rgba(np.asarray(pil))


def glass(w=64, h=64):
    base = np.dstack([noise(w, h, 205, 225, 11, 4), noise(w, h, 215, 232, 12, 4), noise(w, h, 225, 240, 13, 4)])
    return rgba(base)


def steel(w=128, h=128):
    x = np.arange(w)[None, :]
    g = 150 + 25 * np.sin(x * 0.9) + noise(w, h, -10, 10, 14, 2)
    return rgba(np.dstack([g, g, g + 6]).clip(0, 255))


def dark(w=32, h=32):
    return rgba(np.dstack([noise(w, h, 25, 40, 15, 4)] * 3))


def label(w=256, h=128, text='POWDER', colour=(200, 40, 40)):
    img = Image.new('RGB', (w, h), (245, 240, 230))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w, int(h * 0.3)], fill=colour)
    d.rectangle([0, int(h * 0.82), w, h], fill=colour)
    font = ImageFont.truetype(FONT, 34)
    box = d.textbbox((0, 0), text, font=font)
    d.text(((w - (box[2] - box[0])) / 2 - box[0], int(h * 0.42) - box[1]), text, font=font, fill=(40, 40, 40))
    return rgba(np.asarray(img))


# ---------------------------------------------------------------- the plants
def palm(lib, height=9.0, lean=0.9, fronds=12, frond_len=3.2, seed=1):
    """A feather palm: a leaning tapered trunk, a fibre crown, fronds in two
    tiers bending out and down; origin at the foot."""
    r = np.random.RandomState(seed)
    m = VMesh()
    segs = 7
    path, radii = [], []
    lean_dir = r.uniform(0, 2 * math.pi)
    for i in range(segs + 1):
        t = i / segs
        off = lean * t ** 1.6
        path.append((math.cos(lean_dir) * off, math.sin(lean_dir) * off, height * t))
        radii.append(0.22 * (1.0 - 0.45 * t) * (1.0 + 0.5 * (t < 0.08)))
    m.tube('trunk', path, radii, n=10, u_wrap=0.5, v_per_m=0.5)
    top = np.array(path[-1])
    # the crown's fibre: a short wider tube, a cap
    m.tube('crown', [top - [0, 0, 0.5], top + [0, 0, 0.25]], [0.17, 0.26], n=10, u_wrap=1.0, v_per_m=1.3, collide=False)
    m.cap('crown', top + [0, 0, 0.25], 0.26, n=10, uv=(0, 0, 1, 1))
    # the fronds: the upper tier rising, the lower drooping, a few dead ones hanging
    for k in range(fronds):
        ang = 2 * math.pi * k / fronds + r.uniform(-0.15, 0.15)
        tier = k % 3
        elev = {0: 0.95, 1: 0.45, 2: -0.05}[tier] + r.uniform(-0.1, 0.1)
        droop = {0: 0.55, 1: 0.5, 2: 0.4}[tier]
        ln = frond_len * r.uniform(0.85, 1.1)
        spine = []
        for i in range(5):
            t = i / 4
            horiz = ln * math.cos(elev) * t
            z = ln * (math.sin(elev) * t - droop * t * t)
            spine.append(top + [math.cos(ang) * horiz, math.sin(ang) * horiz, z + 0.1])
        width = ln * 0.38
        u = (0.23, 0.64) if r.uniform() < 0.7 else (0.64, 0.99)
        m.strip('fronds', spine, [width * 0.55, width, width, width * 0.8, width * 0.35], u)
    for k in range(3):  # dead fronds hanging under the crown
        ang = 2 * math.pi * (k + 0.5) / 3 + r.uniform(-0.3, 0.3)
        spine = [top + [math.cos(ang) * 0.15 * i, math.sin(ang) * 0.15 * i, -0.2 - 0.75 * i] for i in range(4)]
        m.strip('fronds', spine, [0.5, 0.75, 0.6, 0.25], (0.64, 0.99))
    d = pcwrite.Drawable(lib)
    idx = {'trunk': d.shader('gta_default.sps', {TEX: PALM_TRUNK}),
           'crown': d.shader('gta_default.sps', {TEX: PALM_TOP}),
           'fronds': d.shader('gta_cutout.sps', {TEX: FRONDS}, fallback='gta_default.sps')}
    m.write(d, idx)
    return d, m


def fan_palm(lib, height=6.0, seed=5):
    """A fan palm: a thick trunk, a skirt of dead leaves under the crown,
    fan leaves on stalks all round."""
    r = np.random.RandomState(seed)
    m = VMesh()
    path = [(0, 0, 0), (0.05, 0, height * 0.5), (0.12, 0.05, height)]
    m.tube('trunk', path, [0.42, 0.34, 0.3], n=10, u_wrap=0.5, u0=0.5, v_per_m=0.6)
    top = np.array(path[-1])
    # the skirt: a flaring tube of hanging leaves (cut out), two layers
    for layer, (r0, r1, dz) in enumerate(((0.32, 0.95, 2.4), (0.35, 0.8, 1.6))):
        m.tube('skirt', [top - [0, 0, dz], top - [0, 0, 0.1 * layer]], [r1, r0], n=14, u_wrap=3.0, u0=0.5 * layer, v_per_m=1.0 / dz, collide=False)
    # the fan leaves: cards on short stalks, upper ones upright, lower ones leaning out
    leaves = 16
    for k in range(leaves):
        ang = 2 * math.pi * k / leaves + r.uniform(-0.1, 0.1)
        tier = k % 2
        out = 0.5 + 0.5 * tier
        base_pt = top + [math.cos(ang) * out, math.sin(ang) * out, 0.15 - 0.25 * tier]
        m.card('fan', base_pt, 1.5, 1.5, ang + math.pi / 2, tilt=0.35 + 0.5 * tier)
    m.cap('crown', top + [0, 0, 0.05], 0.3, n=10)
    d = pcwrite.Drawable(lib)
    idx = {'trunk': d.shader('gta_default.sps', {TEX: PALM_TRUNK}),
           'crown': d.shader('gta_default.sps', {TEX: PALM_TOP}),
           'skirt': d.shader('gta_cutout.sps', {TEX: FAN_SKIRT}, fallback='gta_default.sps'),
           'fan': d.shader('gta_cutout.sps', {TEX: 'vp_fanleaf'}, fallback='gta_default.sps')}
    m.write(d, idx)
    return d, m


def saguaro(lib, height=3.6, arms=2, seed=2):
    """A saguaro: a ribbed column rounded at the top, arms bending up."""
    r = np.random.RandomState(seed)
    m = VMesh()

    def column(x, y, z0, h, rad):
        path, radii = [], []
        n = 6
        for i in range(n + 1):
            t = i / n
            path.append((x, y, z0 + h * t))
            radii.append(rad * (1.0 - 0.75 * max(0.0, t - 0.82) / 0.18) * (0.9 + 0.1 * (t > 0.05)))
        m.tube('skin', path, radii, n=16, u_wrap=1.0, v_per_m=0.8, ribs=0.08)
        m.cap('skin', (x, y, z0 + h), radii[-1] * 0.9, n=16)
    column(0, 0, 0, height, 0.3)
    for k in range(arms):
        ang = math.pi * (0.3 + 1.2 * k) + r.uniform(-0.3, 0.3)
        z = height * r.uniform(0.35, 0.55)
        ah = height * r.uniform(0.3, 0.45)
        path = [(0, 0, z), (math.cos(ang) * 0.5, math.sin(ang) * 0.5, z + 0.05), (math.cos(ang) * 0.78, math.sin(ang) * 0.78, z + 0.35),
                (math.cos(ang) * 0.85, math.sin(ang) * 0.85, z + 0.4 + ah * 0.5), (math.cos(ang) * 0.85, math.sin(ang) * 0.85, z + 0.4 + ah)]
        m.tube('skin', path, [0.2, 0.2, 0.19, 0.17, 0.12], n=12, u_wrap=1.0, v_per_m=0.8, ribs=0.08)
        m.cap('skin', path[-1], 0.11, n=12)
    d = pcwrite.Drawable(lib)
    idx = {'skin': d.shader('gta_default.sps', {TEX: 'vp_cactus'})}
    m.write(d, idx)
    return d, m


def cactus_clump(lib, seed=3):
    """Three ribbed columns of different heights, close together."""
    r = np.random.RandomState(seed)
    m = VMesh()
    for (x, y, h, rad) in ((0.0, 0.0, 1.7, 0.2), (0.32, 0.08, 1.1, 0.16), (-0.22, 0.25, 0.8, 0.14)):
        path, radii = [], []
        for i in range(5):
            t = i / 4
            path.append((x, y, h * t))
            radii.append(rad * (1.0 - 0.7 * max(0.0, t - 0.8) / 0.2))
        m.tube('skin', path, radii, n=14, u_wrap=1.0, v_per_m=0.8, ribs=0.09)
        m.cap('skin', (x, y, h), radii[-1] * 0.9, n=14)
    d = pcwrite.Drawable(lib)
    idx = {'skin': d.shader('gta_default.sps', {TEX: 'vp_cactus'})}
    m.write(d, idx)
    return d, m


def agave(lib, leaves=16, seed=4):
    """A rosette of pointed leaves, the outer ones lying out, the inner ones up."""
    r = np.random.RandomState(seed)
    m = VMesh()
    for k in range(leaves):
        ang = 2 * math.pi * k / leaves + r.uniform(-0.12, 0.12)
        tier = k % 2
        elev = 0.35 + 0.55 * tier + r.uniform(-0.1, 0.1)
        ln = 1.1 - 0.25 * tier
        spine = []
        for i in range(4):
            t = i / 3
            horiz = ln * math.cos(elev) * t
            spine.append((math.cos(ang) * horiz, math.sin(ang) * horiz, 0.05 + ln * (math.sin(elev) * t - 0.25 * t * t)))
        m.strip('leaf', spine, [0.26, 0.3, 0.22, 0.08], (0.0, 1.0))
    m.tube('core', [(0, 0, 0), (0, 0, 0.3)], [0.22, 0.12], n=8, u_wrap=1.0, v_per_m=2.0)
    d = pcwrite.Drawable(lib)
    idx = {'leaf': d.shader('gta_cutout.sps', {TEX: 'vp_agave'}, fallback='gta_default.sps'),
           'core': d.shader('gta_default.sps', {TEX: PALM_TOP})}
    m.write(d, idx)
    return d, m


def joshua(lib, height=4.0, seed=6):
    """A joshua tree: a bark trunk forking into branches, each ending in a
    tuft of spikes over a collar of dead leaves."""
    r = np.random.RandomState(seed)
    m = VMesh()
    fork = height * 0.45
    m.tube('bark', [(0, 0, 0), (0.05, 0, fork)], [0.26, 0.2], n=10, u_wrap=1.0, v_per_m=0.7)
    tips = []
    for k in range(3):
        ang = 2 * math.pi * k / 3 + r.uniform(-0.3, 0.3)
        reach = r.uniform(0.5, 0.9)
        h = height * r.uniform(0.75, 1.0)
        path = [(0.05, 0, fork), (math.cos(ang) * reach * 0.5, math.sin(ang) * reach * 0.5, fork + (h - fork) * 0.45),
                (math.cos(ang) * reach, math.sin(ang) * reach, h)]
        m.tube('bark', path, [0.17, 0.14, 0.1], n=8, u_wrap=1.0, v_per_m=0.7, collide=False)
        tips.append(np.array(path[-1]))
        if k == 1:  # one branch forks again
            path2 = [path[1], (path[1][0] * 1.6, path[1][1] * 1.6 - 0.4, path[1][2] + 0.6), (path[1][0] * 2.0, path[1][1] * 2.0 - 0.7, path[1][2] + 1.3)]
            m.tube('bark', path2, [0.12, 0.1, 0.08], n=8, u_wrap=1.0, v_per_m=0.7, collide=False)
            tips.append(np.array(path2[-1]))
    for tip in tips:
        m.tube('skirt', [tip - [0, 0, 0.9], tip - [0, 0, 0.15]], [0.45, 0.2], n=10, u_wrap=2.0, v_per_m=1.2, collide=False)
        for k in range(6):
            ang = math.pi * k / 3
            m.card('spikes', tip - [0, 0, 0.2], 1.1, 1.0, ang, tilt=-0.2)
    d = pcwrite.Drawable(lib)
    idx = {'bark': d.shader('gta_default.sps', {TEX: BARK}),
           'skirt': d.shader('gta_cutout.sps', {TEX: FAN_SKIRT}, fallback='gta_default.sps'),
           'spikes': d.shader('gta_cutout.sps', {TEX: 'vp_spikes'}, fallback='gta_default.sps')}
    m.write(d, idx)
    return d, m


# ---------------------------------------------------------------- the coke lab
def brick_at(m, x, y, z, yaw=0.0, cut=False, collide=True):
    """A kilo brick lying flat, its middle at (x, y) on z; `cut`: a corner
    opened, the powder showing."""
    L, W, H = 0.26, 0.18, 0.075  # (0.075: the step between the mansion's layers of prop_coke_block_01)
    c, s = math.cos(yaw), math.sin(yaw)

    def pt(dx, dy, dz):
        return (x + dx * c - dy * s, y + dx * s + dy * c, z + dz)
    corners = [pt(-L / 2, -W / 2, 0), pt(L / 2, -W / 2, 0), pt(L / 2, W / 2, 0), pt(-L / 2, W / 2, 0)]
    tops = [pt(-L / 2, -W / 2, H), pt(L / 2, -W / 2, H), pt(L / 2, W / 2, H), pt(-L / 2, W / 2, H)]
    m.quad('brick', tops[0], tops[1], tops[2], tops[3], (0, 0), (1, 0), (1, 0.7), (0, 0.7), collide=collide)
    for i in range(4):
        a, b = corners[i], corners[(i + 1) % 4]
        ta, tb = tops[i], tops[(i + 1) % 4]
        m.quad('brick', a, b, tb, ta, (0, 1), (1, 1), (1, 0.65), (0, 0.65), collide=collide)
    if cut:
        m.quad('powder', pt(L / 2 - 0.1, W / 2 - 0.08, H + 0.002), pt(L / 2 + 0.002, W / 2 - 0.08, H + 0.002),
               pt(L / 2 + 0.002, W / 2 + 0.002, H + 0.002), pt(L / 2 - 0.1, W / 2 + 0.002, H + 0.002), collide=False)


def coke_block(lib, cut=False):
    m = VMesh()
    brick_at(m, 0, 0, 0, cut=cut)
    return finish(lib, m, {'brick': 'vp_brick', 'powder': 'vp_powder'})


def coke_stack(lib):
    """A pile of bricks (V's h4 coke stack: 0.46 m, placed by its middle -
    mansion_subs.json's dz puts this foot-origined one on the floor)."""
    m = VMesh()
    z = 0.0
    for layer, (nx, ny) in enumerate(((4, 3), (3, 3), (3, 2), (2, 2), (1, 2))):
        for i in range(nx):
            for j in range(ny):
                brick_at(m, (i - (nx - 1) / 2) * 0.28, (j - (ny - 1) / 2) * 0.2, z, yaw=0.03 * ((i + j + layer) % 3 - 1), collide=False)
        z += 0.08
    m.col_box((-0.56, -0.3, 0), (0.56, 0.3, 0.16))
    m.col_box((-0.42, -0.3, 0.16), (0.42, 0.3, 0.24))
    m.col_box((-0.42, -0.2, 0.24), (0.42, 0.2, 0.32))
    m.col_box((-0.28, -0.2, 0.32), (0.28, 0.2, 0.40))
    return finish(lib, m, {'brick': 'vp_brick'})


def coke_box(lib, size=(0.5, 0.4, 0.35)):
    m = VMesh()
    L, W, H = size
    m.box('box', (-L / 2, -W / 2, 0), (L / 2, W / 2, H), scale=max(L, W))
    return finish(lib, m, {'box': 'vp_box'})


def coke_pallet(lib, bricks=True):
    m = VMesh()
    for y in (-0.45, 0.0, 0.45):
        m.box('wood', (-0.6, y - 0.05, 0.0), (0.6, y + 0.05, 0.1), scale=0.5)
    for x in np.linspace(-0.55, 0.55, 6):
        m.box('wood', (x - 0.05, -0.5, 0.1), (x + 0.05, 0.5, 0.125), scale=0.5)
    if bricks:
        z = 0.125
        for layer in range(2):
            for i in range(4):
                for j in range(4):
                    brick_at(m, (i - 1.5) * 0.28 + 0.02 * layer, (j - 1.5) * 0.2, z, yaw=0.04 * ((i * j + layer) % 3 - 1), collide=False)
            z += 0.095
        m.col_box((-0.56, -0.4, 0.125), (0.58, 0.4, 0.315))
    return finish(lib, m, {'wood': PLANKS, 'brick': 'vp_brick'})


def coke_tube(lib, tall=1.0):
    """A glass mixing tube on a steel base, powder in its bottom third."""
    m = VMesh()
    m.tube('steel', [(0, 0, 0), (0, 0, 0.03)], [0.2, 0.2], n=12, u_wrap=1.0, v_per_m=4.0)
    m.cap('steel', (0, 0, 0.03), 0.2, n=12)
    for z in (0.03, tall * 0.55):  # two steel rings holding the tube to a rod behind
        m.tube('steel', [(0, 0, z), (0, 0, z + 0.03)], [0.14, 0.14], n=12, u_wrap=1.0, v_per_m=4.0, collide=False)
    m.tube('steel', [(-0.16, 0, 0.03), (-0.16, 0, tall * 0.6)], [0.015, 0.015], n=6, u_wrap=1.0, collide=False)
    m.tube('powder', [(0, 0, 0.035), (0, 0, tall * 0.32)], [0.125, 0.125], n=12, u_wrap=1.0, v_per_m=2.0, collide=False)
    m.cap('powder', (0, 0, tall * 0.32), 0.125, n=12)
    m.tube('glass', [(0, 0, 0.03), (0, 0, tall)], [0.13, 0.13], n=12, u_wrap=1.0, v_per_m=1.0)
    m.cap('glass', (0, 0, tall), 0.13, n=12)
    return finish(lib, m, {'steel': 'vp_steel', 'powder': 'vp_powder', 'glass': 'vp_glass'})


def coke_tubes(lib):
    """A rack of six test tubes."""
    m = VMesh()
    m.box('wood', (-0.17, -0.06, 0), (0.17, 0.06, 0.02), scale=0.3)
    for x in (-0.17, 0.17):
        m.box('wood', (x - 0.01, -0.06, 0), (x + 0.01, 0.06, 0.14), scale=0.3)
    m.box('wood', (-0.17, -0.06, 0.12), (0.17, 0.06, 0.14), scale=0.3, collide=False)
    for i in range(6):
        x = -0.125 + 0.05 * i
        m.tube('glass', [(x, 0, 0.02), (x, 0, 0.19)], [0.011, 0.011], n=6, u_wrap=1.0, v_per_m=4.0, collide=False)
        m.tube('powder', [(x, 0, 0.025), (x, 0, 0.02 + 0.05 + 0.02 * (i % 3))], [0.009, 0.009], n=6, u_wrap=1.0, collide=False)
    return finish(lib, m, {'wood': PLANKS, 'glass': 'vp_glass', 'powder': 'vp_powder'})


def coke_bottle(lib, h=0.22, r=0.05, text='POWDER', colour=(200, 40, 40)):
    m = VMesh()
    m.tube('label', [(0, 0, 0), (0, 0, h * 0.8)], [r, r], n=10, u_wrap=1.0, v_per_m=1.0 / (h * 0.8))
    m.tube('steel', [(0, 0, h * 0.8), (0, 0, h)], [r * 0.95, r * 0.6], n=10, u_wrap=1.0, v_per_m=4.0, collide=False)
    m.cap('steel', (0, 0, h), r * 0.6, n=10)
    return finish(lib, m, {'label': 'vp_label_%s' % text.lower(), 'steel': 'vp_steel'})


def coke_scale(lib):
    m = VMesh()
    m.box('steel', (-0.16, -0.13, 0), (0.16, 0.13, 0.05), scale=0.3)
    m.box('steel', (-0.11, -0.11, 0.05), (0.11, 0.11, 0.06), scale=0.3)
    m.quad('dark', (-0.13, -0.12, 0.051), (-0.02, -0.12, 0.051), (-0.02, -0.08, 0.051), (-0.13, -0.08, 0.051), collide=False)
    return finish(lib, m, {'steel': 'vp_steel', 'dark': 'vp_dark'})


def coke_mold(lib):
    """A steel brick mold: a block with the brick's cavity, powder in it."""
    m = VMesh()
    m.box('steel', (-0.19, -0.14, 0), (0.19, 0.14, 0.11), scale=0.3)
    m.quad('powder', (-0.13, -0.09, 0.111), (0.13, -0.09, 0.111), (0.13, 0.09, 0.111), (-0.13, 0.09, 0.111), collide=False)
    m.box('steel', (-0.14, -0.1, 0.11), (0.14, 0.1, 0.112), scale=0.3, faces='xy', collide=False)
    return finish(lib, m, {'steel': 'vp_steel', 'powder': 'vp_powder'})


def coke_press(lib):
    """A hydraulic press: a base, two uprights, a top beam, the ram over a platen."""
    m = VMesh()
    m.box('steel', (-0.3, -0.2, 0), (0.3, 0.2, 0.06), scale=0.3)
    for x in (-0.24, 0.24):
        m.box('steel', (x - 0.03, -0.03, 0.06), (x + 0.03, 0.03, 1.25), scale=0.3)
    m.box('steel', (-0.3, -0.06, 1.25), (0.3, 0.06, 1.35), scale=0.3)
    m.tube('steel', [(0, 0, 0.75), (0, 0, 1.25)], [0.05, 0.05], n=10, u_wrap=1.0, v_per_m=2.0, collide=False)
    m.box('steel', (-0.17, -0.12, 0.68), (0.17, 0.12, 0.75), scale=0.3, collide=False)
    m.box('steel', (-0.19, -0.14, 0.06), (0.19, 0.14, 0.17), scale=0.3)
    m.tube('dark', [(0, 0, 1.35), (0, 0, 1.45)], [0.09, 0.09], n=10, u_wrap=1.0, collide=False)
    m.cap('dark', (0, 0, 1.45), 0.09, n=10)
    return finish(lib, m, {'steel': 'vp_steel', 'dark': 'vp_dark'})


def coke_table(lib):
    """The lab's steel table, 0.77 m to its top (V's bkr_prop_coke_table01a:
    the mansion's scale, mold and lines sit 0.77 over its origin)."""
    m = VMesh()
    m.box('steel', (-1.0, -0.45, 0.72), (1.0, 0.45, 0.77), scale=0.5)
    for x in (-0.95, 0.95):
        for y in (-0.4, 0.4):
            m.tube('steel', [(x, y, 0), (x, y, 0.72)], [0.025, 0.025], n=6, u_wrap=1.0, v_per_m=2.0)
    m.box('steel', (-0.95, -0.4, 0.2), (0.95, 0.4, 0.23), scale=0.5, collide=False)
    return finish(lib, m, {'steel': 'vp_steel'})


def coke_trolley(lib):
    m = VMesh()
    for z in (0.32, 0.88):
        m.box('steel', (-0.5, -0.3, z - 0.03), (0.5, 0.3, z), scale=0.5)
    for x in (-0.47, 0.47):
        for y in (-0.27, 0.27):
            m.tube('steel', [(x, y, 0.08), (x, y, 0.95)], [0.015, 0.015], n=6, u_wrap=1.0, v_per_m=2.0, collide=False)
            m.box('dark', (x - 0.04, y - 0.04, 0), (x + 0.04, y + 0.04, 0.08), scale=0.1, collide=False)
    m.box('steel', (0.47, -0.3, 0.95), (0.5, 0.3, 0.98), scale=0.5, collide=False)
    return finish(lib, m, {'steel': 'vp_steel', 'dark': 'vp_dark'})


def coke_rack(lib):
    """A steel shelf rack: four posts, five shelves. Its origin 0.33 m over
    its feet, as V's v_med_cor_shelfrack stands in the mansion (racks at 6.65
    on a 6.32 floor, their things 0.67, 1.11, 1.67 and 2.04 over them)."""
    m = VMesh()
    for z in (0.0, 0.62, 1.06, 1.60, 1.99):
        m.box('steel', (-0.5, -0.22, z), (0.5, 0.22, z + 0.03), scale=0.5)
    for x in (-0.49, 0.49):
        for y in (-0.21, 0.21):
            m.box('steel', (x - 0.015, y - 0.015, -0.33), (x + 0.015, y + 0.015, 2.15), scale=0.5, collide=False)
    return finish(lib, m, {'steel': 'vp_steel'})


def coke_tin(lib):
    m = VMesh()
    m.tube('label', [(0, 0, 0), (0, 0, 0.16)], [0.07, 0.07], n=10, u_wrap=1.0, v_per_m=1.0 / 0.16)
    m.cap('steel', (0, 0, 0.16), 0.07, n=10)
    return finish(lib, m, {'label': 'vp_label_milk', 'steel': 'vp_steel'})


def coke_heap(lib):
    """A heap of powder on a table (a low cone)."""
    m = VMesh()
    m.tube('powder', [(0, 0, 0), (0, 0, 0.04), (0, 0, 0.07)], [0.16, 0.1, 0.01], n=12, u_wrap=1.0, v_per_m=4.0, collide=False)
    return finish(lib, m, {'powder': 'vp_powder'})


def coke_bowl(lib):
    m = VMesh()
    m.tube('steel', [(0, 0, 0), (0, 0, 0.07)], [0.1, 0.15], n=12, u_wrap=1.0, v_per_m=4.0, collide=False)
    m.cap('powder', (0, 0, 0.05), 0.12, n=12)
    m.cap('steel', (0, 0, 0.0), 0.1, n=12, up=False)
    return finish(lib, m, {'steel': 'vp_steel', 'powder': 'vp_powder'})


def coke_tray(lib):
    """A mirror tray with lines of powder and a card."""
    m = VMesh()
    m.box('glass', (-0.15, -0.1, 0), (0.15, 0.1, 0.01), scale=0.3, collide=False)
    for i in range(4):
        y = -0.06 + 0.04 * i
        m.box('powder', (-0.08, y - 0.004, 0.01), (0.08, y + 0.004, 0.013), scale=0.2, collide=False)
    m.box('brick', (0.08, -0.08, 0.01), (0.13, 0.0, 0.012), scale=0.1, collide=False)
    return finish(lib, m, {'glass': 'vp_glass', 'powder': 'vp_powder', 'brick': 'vp_brick'})


def finish(lib, m, shaders):
    d = pcwrite.Drawable(lib)
    idx = {}
    for key, tex in shaders.items():
        idx[key] = d.shader('gta_default.sps', {TEX: tex})
    m.write(d, idx)
    return d, m


# ---------------------------------------------------------------- the lot
MAKERS = [
    ('palm_tall', lambda lib: palm(lib, 9.5, 1.1, 13, 4.0, seed=1)),
    ('palm_mid', lambda lib: palm(lib, 6.0, 0.6, 12, 3.0, seed=2)),
    ('palm_sm', lambda lib: palm(lib, 3.2, 0.3, 11, 2.3, seed=3)),
    ('palm_fan', lambda lib: fan_palm(lib, 6.0, seed=5)),
    ('palm_fansm', lambda lib: fan_palm(lib, 3.5, seed=6)),
    ('cactus_a', lambda lib: saguaro(lib, 3.6, 2, seed=2)),
    ('cactus_b', cactus_clump),
    ('agave', agave),
    ('joshua', joshua),
    ('coke_block', lambda lib: coke_block(lib, False)),
    ('coke_cutblock', lambda lib: coke_block(lib, True)),
    ('coke_stack', coke_stack),
    ('coke_box', lambda lib: coke_box(lib, (0.5, 0.4, 0.35))),
    ('coke_boxsm', lambda lib: coke_box(lib, (0.35, 0.3, 0.25))),
    ('coke_pallet', coke_pallet),
    ('coke_tube', lambda lib: coke_tube(lib, 1.0)),
    ('coke_tubesm', lambda lib: coke_tube(lib, 0.6)),
    ('coke_tubes', coke_tubes),
    ('coke_bottle', lambda lib: coke_bottle(lib, 0.22, 0.05, 'POWDER', (200, 40, 40))),
    ('coke_chem', lambda lib: coke_bottle(lib, 0.3, 0.06, 'ACETONE', (40, 80, 200))),
    ('coke_tin', coke_tin),
    ('coke_scale', coke_scale),
    ('coke_mold', coke_mold),
    ('coke_press', coke_press),
    ('coke_table', coke_table),
    ('coke_trolley', coke_trolley),
    ('coke_rack', coke_rack),
    ('coke_heap', coke_heap),
    ('coke_bowl', coke_bowl),
    ('coke_tray', coke_tray),
]
TEXTURES = {
    'vp_cactus': (cactus_skin, 256),
    'vp_agave': (agave_leaf, 256),
    'vp_spikes': (spikes, 256),
    'vp_fanleaf': (fan_leaf, 256),
    'vp_brick': (brick, 256),
    'vp_powder': (powder, 128),
    'vp_box': (cardboard, 256),
    'vp_glass': (glass, 64),
    'vp_steel': (steel, 128),
    'vp_dark': (dark, 32),
    'vp_label_powder': (lambda: label(256, 128, 'POWDER', (200, 40, 40)), 256),
    'vp_label_acetone': (lambda: label(256, 128, 'ACETONE', (40, 80, 200)), 256),
    'vp_label_milk': (lambda: label(256, 128, 'MILK POWDER', (210, 60, 60)), 256),
}


def ide_line(name, txd, info):
    lo, hi = info['bbox']
    return '%s, %s, 300, %d, 0, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 0.0, 0.0, 0.0, %.4f, null' % (
        name, txd, FLAGS, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], info['radius'])


def main():
    src, out = sys.argv[1], sys.argv[2]
    dirs = [src] + sys.argv[3:]
    os.makedirs(out, exist_ok=True)
    lib = pcwrite.ShaderLibrary(pcwrite.glob_wdr(dirs))
    lines = []
    for name, maker in MAKERS:
        d, m = maker(lib)
        info = d.write(os.path.join(out, name + '.wdr'))
        n = m.collision(os.path.join(out, name + '.colmesh.npz'))
        lines.append(ide_line(name, 'vprops', info))
        print('%-14s %d shaders, %5d triangles, %3d of collision, box %s..%s' % (
            name, len(d.shaders), sum(len(g['tris']) for g in d.lods[0]), n,
            [round(v, 2) for v in info['bbox'][0]], [round(v, 2) for v in info['bbox'][1]]))
    textures = {}
    sheet = []
    for tname, (draw, side) in TEXTURES.items():
        img = draw()
        fmt, levels = maptex.encode_levels(img, side)
        textures[tname] = (levels[0][0], levels[0][1], fmt, [lv[2] for lv in levels])
        sheet.append(Image.fromarray(img).convert('RGBA').resize((128, 128)))
    pcwrite.Dictionary(textures).write(os.path.join(out, 'vprops.wtd'))
    w = Image.new('RGBA', (128 * 7, 128 * ((len(sheet) + 6) // 7)), (255, 0, 255, 255))
    for i, im in enumerate(sheet):
        w.alpha_composite(im, ((i % 7) * 128, (i // 7) * 128))
    w.save(os.path.join(out, 'vprops.png'))
    with open(os.path.join(out, 'vprops.ide'), 'w') as f:
        f.write('objs\n' + '\n'.join(lines) + '\nend\n')
    print('%d models, %d textures (vprops.wtd), vprops.ide' % (len(lines), len(textures)))


if __name__ == '__main__':
    main()
