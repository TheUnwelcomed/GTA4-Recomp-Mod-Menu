"""Convert a PC GTA IV car (.wft + .wtd) into a 360 add-on vehicle, using a
stock 360 car from your console as the template (physics, collision, the
fragment layout).

usage: run_vehicle.py <export_dir> <work_dir> <pc.wft> <pc.wtd> <out_dir> <name> [template]

  export_dir  the stock 360 files exported by the trainer build (logs/export)
  work_dir    their decompressed bodies (xlzx unrsc <template>.xft work/<template>.xft.bin)
  name        the add-on's model name: writes <name>.xft.bin / <name>.xtd.bin and
              flags.txt (the RSC flags for xlzx rsc)
  template    the stock car to build on (default: fbi)
"""
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batconv  # noqa: E402
import rsc5  # noqa: E402


def load_pc_wtd(path):
    raw = open(path, 'rb').read()
    _, rt, rf = struct.unpack_from('<III', raw, 0)
    return rsc5.Resource(zlib.decompress(raw[12:]), rf, False, rt, os.path.basename(path))


def main():
    export, work, wft, wtd, out, name = sys.argv[1:7]
    template = sys.argv[7] if len(sys.argv) > 7 else 'fbi'
    os.makedirs(out, exist_ok=True)
    t0 = time.time()

    tx = rsc5.Resource.x360(os.path.join(export, template + '.xft'),
                            os.path.join(work, template + '.xft.bin'))
    tt = rsc5.Resource.x360(os.path.join(export, template + '.xtd'),
                            os.path.join(work, template + '.xtd.bin'))
    dft = rsc5.Resource.pc(wft)
    dtd = load_pc_wtd(wtd)

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

    open(os.path.join(out, name + '.xft.bin'), 'wb').write(xft_body)
    open(os.path.join(out, name + '.xtd.bin'), 'wb').write(xtd_body)
    open(os.path.join(out, 'flags.txt'), 'w').write('%s.xft 112 0x%08X\n%s.xtd 7 0x%08X\n'
                                                    % (name, xft_flags, name, xtd_flags))
    for ext, flags, res in (('xft', xft_flags, tx), ('xtd', xtd_flags, tt)):
        v, p = rsc5.seg_sizes(flags)
        print('%s.%s: flags %08X virt %d phys %d (template virt %d phys %d)'
              % (name, ext, flags, v, p, res.vsize, res.psize))
    print('done in %.1fs' % (time.time() - t0))


if __name__ == '__main__':
    main()
