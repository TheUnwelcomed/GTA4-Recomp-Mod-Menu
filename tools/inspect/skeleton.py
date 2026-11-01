"""Print a GTA IV car's skeleton: every bone with its parent, local
translation, stored position and rotation, and (360 files) which bones the
fragment's physics parts drive. Name bones to also print their three matrices.

usage: skeleton.py <car.wft>                      (PC)
       skeleton.py <car.xft> <car.xft.bin>        (360: header + decoded body)
       ... [bone names]

A bone whose stored position lies far outside the car (the Batmobile keeps
door bones 13 m below) is a parking spot, not a part's pivot.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pc2x360'))
import rsc5  # noqa: E402


def load(args):
    if args and args[0].lower().endswith('.wft'):
        return rsc5.Resource.pc(args[0]), args[1:]
    return rsc5.Resource.x360(args[0], args[1]), args[2:]


def fmt(v):
    return '(' + ', '.join('%7.3f' % x for x in v) + ')'


def main():
    r, only = load(sys.argv[1:])
    e = r.e
    f = rsc5.read_fragtype(r)
    sk = f['drawable']['skeleton_off']
    bo, n = r.vptr(sk), r.u16(sk + 20)
    arrays = [r.vptr(sk + k) for k in (8, 12, 16)]
    names = [r.cstr(r.vptr(bo + 0xE0 * i)) for i in range(n)]
    phys = {}
    lst = r.vptr(0xD4)
    for i in range(f['child_count']):
        c = r.vptr(lst + 4 * i)
        if c is not None:
            phys[r.u16(c + 0x0E)] = i
    print('%d bones; physics parts on: %s' % (n, ', '.join(names[b] for b in sorted(phys))))
    for i, name in enumerate(names):
        if only and name not in only:
            continue
        b = bo + 0xE0 * i
        par = r.u32(b + 0x10)
        parent = names[((par & 0x0FFFFFFF) - bo) // 0xE0] if par else '-'
        t = struct.unpack_from(e + '3f', r.virt, b + 0x20)
        q = struct.unpack_from(e + '4f', r.virt, b + 0x40)
        p = struct.unpack_from(e + '3f', r.virt, b + 0x60)
        print('[%2d] %-18s parent %-16s local %s pos %s quat %s%s'
              % (i, name, parent, fmt(t), fmt(p), fmt(q), '  [physics part %d]' % phys[i] if i in phys else ''))
        if only:
            for label, a in zip(('global rot', 'inverse', 'local'), arrays):
                m = struct.unpack_from(e + '16f', r.virt, a + 64 * i)
                print('      %-10s' % label, ' '.join(fmt(m[4 * j:4 * j + 3]) for j in range(4)))


if __name__ == '__main__':
    main()
