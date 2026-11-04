"""The Snow Mod's global texture dictionaries for the console (v1.7.45).

GTA 4 Snow Mod v2.0 (jumbo8) replaces the game's textures with snow-covered
ones; its gtxd.img carries the global parent dictionaries (the roads,
pavements, highways, freeways, rooftops, brownstones, LODs and the tree LOD
sheet of the whole city), 19 PC .wtd files. Each one the console's own
gtxd.img has (its table of contents, exported by the trainer: 17) is built
again as an .xtd with mips (maptex.build, the map sets' way: level 0 no
bigger than 512, the game's own size), its texture names kept, so the
installer can put it in place of the game's in a shadow gtxd.img.

usage: python3 -I snowtex.py <mod gtxd.img> <aes_key.bin> <console gtxd toc.txt> <template.xtd.bin:flags> <xlzx> <out_dir>
  console gtxd toc.txt  the trainer's export (export/index/toc/game_xbox360_data_cdimages_gtxd.img.txt)
  template              a stock .xtd's decoded body and flags (a texture with mips in it)
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, os.path.join(HERE, '..', 'maps'))
import batconv  # noqa: E402
import img3  # noqa: E402
import maptex  # noqa: E402
import rsc5  # noqa: E402

XTD_TYPE = 7
SIDE = 512


def console_names(toc_path):
    names = []
    for line in open(toc_path):
        if line.startswith('#') or not line.strip():
            continue
        name = line.split()[0]
        if name.lower().endswith('.xtd'):
            names.append(name[:-4].lower())
    return names


def main():
    img_path, key_path, toc_path, tmpl_arg, xlzx, out = sys.argv[1:7]
    os.makedirs(out, exist_ok=True)
    key = open(key_path, 'rb').read()
    wanted = console_names(toc_path)
    tmpl_path, tmpl_flags = tmpl_arg.rsplit(':', 1)
    tmpl = maptex.Template(rsc5.Resource(open(tmpl_path, 'rb').read(), int(tmpl_flags, 16), True, XTD_TYPE, tmpl_path))
    with tempfile.TemporaryDirectory() as work:
        entries = img3.extract(img_path, key, work)
        have = {os.path.basename(e)[:-4].lower(): e for e in entries if e.lower().endswith('.wtd')}
        total = 0
        for name in wanted:
            if name not in have:
                print('%s: not in the mod; the game\'s own stays' % name)
                continue
            r = rsc5.Resource.pc(have[name])
            textures = batconv.pc_textures(r, 0)
            flags, body, report = maptex.build(tmpl, textures, side=SIDE, detail=maptex.DETAIL_SIDE)
            verdict, worst = maptex.check(body, flags, textures)
            assert worst < 32.0, (name, worst)
            with tempfile.NamedTemporaryFile(delete=False, dir=work) as tmp:
                tmp.write(body)
            subprocess.run([xlzx, 'rsc', str(XTD_TYPE), '0x%08X' % flags, tmp.name, os.path.join(out, name + '.xtd')],
                           check=True, capture_output=True)
            size = os.path.getsize(os.path.join(out, name + '.xtd'))
            total += size
            print('%s.xtd: %d textures, %.2f MiB (body %.2f MiB), flags %08X, %s' % (
                name, len(textures), size / 1048576.0, len(body) / 1048576.0, flags, verdict))
        extra = sorted(set(have) - set(wanted))
        if extra:
            print('in the mod but not the console\'s gtxd.img (left out): %s' % ', '.join(extra))
        print('%d dictionaries, %.1f MiB' % (len(wanted), total / 1048576.0))


if __name__ == '__main__':
    main()
