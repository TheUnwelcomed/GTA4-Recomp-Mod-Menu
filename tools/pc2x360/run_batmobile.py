"""Convert the Batmobile (PC) into the stock 360 FBI's slot.

usage: run_batmobile.py <export_dir> <oiv_content_car_dir> <work_dir> <out_dir>
"""
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batconv  # noqa: E402
import rsc5  # noqa: E402

export, car, work, out = sys.argv[1:5]
os.makedirs(out, exist_ok=True)
t0 = time.time()

tx = rsc5.Resource.x360(os.path.join(export, 'fbi.xft'), os.path.join(work, 'fbi.xft.bin'))
tt = rsc5.Resource.x360(os.path.join(export, 'fbi.xtd'), os.path.join(work, 'fbi.xtd.bin'))
dft = rsc5.Resource.pc(os.path.join(car, 'fbi.wft'))
raw = open(os.path.join(car, 'fbi.wtd'), 'rb').read()
_, rt, rf = struct.unpack_from('<III', raw, 0)
dtd = rsc5.Resource(zlib.decompress(raw[12:]), rf, False, rt, 'fbi.wtd')

conv = batconv.Converter(tx, dft)
xft_flags, xft_body = conv.run()
for sps, tsps in conv.shader_report:
    if sps != tsps:
        print('shader: %s -> 360 %s' % (sps, tsps))

embedded = rsc5.read_fragtype(dft)['drawable']['shader_group']['texdict']
sources = [batconv.pc_textures(dtd, 0)]
if embedded and embedded[0] == 'v':
    sources.append(batconv.pc_textures(dft, embedded[1]))
xtd_flags, xtd_body = batconv.build_xtd(tt, sources, conv.texture_names)

open(os.path.join(out, 'fbi.xft.bin'), 'wb').write(xft_body)
open(os.path.join(out, 'fbi.xtd.bin'), 'wb').write(xtd_body)
open(os.path.join(out, 'flags.txt'), 'w').write('fbi.xft 112 0x%08X\nfbi.xtd 7 0x%08X\n'
                                                % (xft_flags, xtd_flags))
v, p = rsc5.seg_sizes(xft_flags)
print('fbi.xft: flags %08X virt %d phys %d (template virt %d phys %d)'
      % (xft_flags, v, p, tx.vsize, tx.psize))
v, p = rsc5.seg_sizes(xtd_flags)
print('fbi.xtd: flags %08X virt %d phys %d (template virt %d phys %d)'
      % (xtd_flags, v, p, tt.vsize, tt.psize))
print('done in %.1fs' % (time.time() - t0))
