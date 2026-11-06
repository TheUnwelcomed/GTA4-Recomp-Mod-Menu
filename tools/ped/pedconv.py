"""A GTA V ped mod as GTA IV player outfit components (v1.7.46).

The player's clothes on the console are one drawable per component slot
(playerped.rpf: head, uppr, lowr, feet, hand, hair, teef, suse, sus2 - each
`<slot>_NNN_<u|r>.xdr` with its colour map in `<slot>_diff_NNN_a_<uni|whi>.xtd`,
its normal and specular maps embedded in the drawable), skinned to the
player's 90-bone skeleton. A V ped is one drawable per slot of its own
(the .ydd), skinned to V's 98-bone skeleton (the .yft), its textures in
the .ytd or embedded.

What this does with each V drawable the spec names:
  - the mesh turned round (V peds face -y with their left at +x; IV's face
    +y with their left at -x: 180 degrees about z);
  - every vertex's bones mapped to the IV bone of the same joint (V_TO_IV),
    its position moved by the joint's offset between the two bind poses
    (the same A-pose; the joints a few centimetres apart), so that the IV
    skeleton's rest matrices draw it where the modder put it;
  - the triangles cut into geometries of at most MAX_BONES bones each (the
    console's ped geometries carry 25), re-indexed per geometry;
  - written as a PC drawable (pcwrite, skinned geometry with four weights)
    whose shaders name the textures, then converted on a stock component
    (PedDrawable: the stock file's skeleton and header kept, the shader
    cloned from the stock gta_ped / gta_ped_reflect, the vertices in the
    console's layout) to the .xdr, its normal and specular maps embedded;
  - the colour map as the slot's .xtd (one texture variation, "a");
  - placeholders (a 2 mm triangle inside the pelvis) for the slots the suit
    covers, so Niko's own head, hands, feet, hair and teeth stay hidden.

usage: python3 -I pedconv.py <spec.json> <root> <out_dir>
The spec (ped/specs/beyond.json) names, relative to <root>, the V files, the
console template files (decoded with xlzx: see ped/README.md), the shader
library samples, the touched-up textures (pedtex.py's .wtd), the components
(slot, index, V drawable hash, textures) and the placeholders.
"""
import json
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, os.path.join(HERE, '..', 'maps'))
import batconv  # noqa: E402
import maptex  # noqa: E402
import pcwrite  # noqa: E402
import rsc5  # noqa: E402
import v_reader  # noqa: E402
import xenos  # noqa: E402

VP, PP = batconv.VP, batconv.PP
TEX, BUMP, SPEC = batconv.SAMPLER_TEXTURE, 0x46B7C64F, 0x608799C6
REFLECT = 0xC5BBAE28
XDR_TYPE, XTD_TYPE = 109, 7   # (a ped component is type 109 in the console's archives)
MAX_BONES = 25          # bones per geometry (the console's player components carry 25)
NAN_W = 0x7F800001
# The vertex colour the player's hands carry everywhere (stored A, R, G, B
# on the console: the red and blue an occlusion term, green a mask the
# hands keep at 136): a neutral value for a suit with no baked occlusion.
PED_COLOUR_PC = (216, 136, 216, 255)   # PC D3DCOLOR bytes: B, G, R, A

