"""PC GTA IV vehicle -> Xbox 360 vehicle, by transplant into a stock 360 file.

Inputs:  stock 360 template (decoded fbi.xft/.xtd bodies + their RSC headers)
         PC donor vehicle (.wft/.wtd)
Output:  360 bodies + RSC flags for fbi.xft and fbi.xtd (pack with xlzx rsc).

Everything structural that this does was verified against the stock FBI,
which exists on both platforms (see the verification log in the repo):
  vertex element encodings, texture tiling/endian, skeleton/matrix parity,
  shader parameter subsetting, VB/IB/D3D header layouts.

The 360 template keeps: physics, fragment groups/children, skeleton topology,
all other children. The donor provides: main drawable LODs, the wheel child's
LODs, shaders (cloned from same-named 360 shaders, values by name hash),
textures (merged into the .xtd), bounds, and bone transforms (by bone name).
"""
import struct
import sys

import rsc5
import xenos

VP = 0x50000000
PP = 0x60000000

# donor shader (.sps) -> template shader (.sps) when the template lacks it
SPS_FALLBACK = {
    'gta_vehicle_interior.sps': 'gta_vehicle_interior2.sps',
    'gta_default.sps': 'gta_vehicle_interior2.sps',  # needs 0059, as the donor part supplies
}
BONE_ALIAS = {'chasiss': 'chassis'}  # a typo in the Batmobile's skeleton

TYPES_360 = 0xAA1111111199A996  # pos f3, bw/bi/col c4, normal dec3n, uv half2, tangent/binormal dec3n
SIZE_360 = {0: 12, 1: 4, 2: 4, 3: 4, 4: 4, 5: 4, 14: 4, 15: 4}
SIZE_PC = {0: 12, 1: 4, 2: 4, 3: 12, 4: 4, 5: 4, 14: 16, 15: 12}
for _i in range(6, 14):
    SIZE_360[_i] = 4
    SIZE_PC[_i] = 8
PC_TYPES = 0x6755555555996996


def joaat(s):
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def align(v, a):
    return (v + a - 1) // a * a


def swap_words(b):
    """LE 32-bit words -> BE 32-bit words."""
    n = len(b) // 4
    return struct.pack('>%dI' % n, *struct.unpack('<%dI' % n, b[:n * 4]))


