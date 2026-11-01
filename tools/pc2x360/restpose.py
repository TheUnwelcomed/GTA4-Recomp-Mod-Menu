"""Rest-pose check of a converted 360 car, read from the output file alone:
every vertex skinned the way the game does it (runtime matrix x inverse rest
pose) must land where it is stored, the main drawable may only use bones the
game never hides or poses from collision parts (plus the pivots and doors
named), and every physics part's child data must agree with its bone.

usage: restpose.py <name.xft> <name.xft.bin> [pivot or door bone[,bone...]]   (exit 1 on failure)"""
import struct
import sys
import rsc5
import batconv

def check_rest_pose(r, pivot=None):
    """(ok, message) for a 360 car resource."""
    f = rsc5.read_fragtype(r)
    d = f['drawable']
    sk = d['skeleton_off']
    bo, names = batconv.bones(r, sk)
    n = len(names)
    garr = r.vptr(sk + 8)


    def rows(off, k=4):
        return [struct.unpack_from('>3f', r.virt, off + 16 * i) for i in range(k)]


    bind = []
    for i in range(n):
        rot = rows(garr + 64 * i, 3)
        pos = struct.unpack_from('>3f', r.virt, bo + 0xE0 * i + 0x60)
        bind.append((rot, pos))
    fails = []
    insts = [r.vptr(h) for h in (0x1DC, 0x1E0) if r.vptr(h) is not None]
    runtime = []
    for inst in insts:
        glo = r.vptr(inst + 0x14)
        runtime.append([rows(glo + 64 * i) for i in range(n)])
    # physics parts: collision part matrices must sit on their bones
    lst = r.vptr(0xD4)
    bnd = r.vptr(0xEC)
    cur = r.vptr(bnd + 0x84)
    phys = {}
    for c in range(f['child_count']):
        co = r.vptr(lst + 4 * c)
        phys[r.u16(co + 0x0E)] = c
        part = rows(cur + 64 * c)
        g = runtime[0][r.u16(co + 0x0E)]
        if max(abs(a - b) for a, b in zip(part[3], g[3])) > 1e-3:
            fails.append('collision part %d (%s) at %s, bone at %s' % (c, names[r.u16(co + 0x0E)], part[3], g[3]))


    def skin(v, b, rt):
        """vertex -> inverse rest pose of bone b -> runtime pose."""
        rot, pos = bind[b]
        rel = tuple(v[k] - pos[k] for k in range(3))
        local = tuple(sum(rel[j] * rot[i][j] for j in range(3)) for i in range(3))  # x R^T
        g = rt[b]
        return tuple(local[0] * g[0][k] + local[1] * g[1][k] + local[2] * g[2][k] + g[3][k] for k in range(3))


    pivots = set(pivot) if isinstance(pivot, (set, list, tuple)) else {pivot}
    allowed = lambda nm: batconv.carries(nm) or nm in pivots  # noqa: E731
    worst, bad_bones, checked = 0.0, set(), 0
    for li, lod in enumerate(d['lods']):
        for m in lod:
            for g in m['geometries']:
                bp = r.vptr(g['off'] + 56)
                table = [r.u16(bp + 2 * k) for k in range(r.u16(g['off'] + 62))] if bp else []
                lay = dict(batconv.vertex_layout(g['decl']['usage'], batconv.SIZE_360)[0])
                stride = g['vb_stride']
                base = g['vb_data'][1]
                for v in range(g['vertex_count']):
                    b = base + v * stride
                    p = struct.unpack_from('>3f', r.phys, b + lay[0])
                    w = r.phys[b + lay[1]:b + lay[1] + 4][::-1] if 1 in lay else b'\xff\0\0\0'
                    ix = r.phys[b + lay[2]:b + lay[2] + 4][::-1] if 2 in lay else b'\0\0\0\0'
                    for rt in runtime:
                        acc = [0.0, 0.0, 0.0]
                        tot = 0
                        for k in range(4):
                            if not w[k]:
                                continue
                            bone = table[ix[k]]
                            if not allowed(names[bone]):
                                bad_bones.add(names[bone])
                            q = skin(p, bone, rt)
                            for j in range(3):
                                acc[j] += w[k] * q[j]
                            tot += w[k]
                        acc = [a / tot for a in acc]
                        worst = max(worst, max(abs(a - c) for a, c in zip(acc, p)))
                    checked += 1
    msg = ('%s: %d vertices skinned at rest, worst displacement %.5f m; bones used outside the safe set: %s; %s'
          % (getattr(r, 'name', 'car').split('/')[-1], checked, worst, sorted(bad_bones) or 'none', '; '.join(fails) or 'collision parts on their bones'))
    return worst < 1e-3 and not bad_bones and not fails, msg


def check_children(r):
    """(ok, message): every physics part's fragment child data agree with its
    bone and collision part, as on every stock car: child +0x10 the inverse
    of the bone's rest matrix, child +0x50 its link's centre of gravity (the
    body's unless a joint bone - a door, bonnet, boot - carries the part),
    fragType +0xF8 the part's offset from that centre."""
    f = rsc5.read_fragtype(r)
    _, bones = batconv.read_skeleton(r, f['drawable']['skeleton_off'])
    lst, att = r.vptr(0xD4), r.vptr(0xF8)
    cur = r.vptr(r.vptr(0xEC) + 0x84)
    root_cg = batconv.rows(r, r.vptr(lst) + 0x50)[3]
    bad = []
    for c in range(f['child_count']):
        co = r.vptr(lst + 4 * c)
        b = r.u16(co + 0x0E)
        name, pos = bones[b]['name'], bones[b]['pos']
        inv, cg = batconv.rows(r, co + 0x10), batconv.rows(r, co + 0x50)
        off, part = batconv.rows(r, att + 64 * c), batconv.rows(r, cur + 64 * c)
        link = b
        while link >= 0 and not batconv.is_joint(batconv.bone_limits(r, bones[link]['rec'])):
            link = bones[link]['parent']
        if not (batconv.close(inv[3], tuple(-v for v in batconv.v_mat(pos, inv[:3]))) and
                all(batconv.close(inv[k], batconv.m_t(bones[b]['G'])[k]) for k in range(3))):
            bad.append('%s: +0x10 is not the inverse of its rest matrix' % name)
        if batconv.close(cg[3], root_cg) != (link <= 0):
            bad.append('%s: link centre %s' % (name, tuple(round(v, 3) for v in cg[3])))
        if not batconv.close(off[3], batconv.v_mat(batconv.v_sub(part[3], cg[3]), batconv.m_t(cg[:3]))):
            bad.append('%s: offset from its link centre is stale' % name)
    return not bad, '%d physics parts%s' % (f['child_count'], (': ' + '; '.join(bad)) if bad else ', all consistent')


if __name__ == '__main__':
    _r = rsc5.Resource.x360(sys.argv[1], sys.argv[2])
    _ok, _msg = check_rest_pose(_r, set(sys.argv[3].split(',')) if len(sys.argv) > 3 else None)
    _ok2, _msg2 = check_children(_r)
    print(_msg)
    print(_msg2)
    sys.exit(0 if _ok and _ok2 else 1)