# V bone -> (IV bone, the V joint whose bind-pose offset moves its vertices).
# The helper bones (IK_, PH_, MH_, FB_, the rolls) ride on the nearest joint.
V_TO_IV = {
    'SKEL_ROOT': ('Char', 'SKEL_Pelvis'), 'IK_Root': ('Char', 'SKEL_Pelvis'),
    'SKEL_Pelvis': ('Char_Pelvis', 'SKEL_Pelvis'),
    'SKEL_Spine_Root': ('Char_Spine', 'SKEL_Spine0'),
    'SKEL_Spine0': ('Char_Spine', 'SKEL_Spine0'), 'SKEL_Spine1': ('Char_Spine1', 'SKEL_Spine1'),
    'SKEL_Spine2': ('Char_Spine2', 'SKEL_Spine2'), 'SKEL_Spine3': ('Char_Spine3', 'SKEL_Spine3'),
    'SKEL_Neck_1': ('Char_Neck', 'SKEL_Neck_1'), 'RB_Neck_1': ('Neck_Roll', 'SKEL_Neck_1'),
    'SKEL_Head': ('Char_Head', 'SKEL_Head'), 'IK_Head': ('Char_Head', 'SKEL_Head'),
    'FACIAL_facialRoot': ('Char_Head', 'SKEL_Head'),
}
for _side in 'LR':
    V_TO_IV.update({
        'SKEL_%s_Thigh' % _side: ('Char_%s_Thigh' % _side, 'SKEL_%s_Thigh' % _side),
        'RB_%s_ThighRoll' % _side: ('Char_%s_Thigh' % _side, 'SKEL_%s_Thigh' % _side),
        'SKEL_%s_Calf' % _side: ('Char_%s_Calf' % _side, 'SKEL_%s_Calf' % _side),
        'MH_%s_Knee' % _side: ('Char_%s_Calf' % _side, 'SKEL_%s_Calf' % _side),
        'SKEL_%s_Foot' % _side: ('Char_%s_Foot' % _side, 'SKEL_%s_Foot' % _side),
        'IK_%s_Foot' % _side: ('Char_%s_Foot' % _side, 'SKEL_%s_Foot' % _side),
        'PH_%s_Foot' % _side: ('Char_%s_Foot' % _side, 'SKEL_%s_Foot' % _side),
        'SKEL_%s_Toe0' % _side: ('Char_%s_Toe0' % _side, 'SKEL_%s_Toe0' % _side),
        'SKEL_%s_Clavicle' % _side: ('Char_%s_Clavicle' % _side, 'SKEL_%s_Clavicle' % _side),
        'SKEL_%s_UpperArm' % _side: ('Char_%s_UpperArm' % _side, 'SKEL_%s_UpperArm' % _side),
        'RB_%s_ArmRoll' % _side: ('%s_ArmRoll' % _side, 'SKEL_%s_UpperArm' % _side),
        'SKEL_%s_Forearm' % _side: ('Char_%s_Forearm' % _side, 'SKEL_%s_Forearm' % _side),
        'MH_%s_Elbow' % _side: ('Char_%s_Forearm' % _side, 'SKEL_%s_Forearm' % _side),
        'RB_%s_ForeArmRoll' % _side: ('Char_%s_ForeTwist' % _side, 'SKEL_%s_Forearm' % _side),
        'SKEL_%s_Hand' % _side: ('Char_%s_Hand' % _side, 'SKEL_%s_Hand' % _side),
        'IK_%s_Hand' % _side: ('Char_%s_Hand' % _side, 'SKEL_%s_Hand' % _side),
        'PH_%s_Hand' % _side: ('Char_%s_Hand' % _side, 'SKEL_%s_Hand' % _side),
    })
    for _f in range(5):
        for _k in range(3):
            v = 'SKEL_%s_Finger%d%d' % (_side, _f, _k)
            iv = 'Char_%s_Finger%d' % (_side, _f) if _k == 0 else 'Char_%s_Finger%d%d' % (_side, _f, _k)
            V_TO_IV[v] = (iv, v)


def iv_bone_of(name):
    """The IV bone and the V joint a V bone rides on (a face bone: the head)."""
    if name in V_TO_IV:
        return V_TO_IV[name]
    if name.startswith('FB_'):
        return ('Char_Head', 'SKEL_Head')
    raise KeyError('no IV bone for V bone %s' % name)


ROT = np.array([[-1.0, 0.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, 1.0]])  # 180 degrees about z


# ---------------------------------------------------------------- the console template
def load_console(path, body_path=None):
    """A console resource with its header-only file and its decoded body
    (xlzx unrsc); `path` as the exported file (big-endian header) or with
    the exporter's little-endian header, both taken."""
    raw = open(path, 'rb').read(12)
    magic, rtype, flags = struct.unpack('>III', raw)
    if magic != rsc5.RSC_MAGIC:
        magic, rtype, flags = struct.unpack('<III', raw)
    assert magic == rsc5.RSC_MAGIC, path
    body = open(body_path or path + '.bin', 'rb').read()
    return rsc5.Resource(body, flags, True, rtype, path)


class IvSkeleton:
    def __init__(self, template):
        d = rsc5.read_drawable(template, 0)
        assert d['skeleton_off'], 'the template component carries no skeleton'
        _, self.bones = batconv.read_skeleton(template, d['skeleton_off'])
        self.index = {b['name']: i for i, b in enumerate(self.bones)}
        pos, _ = batconv.pose(self.bones, [b['t'] for b in self.bones])
        self.pos = np.array(pos)