def encode_flags(template_flags, vsize, psize):
    """RSC5 size flags with the 6-bit counts the stock tools emit."""
    def enc(size):
        size = align(max(size, 4096), 4096)  # whole pages, as every stock file has
        b = 0
        while -(-size // (1 << (b + 8))) > 0x3F:
            b += 1
        a = -(-size // (1 << (b + 8)))
        return a, b, a << (b + 8)
    va, vb, vtot = enc(vsize)
    pa, pb, ptot = enc(psize)
    flags = (template_flags & 0xC0000000) | va | (vb << 11) | (pa << 15) | (pb << 26)
    assert rsc5.seg_sizes(flags) == (vtot, ptot)
    return flags, vtot, ptot


class Seg:
    def __init__(self, data=b''):
        self.buf = bytearray(data)

    def alloc(self, size, alignment=16):
        pad = (-len(self.buf)) % alignment
        self.buf += b'\0' * pad
        off = len(self.buf)
        self.buf += b'\0' * size
        return off

    def put(self, data, alignment=16):
        off = self.alloc(len(data), alignment)
        self.buf[off:off + len(data)] = data
        return off

    def w32(self, off, v):
        struct.pack_into('>I', self.buf, off, v & 0xFFFFFFFF)

    def w16(self, off, v):
        struct.pack_into('>H', self.buf, off, v & 0xFFFF)

    def cstr(self, s):
        return self.put(s.encode('latin-1') + b'\0', 4)


# ---------------------------------------------------------------- vertices
def _half(v):
    """float -> half, truncating toward zero, as Rockstar's 360 tools do
    (identity test: every UV differing from theirs was one ulp further out)."""
    b = struct.unpack('<I', struct.pack('<f', v))[0]
    s = (b >> 16) & 0x8000
    e = ((b >> 23) & 0xFF) - 112
    m = b & 0x7FFFFF
    if ((b >> 23) & 0xFF) == 0xFF or e >= 31:
        return s | 0x7BFF
    if e <= 0:
        if e < -10:
            return s
        return s | (((m | 0x800000) >> (1 - e)) >> 13)
    return s | (e << 10) | (m >> 13)


def _dec3n(x, y, z, w=0):
    def q(v):  # truncation toward zero, matching Rockstar's packing
        return max(-511, min(511, int(v * 511.0))) & 0x3FF
    return q(x) | (q(y) << 10) | (q(z) << 20) | ((int(w) & 3) << 30)


def vertex_layout(usage, sizes):
    out, off = [], 0
    for i in range(16):
        if usage & (1 << i):
            out.append((i, off))
            off += sizes[i]
    return out, off


def convert_vertices(src, usage, count, pc_stride):
    pc_elems, pc_size = vertex_layout(usage, SIZE_PC)
    x_elems, x_size = vertex_layout(usage, SIZE_360)
    assert pc_size == pc_stride, (hex(usage), pc_size, pc_stride)
    out = bytearray(x_size * count)
    for v in range(count):
        base = v * pc_stride
        obase = v * x_size
        for (i, po), (_, xo) in zip(pc_elems, x_elems):
            p = base + po
            o = obase + xo
            if i == 0:
                out[o:o + 12] = struct.pack('>3f', *struct.unpack_from('<3f', src, p))
            elif i in (1, 2, 4, 5):
                out[o:o + 4] = src[p:p + 4][::-1]
            elif i in (3, 15):
                x, y, z = struct.unpack_from('<3f', src, p)
                struct.pack_into('>I', out, o, _dec3n(x, y, z, 0))
            elif i == 14:
                x, y, z, w = struct.unpack_from('<4f', src, p)
                struct.pack_into('>I', out, o, _dec3n(x, y, z, -1 if w < 0 else 1))
            else:
                u, t = struct.unpack_from('<2f', src, p)
                struct.pack_into('>HH', out, o, _half(u), _half(t))
    return bytes(out), x_size


# ---------------------------------------------------------------- reading
def model_collection(r, coll_off):
    return [m for m in rsc5.ptr_collection(r, coll_off) if m is not None]


def lod_collections(r, drawable_off):
    return [r.vptr(drawable_off + 64 + 4 * i) for i in range(4)]


def shader_params(r, s):
    b = s + 8
    n = r.u32(b + 0x14)
    po, pt, pn = r.vptr(b + 12), r.vptr(b + 28), r.vptr(b + 44)
    out = []
    for i in range(n):
        out.append(dict(hash=r.u32(pn + 4 * i), type=r.u8(pt + i),
                        value=r.vptr(po + 4 * i)))
    return out


def shader_sps(r, s):
    p = r.vptr(s + 0x48)
    return r.cstr(p) if p is not None else None


def bones(r, skel):
    bo = r.vptr(skel)
    n = r.u16(skel + 20)
    return bo, [r.cstr(r.vptr(bo + 0xE0 * i)) for i in range(n)]


# ---------------------------------------------------------------- the transplant
class Converter:
    def __init__(self, tmpl, donor, log=print):
        self.t = tmpl     # rsc5.Resource (360, big-endian)
        self.d = donor    # rsc5.Resource (PC)
        self.log = log
        self.v = Seg(tmpl.virt)   # template virt kept whole; new structs appended
        self.p = Seg()            # physical rebuilt: donor geometry only
        self.tf = rsc5.read_fragtype(tmpl)
        self.df = rsc5.read_fragtype(donor)
        self.decls = {}
        self.texture_names = set()
        self._templates()

    # Raw template structures to clone (vtables and constant fields).
    def _templates(self):
        t = self.t
        d = self.tf['drawable']
        g0 = d['lods'][0][0]['geometries'][0]
        self.t_geom = t.virt[g0['off']:g0['off'] + 0x50]
        self.t_vb = t.virt[g0['vb_off']:g0['vb_off'] + 0x40]
        self.t_ib = t.virt[g0['ib_off']:g0['ib_off'] + 0x30]
        decl = t.vptr(g0['vb_off'] + 16)
        self.t_decl = t.virt[decl:decl + 16]
        main_model = d['lods'][0][0]['off']
        self.t_model_main = t.virt[main_model:main_model + 0x20]
        wheel = self._template_wheel()
        wm = wheel['lods'][0][0]['off']
        self.t_model_wheel = t.virt[wm:wm + 0x20]
        self.t_coll = t.virt[lod_collections(t, d['off'])[0]:lod_collections(t, d['off'])[0] + 8]
        sg = d['shader_group']['off']
        self.t_sg = t.virt[sg:sg + 0x50]
        # template shaders by .sps, with their group-level per-shader values
        self.t_shaders = {}
        usage_arr, extra_arr = t.vptr(sg + 64), t.vptr(sg + 72)
        for i, s in enumerate(d['shader_group']['shaders']):
            sps = shader_sps(t, s['off'])
            self.t_shaders.setdefault(sps, dict(off=s['off'], usage=t.u32(usage_arr + 4 * i),
                                                extra=t.u32(extra_arr + 4 * i)))
        # a template texture-parameter object to clone when the template slot is empty
        self.t_texobj = None
        for s in d['shader_group']['shaders']:
            for p in shader_params(t, s['off']):
                if p['type'] == 0 and p['value'] is not None:
                    self.t_texobj = t.virt[p['value']:p['value'] + 32]
                    break
            if self.t_texobj:
                break
        # sanity: geometry, VB and IB templates look like what was mapped
        assert struct.unpack_from('>I', self.t_vb, 28)[0] == VP | (g0['vb_off'] + 0x20)
        assert struct.unpack_from('>I', self.t_ib, 12)[0] == VP | (g0['ib_off'] + 0x10)

    def _template_wheel(self):
        for c in self.tf['children']:
            if c['drawable'] and c['drawable']['lods']:
                return c['drawable']
        raise ValueError('template has no wheel child')

    def _donor_wheel(self):
        for c in self.df['children']:
            if c['drawable'] and c['drawable']['lods']:
                return c['drawable']
        raise ValueError('donor has no wheel child')

    # ---------------- skeleton
    def transplant_skeleton(self):
        t, d = self.t, self.d
        tsk = self.tf['drawable']['skeleton_off']
        dsk = self.df['drawable']['skeleton_off']
        tbo, tnames = bones(t, tsk)
        dbo, dnames = bones(d, dsk)
        dindex = {n: i for i, n in enumerate(dnames)}
        self.bone_map = {}
        for di, n in enumerate(dnames):
            tn = BONE_ALIAS.get(n, n)
            assert tn in tnames, 'donor bone %s has no template bone' % n
            self.bone_map[di] = tnames.index(tn)
        moved = 0
        for ti, n in enumerate(tnames):
            if n not in dindex:
                continue
            di = dindex[n]
            src = d.virt[dbo + 0xE0 * di + 0x30:dbo + 0xE0 * di + 0xE0]
            dst = tbo + 0xE0 * ti + 0x30
            self.v.buf[dst:dst + len(src)] = swap_words(src)
            for k in (8, 12, 16):
                ta, da = t.vptr(tsk + k), d.vptr(dsk + k)
                self.v.buf[ta + 64 * ti:ta + 64 * ti + 64] = swap_words(d.virt[da + 64 * di:da + 64 * di + 64])
            moved += 1
        self.log('skeleton: %d of %d template bones moved to the donor\'s pose; '
                 '%d donor bones mapped' % (moved, len(tnames), len(self.bone_map)))

    # ---------------- shaders
    def build_shader_group(self):
        t, d = self.t, self.d
        dsg = self.df['drawable']['shader_group']
        n = len(dsg['shaders'])
        new_shaders, usages, extras = [], [], []
        self.shader_report = []
        for s in dsg['shaders']:
            sps = shader_sps(d, s['off'])
            tsps = sps if sps in self.t_shaders else SPS_FALLBACK.get(sps)
            assert tsps in self.t_shaders, 'no template shader for %s' % sps
            ts = self.t_shaders[tsps]
            new_shaders.append(self._clone_shader(ts['off'], s['off']))
            usages.append(ts['usage'])
            extras.append(ts['extra'])
            self.shader_report.append((sps, tsps))
        v = self.v
        sg = v.put(self.t_sg)
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
        self.shader_usage = usages
        return sg

    def _clone_shader(self, ts, ds):
        """Clone template shader ts; fill parameters from donor shader ds by
        name hash. Parameter block laid out exactly as the template's:
        offsets | types (16-aligned) | names (16-aligned) | values (16-aligned)."""
        t, d, v = self.t, self.d, self.v
        tparams = shader_params(t, ts)
        dparams = {p['hash']: p for p in shader_params(d, ds)}
        so = v.put(t.virt[ts:ts + 0x60])
        # The parameter block (offsets | types | names | values, size at +0x20)
        # is replicated byte for byte with the same 64-byte phase: the 360
        # compiler's packing has gaps no simple rule explains (tire: zeroes
        # before its matrix), so every value keeps the template's position.
        tpo, tpt, tpn = t.vptr(ts + 0x14), t.vptr(ts + 0x24), t.vptr(ts + 0x34)
        size = t.u32(ts + 0x20)
        pad = (tpo - len(v.buf)) % 64
        v.buf += b'\0' * pad
        po = len(v.buf)
        v.buf += t.virt[tpo:tpo + size]
        assert po % 64 == tpo % 64
        pt, pn = po + (tpt - tpo), po + (tpn - tpo)
        for i, p in enumerate(tparams):
            if p['type'] == 0:
                continue
            assert p['value'] is not None and tpo <= p['value'] < tpo + size
            o = po + (p['value'] - tpo)
            vsize = 16 * p['type']  # the type byte is the vec4 count (1, 4 = matrix, 15 = array)
            dp = dparams.get(p['hash'])
            if dp and dp['type'] == p['type'] and dp['value'] is not None:
                v.buf[o:o + vsize] = swap_words(d.virt[dp['value']:dp['value'] + vsize])
            v.w32(po + 4 * i, VP | o)
        # texture parameters: objects outside the block, as in the template
        for i, p in enumerate(tparams):
            if p['type'] != 0:
                continue
            dp = dparams.get(p['hash'])
            name = None
            if dp is not None and dp['type'] == 0:
                if dp['value'] is not None:
                    np_ = d.vptr(dp['value'] + 20)
                    name = d.cstr(np_) if np_ is not None else None
                use = name is not None
            else:
                use = p['value'] is not None  # donor lacks it: keep the template's
            if not use:
                v.w32(po + 4 * i, 0)
                continue
            src = t.virt[p['value']:p['value'] + 32] if p['value'] is not None else self.t_texobj
            obj = v.put(src, 16)
            if name is not None:
                v.w32(obj + 20, VP | v.cstr(name))
                self.texture_names.add(name.lower())
            v.w32(po + 4 * i, VP | obj)
        v.w32(so + 0x14, VP | po)
        v.w32(so + 0x24, VP | pt)
        v.w32(so + 0x34, VP | pn)
        return so

    # ---------------- geometry
    def _decl(self, usage, src_decl_off):
        if usage in self.decls:
            return self.decls[usage]
        d = self.d
        _, stride = vertex_layout(usage, SIZE_360)
        assert struct.unpack_from('<Q', d.virt, src_decl_off + 8)[0] == PC_TYPES
        b = bytearray(self.t_decl)
        struct.pack_into('>I', b, 0, usage)
        b[4] = stride
        b[5] = 0
        b[6] = d.virt[src_decl_off + 6]
        b[7] = d.virt[src_decl_off + 7]
        struct.pack_into('>Q', b, 8, TYPES_360)
        off = self.v.put(bytes(b))
        self.decls[usage] = off
        return off

    def convert_geometry(self, g):
        """Donor geometry -> phys data (shared across LOD duplicates)."""
        d = self.d
        assert g['prim'] == 3, 'only triangle lists'
        usage = g['decl']['usage']
        vsrc = d.phys[g['vb_data'][1]:g['vb_data'][1] + g['vertex_count'] * g['vb_stride']]
        vdata, stride = convert_vertices(vsrc, usage, g['vertex_count'], g['vb_stride'])
        n = g['ib_count']
        isrc = d.phys[g['ib_data'][1]:g['ib_data'][1] + 2 * n]
        idata = struct.pack('>%dH' % n, *struct.unpack('<%dH' % n, isrc))
        assert max(struct.unpack('<%dH' % n, isrc)) < g['vertex_count']
        vo = self.p.put(vdata, 0x100)
        io = self.p.put(idata, 0x100)
        bp = d.vptr(g['off'] + 56)
        bc = d.u16(g['off'] + 62)
        tb = [self.bone_map[d.u16(bp + 2 * k)] for k in range(bc)] if bp is not None else []
        decl_src = d.vptr(g['vb_off'] + 16)
        return dict(usage=usage, stride=stride, vcount=g['vertex_count'], icount=n,
                    vo=vo, vbytes=len(vdata), io=io, ibytes=len(idata), bones=tb,
                    decl_src=decl_src)

    def emit_geometry(self, c):
        v = self.v
        decl = self._decl(c['usage'], c['decl_src'])
        # vertex buffer + its D3D header
        vb = v.put(self.t_vb)
        v.w16(vb + 4, c['vcount'])
        v.w32(vb + 8, PP | c['vo'])
        v.w32(vb + 12, c['stride'])
        v.w32(vb + 16, VP | decl)
        v.w32(vb + 24, PP | c['vo'])
        v.w32(vb + 28, VP | (vb + 0x20))
        f1 = struct.unpack_from('>I', self.t_vb, 0x3C)[0]
        assert c['vbytes'] % 4 == 0 and c['vbytes'] // 4 < (1 << 24)
        v.w32(vb + 0x38, (PP | c['vo']) | 3)
        v.w32(vb + 0x3C, (f1 & 0xFC000003) | ((c['vbytes'] // 4) << 2))
        # index buffer + its D3D header
        ib = v.put(self.t_ib)
        v.w32(ib + 4, c['icount'])
        v.w32(ib + 8, PP | c['io'])
        v.w32(ib + 12, VP | (ib + 0x10))
        v.w32(ib + 0x28, PP | c['io'])
        v.w32(ib + 0x2C, c['ibytes'])
        # bone table
        bt = 0
        if c['bones']:
            bt = v.alloc(2 * len(c['bones']), 16)
            for k, b in enumerate(c['bones']):
                v.w16(bt + 2 * k, b)
        geo = v.put(self.t_geom)
        v.w32(geo + 12, VP | vb)
        v.w32(geo + 28, VP | ib)
        v.w32(geo + 44, c['icount'])
        v.w32(geo + 48, c['icount'] // 3)
        v.w16(geo + 52, c['vcount'])
        v.w16(geo + 54, 3)
        v.w32(geo + 56, (VP | bt) if bt else 0)
        v.w16(geo + 60, c['stride'])
        v.w16(geo + 62, len(c['bones']))
        return geo

    def emit_lod(self, converted_models, model_template):
        """converted_models: list of (list of converted geometry, shader map)."""
        v = self.v
        models = []
        for geoms, smap in converted_models:
            gofs = [self.emit_geometry(c) for c in geoms]
            garr = v.alloc(4 * len(gofs))
            for k, g in enumerate(gofs):
                v.w32(garr + 4 * k, VP | g)
            sm = v.alloc(2 * len(smap), 16)
            for k, s in enumerate(smap):
                v.w16(sm + 2 * k, s)
            m = v.put(model_template)
            v.w32(m + 4, VP | garr)
            v.w16(m + 8, len(gofs))
            v.w16(m + 10, len(gofs))
            v.w32(m + 12, 0)
            v.w32(m + 16, VP | sm)
            v.w16(m + 26, len(gofs))
            models.append(m)
        marr = v.alloc(4 * len(models))
        for k, m in enumerate(models):
            v.w32(marr + 4 * k, VP | m)
        coll = v.put(self.t_coll)
        v.w32(coll, VP | marr)
        v.w16(coll + 4, len(models))
        v.w16(coll + 6, len(models))
        return coll

    def convert_drawable_lods(self, ddraw):
        out = []
        for coll in lod_collections(self.d, ddraw['off']):
            if coll is None:
                out.append(None)
                continue
            lod = []
            for m in model_collection(self.d, coll):
                gs = [g for g in rsc5.ptr_collection(self.d, m + 4) if g is not None]
                smap_p = self.d.vptr(m + 16)
                smap = [self.d.u16(smap_p + 2 * k) for k in range(len(gs))]
                lod.append(([self.convert_geometry(rsc5.read_geometry(self.d, g)) for g in gs], smap))
            out.append(lod)
        return out

    def check_shader_inputs(self, lods):
        for lod in lods:
            for geoms, smap in (lod or []):
                for c, si in zip(geoms, smap):
                    need = self.shader_usage[si]
                    assert need & ~c['usage'] == 0, \
                        'shader %d needs %04X, geometry has %04X' % (si, need, c['usage'])

    def copy_bounds(self, t_draw_off, d_draw_off):
        for k in (16, 32, 48):
            self.v.buf[t_draw_off + k:t_draw_off + k + 16] = swap_words(self.d.virt[d_draw_off + k:d_draw_off + k + 16])
        self.v.buf[t_draw_off + 112:t_draw_off + 116] = swap_words(self.d.virt[d_draw_off + 112:d_draw_off + 116])

    def run(self):
        t = self.t
        self.transplant_skeleton()
        sg = self.build_shader_group()
        tdraw = self.tf['drawable']['off']
        ddraw = self.df['drawable']
        self.v.w32(tdraw + 8, VP | sg)

        main = self.convert_drawable_lods(ddraw)
        wheel_d = self._donor_wheel()
        wheel = self.convert_drawable_lods(wheel_d)
        self.check_shader_inputs(main)
        self.check_shader_inputs(wheel)

        # main: template slots 0-2 used; donor has fewer -> repeat its last LOD
        tslots = [c is not None for c in lod_collections(t, tdraw)]
        dlods = [l for l in main if l is not None]
        for slot, used in enumerate(tslots):
            if not used:
                continue
            src = dlods[min(slot, len(dlods) - 1)]
            self.v.w32(tdraw + 64 + 4 * slot, VP | self.emit_lod(src, self.t_model_main))
        self.copy_bounds(tdraw, ddraw['off'])

        twheel = self._template_wheel()
        tw_slots = [c is not None for c in lod_collections(t, twheel['off'])]
        wlods = [l for l in wheel if l is not None]
        for slot, used in enumerate(tw_slots):
            if not used:
                continue
            src = wlods[min(slot, len(wlods) - 1)]
            self.v.w32(twheel['off'] + 64 + 4 * slot, VP | self.emit_lod(src, self.t_model_wheel))
        self.copy_bounds(twheel['off'], wheel_d['off'])

        # fragment bounding sphere (+16..+31), same layout on both (checked by caller)
        self.v.buf[16:32] = swap_words(self.d.virt[16:32])

        flags, vtot, ptot = encode_flags(t.flags, len(self.v.buf), len(self.p.buf))
        virt = bytes(self.v.buf) + b'\0' * (vtot - len(self.v.buf))
        phys = bytes(self.p.buf) + b'\0' * (ptot - len(self.p.buf))
        return flags, virt + phys


# ---------------------------------------------------------------- textures
def pc_textures(r, dict_off):
    out = {}
    tl = r.vptr(dict_off + 24)
    for i in range(r.u16(dict_off + 20)):
        ti = r.vptr(tl + 4 * i)
        name = r.cstr(r.vptr(ti + 20))
        bare = name.split('/')[-1]
        if bare.lower().endswith('.dds'):
            bare = bare[:-4]
        fmt = bytes(r.virt[ti + 32:ti + 36]).decode('latin-1')
        data = r.resolve(r.u32(ti + 72))
        out.setdefault(bare.lower(), dict(name=bare.lower(), w=r.u16(ti + 28), h=r.u16(ti + 30),
                                          fmt=fmt, levels=r.u8(ti + 39), res=r,
                                          data=data[1] if data else None))
    return out


def build_xtd(tmpl, sources, wanted, log=print):
    """Template 360 dictionary kept whole (its textures stay valid); donor
    textures appended; hash table and list rebuilt, sorted by hash."""
    t = tmpl
    v = Seg(t.virt[:align(max(o for o in range(t.vsize) if t.virt[o] not in (0, 0xCD)) + 1, 16)])
    p = Seg(t.phys)
    tl, n = t.vptr(24), t.u16(20)
    ht = t.vptr(16)
    entries = []  # (hash, info offset)
    have = set()
    t_info = t_d3d = None
    for i in range(n):
        info = t.vptr(tl + 4 * i)
        entries.append((t.u32(ht + 4 * i), info))
        nm = t.cstr(t.vptr(info + 20)).split('/')[-1].lower()
        have.add(nm[:-4] if nm.endswith('.dds') else nm)
        d3d = t.vptr(info + 24)
        if t.u32(info + 32) == 1:  # a single-level template to clone
            t_info, t_d3d = t.virt[info:info + 0x40], t.virt[d3d:d3d + 0x34]
    added = []
    for name in sorted(wanted):
        if name in have:
            continue
        src = next((s[name] for s in sources if name in s), None)
        if src is None:
            continue
        fmt, bpp = xenos.FORMATS[src['fmt']]
        w, h = src['w'], src['h']
        bw, bh = max(1, w // 4), max(1, h // 4)
        lin = src['res'].phys[src['data']:src['data'] + bw * bh * bpp]
        tiled = xenos.tile_level(xenos.swap16(lin), bw, bh, bpp)
        base = p.put(tiled, 0x1000)
        info = v.put(t_info)
        d3d = v.put(t_d3d)
        nm = v.cstr('pack:/%s.dds' % name)
        v.w32(info + 20, VP | nm)
        v.w32(info + 24, VP | d3d)
        v.w16(info + 28, w)
        v.w16(info + 30, h)
        v.w32(info + 32, 1)
        f = list(struct.unpack_from('>6I', t_d3d, 28))
        pitch = (align(bw, 32) * 4) >> 5
        f[0] = (f[0] & ~(0x1FF << 22)) | (pitch << 22)
        f[1] = fmt | (1 << 6) | (f[1] & 0xF00) | ((PP | base) & 0xFFFFF000)
        f[2] = (f[2] & ~0x03FFFFFF) | (w - 1) | ((h - 1) << 13)
        f[4] = f[4] & ~(0xFF << 2)                      # mip min = max = 0
        f[5] = (f[5] & ~0xFFFFF800) | (1 << 9) & 0x600  # 2D, no packed mips, no mip address
        struct.pack_into('>6I', v.buf, d3d + 28, *f)
        entries.append((joaat(name), info))
        added.append('%s %dx%d %s' % (name, w, h, src['fmt']))
        have.add(name)
    entries.sort()
    assert len({h for h, _ in entries}) == len(entries), 'hash collision'
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
    missing = sorted(set(wanted) - have)
    log('textures: %d added (%s); template kept %d; resolved elsewhere (vehicle shared pack): %s'
        % (len(added), ', '.join(added), n, ', '.join(missing)))
    flags, vtot, ptot = encode_flags(t.flags, len(v.buf), len(p.buf))
    return flags, bytes(v.buf) + b'\0' * (vtot - len(v.buf)) + bytes(p.buf) + b'\0' * (ptot - len(p.buf))
