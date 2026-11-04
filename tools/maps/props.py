"""Props made here for a map set, as PC files the set's pipeline takes like
the mod's own (pc2x360/pcwrite.py): Benny's new sign and its bar hut for the
Gator Keys (v1.7.38: the mod's sign was a one-sided oval that showed as a
floating plank from the deck behind it; the user asked for it gone and the
deck done up).

usage: python3 -I props.py <set source dir> <out dir> [<more .wdr dirs for shader samples>...]
  writes <out dir>/bsign.wdr, bhut.wdr, props.wtd (their own textures:
  the sign faces drawn here; the wood and fronds are the mod's, found by name
  in its dictionaries), <model>.colmesh.npz (collision in model space) and
  custom.ide (their lines). Link them into the set's source directory and
  name custom.ide in the set's "ide" list.
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

TEX = batconv.SAMPLER_TEXTURE
WOOD = 'T_cratetop128_portabarrier_BC'   # bennys.wtd: deck planks
POLE = 'T_telepole128_BC'                # bennys.wtd: the stilts
PLANKS = 't_planks01_bc'                 # gtrdock.wtd
FRONDS = 't_palmfronds_bc'               # gtrveg.wtd (a cut-out mask)
FLAGS = 0xC00000
FONTS = {
    'script': '/usr/share/fonts/truetype/google-fonts/Lora-Italic-Variable.ttf',
    'caps': '/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf',
}


# ---------------------------------------------------------------- meshes
class Mesh:
    """Triangles gathered per shader: positions, normals, colours, uvs."""
    def __init__(self):
        self.parts = {}  # shader -> dict(pos, nrm, uv, tris)
        self.col = []    # collision triangles (positions)

    def _part(self, shader):
        return self.parts.setdefault(shader, dict(pos=[], nrm=[], uv=[], tris=[]))

    def quad(self, shader, p0, p1, p2, p3, uv0=(0, 0), uv1=(1, 0), uv2=(1, 1), uv3=(0, 1), double=False, collide=True):
        """p0..p3 counter-clockwise seen from the front; uvs per corner."""
        p = self._part(shader)
        pts = [np.asarray(p0, float), np.asarray(p1, float), np.asarray(p2, float), np.asarray(p3, float)]
        n = np.cross(pts[1] - pts[0], pts[2] - pts[0])
        n = n / max(np.linalg.norm(n), 1e-9)
        base = len(p['pos'])
        p['pos'] += pts
        p['nrm'] += [n] * 4
        p['uv'] += [uv0, uv1, uv2, uv3]
        p['tris'] += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
        if collide:
            self.col += [(pts[0], pts[1], pts[2]), (pts[0], pts[2], pts[3])]
        if double:
            self.quad(shader, p3, p2, p1, p0, uv3, uv2, uv1, uv0, collide=False)

    def box(self, shader, lo, hi, scale=1.0, faces='xyz', collide=True):
        """An axis-aligned box, textured in metres / `scale`; `faces`: which
        axes' faces to draw ('z' alone: top and bottom)."""
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        s = 1.0 / scale
        if 'y' in faces:  # front (-y) and back (+y)
            self.quad(shader, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1),
                      (0, 0), ((x1 - x0) * s, 0), ((x1 - x0) * s, (z1 - z0) * s), (0, (z1 - z0) * s), collide=collide)
            self.quad(shader, (x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1),
                      (0, 0), ((x1 - x0) * s, 0), ((x1 - x0) * s, (z1 - z0) * s), (0, (z1 - z0) * s), collide=collide)
        if 'x' in faces:
            self.quad(shader, (x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1),
                      (0, 0), ((y1 - y0) * s, 0), ((y1 - y0) * s, (z1 - z0) * s), (0, (z1 - z0) * s), collide=collide)
            self.quad(shader, (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1),
                      (0, 0), ((y1 - y0) * s, 0), ((y1 - y0) * s, (z1 - z0) * s), (0, (z1 - z0) * s), collide=collide)
        if 'z' in faces:
            self.quad(shader, (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
                      (0, 0), ((x1 - x0) * s, 0), ((x1 - x0) * s, (y1 - y0) * s), (0, (y1 - y0) * s), collide=collide)
            self.quad(shader, (x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0),
                      (0, 0), ((x1 - x0) * s, 0), ((x1 - x0) * s, (y1 - y0) * s), (0, (y1 - y0) * s), collide=collide)

    def post(self, shader, x, y, z0, z1, r, n=8, scale=1.0):
        """A round post (its collision a box)."""
        p = self._part(shader)
        base = len(p['pos'])
        for k in range(n + 1):
            a = 2 * math.pi * k / n
            c, s_ = math.cos(a), math.sin(a)
            for z, v in ((z0, 0.0), (z1, (z1 - z0) / scale)):
                p['pos'].append(np.array([x + r * c, y + r * s_, z]))
                p['nrm'].append(np.array([c, s_, 0.0]))
                p['uv'].append((2 * math.pi * r * k / n / scale, v))
        for k in range(n):
            a, b = base + 2 * k, base + 2 * (k + 1)
            p['tris'] += [(a, b, b + 1), (a, b + 1, a + 1)]
        lo, hi = (x - r, y - r, z0), (x + r, y + r, z1)
        for face in self._box_faces(lo, hi):
            self.col += face

    @staticmethod
    def _box_faces(lo, hi):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        c = lambda *a: np.array(a, float)  # noqa: E731
        quads = [((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)), ((x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1)),
                 ((x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1)), ((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)),
                 ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), ((x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0))]
        return [[(c(*q[0]), c(*q[1]), c(*q[2])), (c(*q[0]), c(*q[2]), c(*q[3]))] for q in quads]

    def write(self, drawable, shader_index):
        """Into a pcwrite.Drawable: `shader_index` maps a part's shader key to its index there."""
        for key, p in self.parts.items():
            pos = np.array(p['pos'], np.float32)
            nrm = np.array(p['nrm'], np.float32)
            col = np.full((len(pos), 4), 255, np.uint8)
            uv = np.array(p['uv'], np.float32)
            drawable.geometry(0, shader_index[key], pos, nrm, col, [uv], np.array(p['tris']))

    def collision(self, path):
        if not self.col:
            # (nothing to collide with - a heap of powder, a tray: no file,
            # rather than an empty mesh mapset.py would try to build a bound of)
            if os.path.exists(path):
                os.remove(path)
            return 0
        t = np.array(self.col, np.float32)  # (n, 3, 3)
        v = t.reshape(-1, 3)
        tri = np.arange(len(v)).reshape(-1, 3)
        np.savez(path, v=v, t=tri, m=np.zeros(len(tri), np.uint32))
        return len(tri)