class PedDrawable(batconv.Converter):
    """A PC component drawable (pcwrite's) on a stock console component: the
    stock file's virtual segment kept whole (its skeleton, the drawable
    header), a new shader group (the stock shader of the .sps the spec
    gives each donor shader, the donor's texture names in it), the donor's
    geometries in the console layout with their bone tables as they come
    (the donor's tables already hold IV bone indexes), the normal and
    specular maps embedded as the stock files carry them, the bounds the
    donor's. The stock embedded dictionary is unhooked (its pixels are not
    in the rebuilt physical segment)."""

    def __init__(self, tmpl, donor, sps_of_shader, sources=(), embed=None, log=print):
        self.t = tmpl
        self.d = donor
        self.log = log
        self.sps_of_shader = sps_of_shader
        self.embed = embed or {}   # name -> pc_textures entry, for the embedded dictionary
        self.tf = {'drawable': rsc5.read_drawable(tmpl, 0), 'children': []}
        self.df = {'drawable': rsc5.read_drawable(donor, 0), 'children': []}
        self.v = batconv.Seg(tmpl.virt, batconv.VIRTUAL_PAGE)
        self.p = batconv.Seg()
        self.decls = {}
        self.uv_filled = 0
        self.zero_vectors = 0
        self.texture_names = set()
        self.options = {}
        self.face_rules, self.split_rules = [], []
        self.donor_bones = []
        self.pivots = {}
        self.shader_sources = list(sources)
        self._templates()
        self.bone_map = {}

    def _templates(self):
        t = self.t
        d = self.tf['drawable']
        g0 = d['lods'][0][0]['geometries'][0]
        self.t_geom = t.virt[g0['off']:g0['off'] + 0x50]
        self.t_vb = t.virt[g0['vb_off']:g0['vb_off'] + 0x40]
        self.t_ib = t.virt[g0['ib_off']:g0['ib_off'] + 0x30]
        decl = t.vptr(g0['vb_off'] + 16)
        self.t_decl = t.virt[decl:decl + 16]
        m0 = d['lods'][0][0]['off']
        self.t_model_main = t.virt[m0:m0 + 0x20]
        coll = batconv.lod_collections(t, d['off'])[0]
        self.t_coll = t.virt[coll:coll + 8]
        sg = d['shader_group']['off']
        self.t_sg = t.virt[sg:sg + 0x50]
        self.t_shaders = {}
        for res in [t] + self.shader_sources:
            rd = rsc5.read_drawable(res, 0)
            rsg = rd['shader_group']['off']
            usage_arr, extra_arr = res.vptr(rsg + 64), res.vptr(rsg + 72)
            for i, s in enumerate(rd['shader_group']['shaders']):
                sps = batconv.shader_sps(res, s['off'])
                self.t_shaders.setdefault(sps, dict(res=res, off=s['off'], usage=res.u32(usage_arr + 4 * i),
                                                    extra=res.u32(extra_arr + 4 * i)))
        self.t_texobj = None
        for s in d['shader_group']['shaders']:
            for p in batconv.shader_params(t, s['off']):
                if p['type'] == 0 and p['value'] is not None and \
                        struct.unpack_from('>I', t.virt, p['value'])[0] == batconv.TEXTURE_REFERENCE_ID:
                    self.t_texobj = t.virt[p['value']:p['value'] + 32]
                    break
            if self.t_texobj:
                break
        assert self.t_texobj, 'the template names no texture by reference'
        # the stock embedded dictionary: its header, a texture record and D3D header with mips, to clone
        td = t.vptr(sg + 4)
        assert td is not None, 'the template embeds no texture dictionary'
        self.t_dict = t.virt[td:td + 0x20]
        self.t_texinfo = self.t_d3d = None
        tl, n = t.vptr(td + 24), t.u16(td + 20)
        for i in range(n):
            info = t.vptr(tl + 4 * i)
            d3d = t.vptr(info + 24)
            if t.u32(info + 32) > 1:
                self.t_texinfo, self.t_d3d = t.virt[info:info + 0x40], t.virt[d3d:d3d + 0x34]
                break
        assert self.t_texinfo, 'the template embeds no texture with mips'
        assert struct.unpack_from('>I', self.t_vb, 28)[0] == VP | (g0['vb_off'] + 0x20)
        assert struct.unpack_from('>I', self.t_ib, 12)[0] == VP | (g0['ib_off'] + 0x10)

    def build_shader_group(self):
        d = self.d
        dsg = self.df['drawable']['shader_group']
        new_shaders, usages, extras = [], [], []
        self.shader_report = []
        for si, s in enumerate(dsg['shaders']):
            tsps = self.sps_of_shader[si]
            assert tsps in self.t_shaders, 'no stock shader for %s' % tsps
            ts = self.t_shaders[tsps]
            new_shaders.append(self._clone_shader(ts, s['off']))
            usages.append(ts['usage'])
            extras.append(ts['extra'])
            self.shader_report.append((batconv.shader_sps(d, s['off']), tsps))
        self.shader_slot = {}
        self.shader_usage = list(usages)
        n = len(new_shaders)
        v = self.v
        sg = v.put(self.t_sg)
        v.w32(sg + 4, 0)   # (the embedded dictionary: hooked up in run once its pixels are placed)
        arr = v.alloc(4 * n)
        for i, so in enumerate(new_shaders):
            v.w32(arr + 4 * i, VP | so)
        v.w32(sg + 8, VP | arr)
        v.w16(sg + 12, n)
        v.w16(sg + 14, n)
        for k, values in ((64, usages), (72, extras)):
            a = v.alloc(4 * n)
            for i, val in enumerate(values):
                v.w32(a + 4 * i, val)
            v.w32(sg + k, VP | a)
            v.w16(sg + k + 4, n)
            v.w16(sg + k + 6, n)
        return sg

    def convert_drawable_lods(self, ddraw):
        out = []
        for coll in batconv.lod_collections(self.d, ddraw['off']):
            if coll is None:
                out.append(None)
                continue
            lod = []
            for m in batconv.model_collection(self.d, coll):
                gs = [g for g in rsc5.ptr_collection(self.d, m + 4) if g is not None]
                smap_p = self.d.vptr(m + 16)
                smap = [self.d.u16(smap_p + 2 * k) for k in range(len(gs))]
                geoms, shaders = [], []
                for g, si in zip(gs, smap):
                    whole = self.convert_geometry(rsc5.read_geometry(self.d, g), self.shader_usage[si])
                    assert len(whole['bones']) <= MAX_BONES, 'geometry with %d bones' % len(whole['bones'])
                    parts = batconv.split_geometry(whole)
                    geoms += parts
                    shaders += [si] * len(parts)
                lod.append((geoms, shaders))
            out.append(lod)
        return out

    def convert_geometry(self, g, need=0):
        # the donor's bone tables hold IV skeleton indexes already
        bp = self.d.vptr(g['off'] + 56)
        bc = self.d.u16(g['off'] + 62)
        self.bone_map = {self.d.u16(bp + 2 * k): self.d.u16(bp + 2 * k) for k in range(bc)} if bp else {}
        return batconv.Converter.convert_geometry(self, g, need)

    def place_buffers(self, *lod_sets):
        """The geometries' buffers and the embedded textures' pixels (level 0
        and the mip region each, on a tile's bytes as the stock textures)."""
        geoms, seen = [], set()
        for lods in lod_sets:
            for lod in lods:
                for gs, _ in (lod or []):
                    for c in gs:
                        if id(c) not in seen:
                            seen.add(id(c))
                            geoms.append(c)
        objs = []
        for c in geoms:
            objs += [(c['vdata'], 0x100), (c['idata'], 0x100)]
        self.tex_plan = []
        for name in sorted(self.embed):
            src = self.embed[name]
            fmt, levels, how = maptex.texture_levels(src, src.get('side') or src['w'])
            xfmt, bpp = maptex.XENOS[fmt]
            w0, h0 = levels[0][0], levels[0][1]
            bw0, bh0 = (w0 + 3) // 4, (h0 + 3) // 4
            base = bytearray(xenos.align(bw0, 32) * xenos.align(bh0, 32) * bpp)
            xenos.tile_into(base, xenos.swap16(levels[0][2]), bw0, bh0, bpp, xenos.align(bw0, 32))
            tile_align = 32 * 32 * bpp
            k_base = len(objs)
            objs.append((bytes(base), tile_align))
            k_mip = None
            if len(levels) > 1:
                layout, size = xenos.mip_layout(w0, h0, len(levels), bpp)
                region = bytearray(size)
                for L, (lw, lh, data) in zip(layout, levels[1:]):
                    sub = bytearray(L['pitch'] * L['rows'] * bpp)
                    xenos.tile_into(sub, xenos.swap16(data), L['bw'], L['bh'], bpp, L['pitch'], L['x'], L['y'])
                    region[L['offset']:L['offset'] + len(sub)] = sub
                k_mip = len(objs)
                objs.append((bytes(region), tile_align))
            self.tex_plan.append(dict(name=name, fmt=fmt, xfmt=xfmt, w=w0, h=h0, bw=bw0, levels=len(levels),
                                      k_base=k_base, k_mip=k_mip, how=how))
        phys, offs, count, shift = batconv.place_physical(objs)
        for k, c in enumerate(geoms):
            c['vo'], c['io'] = offs[2 * k], offs[2 * k + 1]
        for t in self.tex_plan:
            t['base'] = offs[t['k_base']]
            t['mip'] = offs[t['k_mip']] if t['k_mip'] is not None else None
        self.p = batconv.Seg(phys)
        self.p_count, self.p_shift = count, shift

    def emit_dictionary(self):
        """The embedded dictionary (the stock one's header cloned) over the
        placed textures; returns its offset, or 0 with none."""
        if not self.tex_plan:
            return 0
        v = self.v
        td = v.put(self.t_dict)
        v.w32(td + 4, 0)
        v.w32(td + 8, 0)
        entries = []
        for t in self.tex_plan:
            f = list(struct.unpack_from('>6I', self.t_d3d, 28))
            f[0] = (f[0] & ~(0x1FF << 22)) | ((xenos.align(t['bw'], 32) * 4 // 32) << 22) | (1 << 31)
            f[1] = (f[1] & 0xFC0) | t['xfmt'] | ((PP | t['base']) & 0xFFFFF000)
            f[2] = (f[2] & ~0x03FFFFFF) | (t['w'] - 1) | ((t['h'] - 1) << 13)
            f[4] = (f[4] & ~(0xFF << 2)) | ((t['levels'] - 1) << 6)
            f[5] = (f[5] & 0x1FF) | (1 << 9) | ((1 << 11) if t['levels'] > 1 else 0)
            if t['mip'] is not None:
                f[5] |= (PP | t['mip']) & 0xFFFFF000
            info = v.put(self.t_texinfo)
            d3d = v.put(self.t_d3d)
            struct.pack_into('>6I', v.buf, d3d + 28, *f)
            v.w32(info + 20, VP | v.cstr('pack:/%s.dds' % t['name']))
            v.w32(info + 24, VP | d3d)
            v.w16(info + 28, t['w'])
            v.w16(info + 30, t['h'])
            v.w32(info + 32, t['levels'])
            entries.append((batconv.joaat(t['name']), info))
        entries.sort()
        hs = v.alloc(4 * len(entries))
        ls = v.alloc(4 * len(entries))
        for k, (h, info) in enumerate(entries):
            v.w32(hs + 4 * k, h)
            v.w32(ls + 4 * k, VP | info)
        v.w32(td + 16, VP | hs)
        v.w16(td + 20, len(entries))
        v.w16(td + 22, len(entries))
        v.w32(td + 24, VP | ls)
        v.w16(td + 28, len(entries))
        v.w16(td + 30, len(entries))
        return td

    def run(self):
        v = self.v
        sg = self.build_shader_group()
        lods = self.convert_drawable_lods(self.df['drawable'])
        self._lods = lods
        self.check_shader_inputs(lods)
        self.place_buffers(lods)
        td = self.emit_dictionary()
        v.w32(sg + 4, (VP | td) if td else 0)
        v.w32(8, VP | sg)
        for slot in range(4):
            lod = lods[slot] if slot < len(lods) else None
            v.w32(0x40 + 4 * slot, (VP | self.emit_lod(lod, self.t_model_main)) if lod else 0)
            v.w32(0x60 + 4 * slot, 1 if lod else 0xFFFFFFFF)
        d = self.d
        for k in (0x10, 0x20, 0x30):
            x, y, z = struct.unpack_from('<3f', d.virt, k)
            struct.pack_into('>3fI', v.buf, k, x, y, z, NAN_W)
        struct.pack_into('>f', v.buf, 0x70, struct.unpack_from('<f', d.virt, 0x70)[0])
        flags, vtot, ptot = batconv.make_flags(self.t.flags, len(v.buf), self.p_count, self.p_shift)
        assert ptot == len(self.p.buf)
        return flags, bytes(v.buf) + b'\0' * (vtot - len(v.buf)) + bytes(self.p.buf)


# ---------------------------------------------------------------- the mesh
def weights_bytes(w):
    """Four float weights per vertex as bytes summing to 255 (the largest
    takes the rounding)."""
    w = np.asarray(w, np.float64)
    w = w / np.maximum(w.sum(1, keepdims=True), 1e-9)
    b = np.floor(w * 255.0 + 0.5).astype(np.int64)
    short = 255 - b.sum(1)
    top = w.argmax(1)
    b[np.arange(len(b)), top] += short
    assert (b >= 0).all() and (b.sum(1) == 255).all()
    return b.astype(np.uint8)


def split_by_bones(tris, vbones, vweights, limit=MAX_BONES):
    """Triangles grouped so each group's vertices use at most `limit`
    distinct bones: the triangles taken in the order of their heaviest bone
    (a walk down the skeleton), a new group begun when the next would not
    fit. Returns [(triangle rows, sorted bone list)]."""
    # the heaviest bone of each triangle's three vertices, the lowest index first
    heavy = vbones[np.arange(len(vbones)), vweights.argmax(1)]
    key = heavy[tris].min(1)
    order = np.argsort(key, kind='stable')
    groups, cur, cur_bones = [], [], set()
    for t in order:
        used = {int(b) for v in tris[t] for b, w in zip(vbones[v], vweights[v]) if w > 0}
        if cur and len(cur_bones | used) > limit:
            groups.append((np.array(cur), sorted(cur_bones)))
            cur, cur_bones = [], set()
        cur.append(t)
        cur_bones |= used
    if cur:
        groups.append((np.array(cur), sorted(cur_bones)))
    return groups


def convert_mesh(vdraw, vbones, iv, log=print):
    """A V drawable's LOD 0 as IV-space skinned geometries: a list of dicts
    (pos, nrm, tan, uv, col, tris, table, weights, indices, shader)."""
    vindex = {b['name']: i for i, b in enumerate(vbones)}
    vpos = np.array([b['pos'] for b in vbones]) @ ROT.T
    # each V bone's IV bone and offset (IV joint position less the turned V joint's)
    iv_of = np.zeros(len(vbones), np.int64)
    delta = np.zeros((len(vbones), 3))
    for i, b in enumerate(vbones):
        ivn, joint = iv_bone_of(b['name'])
        iv_of[i] = iv.index[ivn]
        delta[i] = iv.pos[iv.index[iv_bone_of(joint)[0]]] - vpos[vindex[joint]]
    out = []
    for m in vdraw['lods'][0]:
        for g in m['geometries']:
            c = g['components']
            pos = c['position'][:, :3].astype(np.float64) @ ROT.T
            n = len(pos)
            ids = g['bone_ids']
            bi = np.rint(c['blendindices'] * 255.0).astype(np.int64)
            if ids:
                bi = np.array(ids, np.int64)[np.clip(bi, 0, len(ids) - 1)]
            bw = c['blendweights'].astype(np.float64)
            bw = np.where(bw > 0, bw, 0.0)
            bw /= np.maximum(bw.sum(1, keepdims=True), 1e-9)
            pos = pos + np.einsum('vk,vkj->vj', bw, delta[bi])
            nrm = c['normal'][:, :3].astype(np.float64) @ ROT.T if 'normal' in c else None
            tan = None
            if 'tangent' in c and c['tangent'].shape[1] >= 4:
                tan = np.concatenate([c['tangent'][:, :3] @ ROT.T, c['tangent'][:, 3:4]], 1)
            uv = c['texcoord0'][:, :2] if 'texcoord0' in c else np.zeros((n, 2), np.float32)
            ivb = iv_of[bi]
            wb = weights_bytes(bw)
            tris = g['indices'].reshape(-1, 3).astype(np.int64)
            for rows, table in split_by_bones(tris, ivb, wb):
                part = tris[rows]
                used = np.unique(part)
                remap = np.full(n, -1, np.int64)
                remap[used] = np.arange(len(used))
                slot = {b: k for k, b in enumerate(table)}
                idx = np.zeros((len(used), 4), np.uint8)
                for k in range(4):
                    idx[:, k] = [slot[int(b)] if w > 0 else 0 for b, w in zip(ivb[used, k], wb[used, k])]
                out.append(dict(pos=pos[used].astype(np.float32), nrm=None if nrm is None else nrm[used].astype(np.float32),
                                tan=None if tan is None else tan[used].astype(np.float32), uv=uv[used].astype(np.float32),
                                tris=remap[part], table=[int(b) for b in table], weights=wb[used], indices=idx,
                                shader=g['shader']))
            log('  geometry: %d vertices, %d triangles, %d bones -> %d geometries' % (
                n, len(tris), len(np.unique(ivb[wb > 0])), sum(1 for _ in split_by_bones(tris, ivb, wb))))
    return out


def write_component(parts, textures, lib, out_wdr, log=print):
    """The parts as one PC drawable: one shader (gta_normal_spec, its
    textures by name), every part a skinned geometry."""
    d = pcwrite.Drawable(lib)
    s = d.shader('gta_normal_spec.sps', {TEX: textures['diffuse'], BUMP: textures['normal'], SPEC: textures['spec']})
    col = np.tile(np.array(PED_COLOUR_PC, np.uint8), (1, 1))
    for part in parts:
        n = len(part['pos'])
        d.geometry(0, s, part['pos'], part['nrm'], np.tile(col, (n, 1)), [part['uv']], part['tris'], part['tan'],
                   bones=part['table'], skin=(part['weights'], part['indices']))
    info = d.write(out_wdr)
    log('  %s: %d geometries, radius %.3f' % (os.path.basename(out_wdr), sum(len(l) for l in d.lods), info['radius']))
    return info


def placeholder_parts(iv):
    """One 2 mm triangle inside the pelvis, on the root bone."""
    c = iv.pos[iv.index['Char_Pelvis']]
    pos = np.array([c, c + [0.002, 0, 0], c + [0, 0.002, 0]], np.float32)
    return [dict(pos=pos, nrm=np.tile([0, 0, 1.0], (3, 1)).astype(np.float32),
                 tan=np.tile([1.0, 0, 0, 1.0], (3, 1)).astype(np.float32), uv=np.zeros((3, 2), np.float32),
                 tris=np.array([[0, 1, 2]]), table=[0], weights=np.tile([255, 0, 0, 0], (3, 1)).astype(np.uint8),
                 indices=np.zeros((3, 4), np.uint8), shader=0)]


def scaled_parts(parts, iv, bone, sx):
    """The parts with x scaled about a bone's position (the wings half open)."""
    c = iv.pos[iv.index[bone]]
    out = []
    for p in parts:
        q = dict(p)
        pos = p['pos'].astype(np.float64).copy()
        pos[:, 0] = c[0] + (pos[:, 0] - c[0]) * sx
        q['pos'] = pos.astype(np.float32)
        out.append(q)
    return out


# ---------------------------------------------------------------- textures
def pc_texture_entry(path):
    r = rsc5.Resource.pc(path)
    return r, batconv.pc_textures(r, 0)


def pack(rtype, flags, body, out_path, xlzx):
    """The body LZX-packed behind a little-endian RSC header: the form the
    console's playerped.rpf stores its files in, and the port's installer
    unpacks them (the .img archives' files carry the header big-endian)."""
    tmp = out_path + '.body'
    with open(tmp, 'wb') as f:
        f.write(body)
    subprocess.run([xlzx, 'rsc', str(rtype), '0x%08X' % flags, tmp, out_path], check=True, capture_output=True)
    os.remove(tmp)
    raw = bytearray(open(out_path, 'rb').read())
    magic, t, fl = struct.unpack_from('>3I', raw, 0)
    assert magic == rsc5.RSC_MAGIC and t == rtype and fl == flags, 'xlzx wrote an unexpected header'
    struct.pack_into('<3I', raw, 0, magic, t, fl)
    open(out_path, 'wb').write(raw)


def build_xtd(tmpl, textures, out_path, xlzx, side):
    flags, body, report = maptex.build(tmpl, textures, log=lambda *a: None, side=side, detail=side)
    verdict, worst = maptex.check(body, flags, textures, log=lambda *a: None)
    pack(XTD_TYPE, flags, body, out_path, xlzx)
    return flags, report, verdict


def pack_xdr(flags, body, out_path, xlzx):
    pack(XDR_TYPE, flags, body, out_path, xlzx)


# ---------------------------------------------------------------- main
def main():
    spec_path, base, out = sys.argv[1:4]
    spec = json.load(open(spec_path))

    def P(p):
        return p if os.path.isabs(p) else os.path.normpath(os.path.join(base, p))
    os.makedirs(out, exist_ok=True)
    xlzx = P(spec['xlzx'])
    log = print
    tmpl = load_console(P(spec['template']), P(spec['template_body']) if spec.get('template_body') else None)
    sources = [load_console(P(s), P(b) if b else None) for s, b in
               [(x, spec.get('shader_source_bodies', {}).get(x)) for x in spec.get('shader_sources', [])]]
    iv = IvSkeleton(tmpl)
    lib = pcwrite.ShaderLibrary([P(s) for s in spec['pc_samples']])
    assert 'gta_normal_spec.sps' in lib, 'the PC samples carry no gta_normal_spec.sps'
    xtd_tmpl = maptex.Template(load_console(P(spec['xtd_template'])))
    ydd = v_reader.read_ydd(P(spec['ydd']))
    vbones = v_reader.read_yft(P(spec['yft']))['skeleton']
    # the touched-up textures (ped/pedtex.py's .wtd): one entry per name
    tex_res, tex = pc_texture_entry(P(spec['textures']))
    flags_lines = []
    for comp in spec['components']:
        slot, index, kind = comp['slot'], comp['index'], comp.get('kind', 'mesh')
        suffix = comp.get('suffix', 'u')
        name = '%s_%03d_%s' % (slot, index, suffix)
        log('%s (%s):' % (name, kind))
        if kind == 'placeholder':
            parts = placeholder_parts(iv)
        else:
            vdraw = ydd[int(comp['hash'])]
            parts = convert_mesh(vdraw, vbones, iv, log)
            if comp.get('scale_x'):
                parts = scaled_parts(parts, iv, comp.get('scale_about', 'Char_Spine3'), float(comp['scale_x']))
        diff_name = '%s_diff_%03d_a_%s' % (slot, index, comp.get('race', 'uni'))
        names = dict(diffuse=diff_name, normal='%s_normal_%03d' % (slot, index), spec='%s_spec_%03d' % (slot, index))
        wdr = os.path.join(out, name + '.wdr')
        write_component(parts, names, lib, wdr, log)
        donor = rsc5.Resource.pc(wdr)
        src_tex = comp.get('textures', spec.get('textures_of', {}))
        embed = {}
        if kind != 'placeholder':
            for k in ('normal', 'spec'):
                e = dict(tex[src_tex[k].lower()])
                e['name'] = names[k]
                if comp.get('embed_side'):
                    e['side'] = int(comp['embed_side']) if k == 'normal' else int(comp['embed_side']) // 2
                embed[names[k]] = e
        conv = PedDrawable(tmpl, donor, {0: comp.get('sps', 'gta_ped.sps')}, sources, embed, log)
        flags, body = conv.run()
        pack_xdr(flags, body, os.path.join(out, name + '.xdr'), xlzx)
        flags_lines.append('%s.xdr %08X' % (name, flags))
        log('  %s.xdr: shader %s, %d geometries, %d KiB (embedded: %s)' % (
            name, conv.shader_report[0][1], sum(len(gs) for gs, _ in (conv._lods[0] or [])),
            os.path.getsize(os.path.join(out, name + '.xdr')) >> 10,
            ', '.join('%s %dx%d %s' % (t['name'], t['w'], t['h'], t['how']) for t in conv.tex_plan) or 'none'))
        # the colour map's dictionary
        if kind != 'placeholder':
            e = dict(tex[src_tex['diffuse'].lower()])
            e['name'] = diff_name
            xtd = os.path.join(out, diff_name + '.xtd')
            xflags, report, verdict = build_xtd(xtd_tmpl, {diff_name: e}, xtd, xlzx, int(comp.get('side', 1024)))
            flags_lines.append('%s.xtd %08X' % (diff_name, xflags))
            log('  %s.xtd: %s, %s, %d KiB' % (diff_name, report[0], verdict, os.path.getsize(xtd) >> 10))
        else:
            # a placeholder still needs a colour map of its own: a tiny grey one
            e = dict(tex[spec['placeholder_texture'].lower()])
            e['name'] = diff_name
            xtd = os.path.join(out, diff_name + '.xtd')
            xflags, report, verdict = build_xtd(xtd_tmpl, {diff_name: e}, xtd, xlzx, 64)
            flags_lines.append('%s.xtd %08X' % (diff_name, xflags))
    with open(os.path.join(out, 'flags.txt'), 'w') as f:
        f.write('\n'.join(flags_lines) + '\n')
    log('done: %d files' % len(flags_lines))


if __name__ == '__main__':
    main()
