"""Print the bones each geometry of a GTA IV car is skinned to (its bone
table), per drawable and LOD, and the shader it is drawn with.

usage: palettes.py <car.wft>                      (PC)
       palettes.py <car.xft> <car.xft.bin>        (360: header + decoded body)

Handy for checking what a converted car's parts ride on: v1.7.5 and later
keep a mod's geometry on the chassis, the body shell, the wheel assembly and
a pivot, nothing the game hides or poses from collision parts.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pc2x360'))
import batconv  # noqa: E402
import rsc5  # noqa: E402


def main():
    args = sys.argv[1:]
    r = rsc5.Resource.pc(args[0]) if args[0].lower().endswith('.wft') else rsc5.Resource.x360(args[0], args[1])
    f = rsc5.read_fragtype(r)
    d = f['drawable']
    _, names = batconv.bones(r, d['skeleton_off'])
    sps = [batconv.shader_sps(r, s['off']) for s in d['shader_group']['shaders']]
    draws = [('main', d)] + [('child%d' % i, c['drawable']) for i, c in enumerate(f['children'])
                             if c['drawable'] and c['drawable']['lods']]
    biggest = 0
    for label, draw in draws:
        for li, lod in enumerate(draw['lods']):
            for mi, m in enumerate(lod):
                smap = r.vptr(m['off'] + 16)
                for gi, g in enumerate(m['geometries']):
                    bp, bc = r.vptr(g['off'] + 56), r.u16(g['off'] + 62)
                    table = [r.u16(bp + 2 * k) for k in range(bc)] if bp is not None else []
                    biggest = max(biggest, bc)
                    shader = sps[r.u16(smap + 2 * gi)] if label == 'main' and smap is not None else '-'
                    print('%s lod%d m%d g%d: %5d verts  %-30s %s'
                          % (label, li, mi, gi, g['vertex_count'] or g.get('vb_count', 0), shader,
                             [names[b] for b in table]))
    print('largest bone table: %d; skeleton %d bones' % (biggest, len(names)))


if __name__ == '__main__':
    main()
