"""Convert a PC GTA IV car (.wft + .wtd) into a 360 add-on vehicle, using a
stock 360 car from your console as the template (physics, collision, the
fragment layout).

usage: run_vehicle.py <export_dir> <work_dir> <pc.wft> <pc.wtd> <out_dir> <name> [template] [pivots] [sources]

  export_dir  the stock 360 files exported by the trainer build (logs/export)
  work_dir    their decompressed bodies (xlzx unrsc <car>.xft work/<car>.xft.bin)
  name        the add-on's model name: writes <name>.xft.bin / <name>.xtd.bin and
              flags.txt (the RSC flags for xlzx rsc)
  template    the stock car to build on (default: fbi)
  pivots      optional ("-": none): donor geometry a template bone carries and the
              trainer turns (batconv.parse_pivots), e.g. the Deimos's pop-ups
              "extra_1=misc_a,misc_y,misc_headlight_l", or the F-16's gear
              "extra_1@0,2.73,-0.22=boot:y>1.5;extra_6@0,-0.073,-0.028=boot:y<=1.5,door_pside_f"
  sources     optional ("-": none): other stock cars (decoded in work_dir) to take
              a shader from that the template lacks, e.g. "airtug" for the F-16's
              livery paints. Without them a missing shader falls back to the
              template's nearest (paint3 -> paint1, interior -> interior2), which
              is what the game colours like its own cars.
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


def arg(i, default=''):
    v = sys.argv[i] if len(sys.argv) > i else default
    return '' if v == '-' else v


def main():
    export, work, wft, wtd, out, name = sys.argv[1:7]
    template = arg(7) or 'fbi'
    pivots = batconv.parse_pivots(arg(8))
    source_names = [s for s in arg(9).split(',') if s]
    os.makedirs(out, exist_ok=True)
    t0 = time.time()

    tx = rsc5.Resource.x360(os.path.join(export, template + '.xft'),
                            os.path.join(work, template + '.xft.bin'))
    tt = rsc5.Resource.x360(os.path.join(export, template + '.xtd'),
                            os.path.join(work, template + '.xtd.bin'))
    sources = [rsc5.Resource.x360(os.path.join(export, s + '.xft'), os.path.join(work, s + '.xft.bin'))
               for s in source_names if s != template]
    dft = rsc5.Resource.pc(wft)
    dtd = load_pc_wtd(wtd)

    conv = batconv.Converter(tx, dft, pivots=pivots, shader_sources=sources)
    xft_flags, xft_body = conv.run()
    for sps, tsps in conv.shader_report:
        if sps != tsps:
            print('shader: %s -> 360 %s' % (sps, tsps))
    if getattr(conv, 'split_moved', 0):
        print('pivots: %d vertex influences re-bound by position' % conv.split_moved)

    embedded = rsc5.read_fragtype(dft)['drawable']['shader_group']['texdict']
    tex_sources = [batconv.pc_textures(dtd, 0)]
    if embedded and embedded[0] == 'v':
        tex_sources.append(batconv.pc_textures(dft, embedded[1]))
    xtd_flags, xtd_body = batconv.build_xtd(tt, tex_sources, conv.texture_names)

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