# ---------------------------------------------------------------- textures
def sign_face(w=1024, h=256):
    """Benny's board: cream, teal script, coral rule, caps below."""
    img = Image.new('RGBA', (w, h), (246, 235, 208, 255))
    d = ImageDraw.Draw(img)
    d.rectangle([6, 6, w - 7, h - 7], outline=(232, 105, 74, 255), width=10)
    d.rectangle([22, 22, w - 23, h - 23], outline=(27, 127, 134, 255), width=3)
    script = ImageFont.truetype(FONTS['script'], 150)
    try:
        script.set_variation_by_name('Bold Italic')
    except Exception:
        pass
    caps = ImageFont.truetype(FONTS['caps'], 40)
    text = "Benny's"
    box = d.textbbox((0, 0), text, font=script)
    d.text(((w - (box[2] - box[0])) / 2 - box[0], 28 - box[1]), text, font=script, fill=(27, 127, 134, 255))
    sub = 'SEAFOOD  •  COCKTAILS  •  BAIT'
    box = d.textbbox((0, 0), sub, font=caps)
    d.text(((w - (box[2] - box[0])) / 2 - box[0], 188 - box[1]), sub, font=caps, fill=(232, 105, 74, 255))
    return np.asarray(img).astype(np.uint8)


def bar_board(w=512, h=128):
    img = Image.new('RGBA', (w, h), (27, 127, 134, 255))
    d = ImageDraw.Draw(img)
    d.rectangle([5, 5, w - 6, h - 6], outline=(246, 235, 208, 255), width=6)
    caps = ImageFont.truetype(FONTS['caps'], 72)
    text = 'TIKI BAR'
    box = d.textbbox((0, 0), text, font=caps)
    d.text(((w - (box[2] - box[0])) / 2 - box[0], (h - (box[3] - box[1])) / 2 - box[1]), text, font=caps,
           fill=(246, 235, 208, 255))
    return np.asarray(img).astype(np.uint8)


