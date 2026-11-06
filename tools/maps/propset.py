"""A props set (v1.7.47): PC pieces (vprop.py's, blade.py's: <name>.wdr,
<name>.wtd, <name>.ide) as a map set folder that places nothing - its
models go into vehicles.img and gtxd.ide like a place's, for the trainer
to make as objects (the Beyond suit's blade and Bat-Signal). map.txt says
"props yes": the installer takes it without pieces, the menu lists it
nowhere.

usage: python3 -I propset.py <set_dir> <set name> <label> <xlzx> <xdr template.bin:flags> <xtd template.bin:flags> <stock src dir> <pc piece> ...
  xdr template   a stock map drawable's decoded body and flags (mapset.py's --draw)
  xtd template   a stock texture dictionary with mips (maptex.Template)
  stock src dir  decoded stock drawables (.bin) whose shaders the pieces' may stand in for
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import mapdraw  # noqa: E402
import maptex  # noqa: E402
import rsc5  # noqa: E402

XDR_TYPE, XTD_TYPE = 109, 7


def pack(xlzx, rtype, flags, body, out):
    tmp = out + '.body'
    open(tmp, 'wb').write(body)
    subprocess.run([xlzx, 'rsc', str(rtype), '0x%08X' % flags, tmp, out], check=True, capture_output=True)
    os.remove(tmp)


def main():
    set_dir, name, label, xlzx, xdr_arg, xtd_arg, src_dir = sys.argv[1:8]
    pieces = sys.argv[8:]
    os.makedirs(set_dir, exist_ok=True)
    tp, tf = xdr_arg.rsplit(':', 1)
    tmpl = rsc5.Resource(open(tp, 'rb').read(), int(tf, 16), True, XDR_TYPE, tp)
    xp, xf = xtd_arg.rsplit(':', 1)
    xtd_tmpl = maptex.Template(rsc5.Resource(open(xp, 'rb').read(), int(xf, 16), True, XTD_TYPE, xp))
    sources = [mapdraw.load_console(p) for p in sorted(glob.glob(os.path.join(src_dir, '*.bin')))]
    for k, fb in mapdraw.MAP_FALLBACK.items():
        batconv.SPS_FALLBACK.setdefault(k, fb)
    lines = []
    for wdr in pieces:
        pname = os.path.basename(wdr)[:-4]
        donor = rsc5.Resource.pc(wdr)
        conv = mapdraw.MapDrawable(tmpl, donor, sources, log=lambda s, n=pname: print('[%s] %s' % (n, s)))
        flags, body = conv.run()
        verdict = mapdraw.check(body, flags, donor, added=conv.added, dropped=conv.dropped)
        pack(xlzx, XDR_TYPE, flags, body, os.path.join(set_dir, pname + '.xdr'))
        wtd = wdr[:-4] + '.wtd'
        r = rsc5.Resource.pc(wtd)
        textures = batconv.pc_textures(r, 0)
        tflags, tbody, report = maptex.build(xtd_tmpl, textures, log=lambda *a: None)
        tverdict, worst = maptex.check(tbody, tflags, textures, log=lambda *a: None)
        pack(xlzx, XTD_TYPE, tflags, tbody, os.path.join(set_dir, pname + '.xtd'))
        line = open(wdr[:-4] + '.ide').read().strip()
        lines.append(line)
        print('%s: %s; %d textures (%s)' % (pname, verdict, len(textures), tverdict))
    with open(os.path.join(set_dir, 'map.txt'), 'w') as f:
        f.write('name %s\nlabel %s\nprops yes\ndate 2039 1 1 0 0\nshift 0.0 0.0\nsea 0.00\n'
                'arrive 0.0 0.0 0.0 0.0 0\narea -9999.0 -9999.0 -9998.0 -9998.0\npieces 0\nprefix bk_\n' % (name, label))
    with open(os.path.join(set_dir, 'place.txt'), 'w') as f:
        f.write('# model qx qy qz qw x y z collision (a props set: nothing is placed)\n')
    with open(os.path.join(set_dir, 'objs.ide'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d props' % (set_dir, len(lines)))


if __name__ == '__main__':
    main()
