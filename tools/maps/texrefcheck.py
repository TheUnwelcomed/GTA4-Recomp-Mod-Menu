"""The texture objects a console drawable's shaders point to, by class id.

A 360 resource's objects carry a class id where their vtable will go, and
the game sets the vtable as it places each object by type. A shader's
texture parameter should point to a reference by name (id 2C926300: +8
0x02000001, the name at +20); a drawable that embeds its own textures (a
lamp's glow, a gate's reflection map) points at a whole grcTextureXenon
(id 941A6300, a D3D header at +24) which the game places through the
drawable's texture dictionary instead, and a fragment's references have an
id of their own (E4916300). v1.7.38-40 cloned 32 bytes of an embedded
texture as a reference: an object the game never placed - a null call
binding it (Wake Island, logs 106-107), a fault placing it (the Keys, log
108). batconv._clone_shader writes references only since v1.7.41; this
checks a folder of converted drawables (mapdraw.py's <model>.xdr.bin with
its flags.txt) or decompressed console files (<name>.xdr.bin / .xft.bin
with <name>.log from xlzx unrsc) and reports every id seen, and the
shaders whose texture objects are not references.

usage: python3 -I texrefcheck.py <dir> [...]
"""
import collections
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pc2x360'))
sys.path.insert(0, HERE)
import batconv  # noqa: E402
import mapdraw  # noqa: E402
import rsc5  # noqa: E402

NAMES = {batconv.TEXTURE_REFERENCE_ID: 'reference', batconv.TEXTURE_EMBEDDED_ID: 'embedded texture',
         0xE4916300: "a fragment's reference"}


def resources(folder):
    """(name, resource) for each drawable in the folder, either way it is kept."""
    flags = {}
    flags_path = os.path.join(folder, 'flags.txt')
    if os.path.exists(flags_path):
        for line in open(flags_path):
            f = line.split()
            if len(f) == 3:
                flags[f[0]] = int(f[2], 16)
    for path in sorted(glob.glob(os.path.join(folder, '*.bin'))):
        name = os.path.basename(path)[:-4]
        if os.path.exists(path[:-4] + '.log'):
            yield name, mapdraw.load_console(path)
        elif name in flags:
            yield name, rsc5.Resource(open(path, 'rb').read(), flags[name], True, 109, name)


def main():
    for folder in sys.argv[1:]:
        ids = collections.Counter()
        files = 0
        bad = []
        for name, res in resources(folder):
            files += 1
            for rd in mapdraw.drawables_of(res):
                if not rd or not rd['shader_group']:
                    continue
                for s in rd['shader_group']['shaders']:
                    for p in batconv.shader_params(res, s['off']):
                        if p['type'] != 0 or p['value'] is None:
                            continue
                        w = res.u32(p['value'])
                        ids[w] += 1
                        if w != batconv.TEXTURE_REFERENCE_ID:
                            np_ = res.vptr(p['value'] + 20)
                            bad.append('%s %s %08X (%s) %s' % (name, batconv.shader_sps(res, s['off']), w,
                                                              NAMES.get(w, '?'), res.cstr(np_) if np_ is not None else ''))
        print('%s: %d files; texture objects: %s' % (
            folder, files, ', '.join('%d %s (%08X)' % (n, NAMES.get(k, 'unknown class'), k) for k, n in ids.most_common())))
        for line in bad[:40]:
            print('   ' + line)
        if len(bad) > 40:
            print('   ... %d more' % (len(bad) - 40))


if __name__ == '__main__':
    main()