# ---------------------------------------------------------------- the props
def make_sign(lib):
    """Origin at the deck under the board's centre; the lettering faces -y
    (the sea) and +y (the deck) alike."""
    m = Mesh()
    for x in (-2.9, 2.9):
        m.post('pole', x, 0.0, 0.0, 3.75, 0.12)
    # the board (its faces lit at night), a cap beam over it
    x0, x1, z0, z1, t = -3.45, 3.45, 1.95, 3.55, 0.07
    m.quad('face', (x0, -t, z0), (x1, -t, z0), (x1, -t, z1), (x0, -t, z1), (0, 1), (1, 1), (1, 0), (0, 0))
    m.quad('face', (x1, t, z0), (x0, t, z0), (x0, t, z1), (x1, t, z1), (0, 1), (1, 1), (1, 0), (0, 0))
    m.box('wood', (x0, -t, z0), (x1, t, z1), 0.5, faces='xz')
    m.box('wood', (x0 - 0.05, -0.12, z1), (x1 + 0.05, 0.12, z1 + 0.1), 0.5)
    m.box('wood', (x0 - 0.05, -0.12, z0 - 0.1), (x1 + 0.05, 0.12, z0), 0.5)
    d = pcwrite.Drawable(lib)
    idx = {'pole': d.shader('gta_default.sps', {TEX: POLE}),
           'face': d.shader('gta_emissivenight.sps', {TEX: 'gk_bennys_sign'}, fallback='gta_default.sps'),
           'wood': d.shader('gta_default.sps', {TEX: WOOD})}
    m.write(d, idx)
    return d, m


def make_hut(lib):
    """A tiki bar: a plank counter, four posts, a back shelf, a frond roof
    with a thatch skirt; the counter faces -y."""
    m = Mesh()
    m.box('planks', (-2.4, -0.5, 0.0), (2.4, 0.5, 1.1), 1.0)
    m.box('wood', (-2.5, -0.62, 1.1), (2.5, 0.62, 1.18), 0.5)
    m.box('planks', (-2.4, 1.25, 0.0), (2.4, 1.55, 2.05), 1.0)          # back bar
    m.box('wood', (-2.4, 1.0, 1.35), (2.4, 1.55, 1.42), 0.5)            # its shelf
    m.box('wood', (-2.4, 1.0, 1.95), (2.4, 1.55, 2.05), 0.5)
    for x in (-2.6, 2.6):
        for y in (-0.95, 1.6):
            m.post('pole', x, y, 0.0, 2.9, 0.1)
    # the roof: a ridge over y = 0.3, eaves at y -1.5 and 2.1
    ridge, eave_f, eave_b, zr, ze = 0.3, -1.55, 2.15, 3.55, 2.85
    for x0, x1 in ((-3.1, 3.1),):
        # fronds on top (cut out, two sides), planks underneath
        m.quad('fronds', (x0, eave_f, ze), (x1, eave_f, ze), (x1, ridge, zr), (x0, ridge, zr), (0, 0), (6, 0), (6, 2), (0, 2), double=True)
        m.quad('fronds', (x1, eave_b, ze), (x0, eave_b, ze), (x0, ridge, zr), (x1, ridge, zr), (0, 0), (6, 0), (6, 2), (0, 2), double=True)
        m.quad('planks', (x1, eave_f, ze - 0.06), (x0, eave_f, ze - 0.06), (x0, ridge, zr - 0.06), (x1, ridge, zr - 0.06),
               (0, 0), (6, 0), (6, 2), (0, 2), collide=False)
        m.quad('planks', (x0, eave_b, ze - 0.06), (x1, eave_b, ze - 0.06), (x1, ridge, zr - 0.06), (x0, ridge, zr - 0.06),
               (0, 0), (6, 0), (6, 2), (0, 2), collide=False)
        # the thatch skirt hanging off the eaves and the gable ends
        for y, sgn in ((eave_f, -1), (eave_b, 1)):
            m.quad('fronds', (x0, y, ze - 0.55), (x1, y, ze - 0.55), (x1, y, ze + 0.05), (x0, y, ze + 0.05),
                   (0, 1), (6, 1), (6, 0), (0, 0), double=True, collide=False)
        for x in (x0, x1):
            m.quad('fronds', (x, eave_f, ze - 0.5), (x, eave_b, ze - 0.5), (x, ridge, zr - 0.1), (x, eave_f, ze + 0.0),
                   (0, 1), (3, 1), (1.5, 0), (0, 0), double=True, collide=False)
            m.quad('fronds', (x, eave_b, ze - 0.5), (x, eave_b, ze + 0.0), (x, ridge, zr - 0.1), (x, eave_f, ze - 0.5),
                   (0, 1), (0, 0), (1.5, 0), (3, 1), double=True, collide=False)
    # the board on the front eave
    m.quad('board', (-0.9, eave_f - 0.1, ze + 0.1), (0.9, eave_f - 0.1, ze + 0.1), (0.9, eave_f - 0.1, ze + 0.55),
           (-0.9, eave_f - 0.1, ze + 0.55), (0, 1), (1, 1), (1, 0), (0, 0), double=True, collide=False)
    d = pcwrite.Drawable(lib)
    idx = {'planks': d.shader('gta_default.sps', {TEX: PLANKS}),
           'wood': d.shader('gta_default.sps', {TEX: WOOD}),
           'pole': d.shader('gta_default.sps', {TEX: POLE}),
           'fronds': d.shader('gta_cutout.sps', {TEX: FRONDS}, fallback='gta_default.sps'),
           'board': d.shader('gta_emissivenight.sps', {TEX: 'gk_bennys_bar'}, fallback='gta_default.sps')}
    m.write(d, idx)
    return d, m


def ide_line(name, txd, info):
    lo, hi = info['bbox']
    return '%s, %s, 300, %d, 0, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 0.0, 0.0, 0.0, %.4f, null' % (
        name, txd, FLAGS, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], info['radius'])


def main():
    src, out = sys.argv[1], sys.argv[2]
    dirs = [src] + sys.argv[3:]
    os.makedirs(out, exist_ok=True)
    lib = pcwrite.ShaderLibrary(pcwrite.glob_wdr(dirs))
    print('shader samples:', ', '.join(sorted(lib.samples)))
    lines = []
    for name, maker in (('bsign', make_sign), ('bhut', make_hut)):
        d, m = maker(lib)
        info = d.write(os.path.join(out, name + '.wdr'))
        n = m.collision(os.path.join(out, name + '.colmesh.npz'))
        lines.append(ide_line(name, 'props', info))
        print('%s: %d shaders, %d triangles, %d of collision, box %s..%s' % (
            name, len(d.shaders), sum(len(g['tris']) for g in d.lods[0]), n,
            [round(v, 2) for v in info['bbox'][0]], [round(v, 2) for v in info['bbox'][1]]))
    textures = {}
    for tname, rgba in (('gk_bennys_sign', sign_face()), ('gk_bennys_bar', bar_board())):
        fmt, levels = maptex.encode_levels(rgba, 1024)
        textures[tname] = (levels[0][0], levels[0][1], fmt, [lv[2] for lv in levels])
        Image.fromarray(rgba).save(os.path.join(out, tname + '.png'))
    pcwrite.Dictionary(textures).write(os.path.join(out, 'props.wtd'))
    with open(os.path.join(out, 'custom.ide'), 'w') as f:
        f.write('objs\n' + '\n'.join(lines) + '\nend\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
