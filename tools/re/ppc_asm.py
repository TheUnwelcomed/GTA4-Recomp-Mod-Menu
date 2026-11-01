"""Print a game function's PowerPC assembly from the recompiled sources.

The recompiler keeps each original instruction as a comment above its C++
(`// lwz r11,196(r11)`), so the generated gta4_recomp.*.cpp files double as a
disassembly of the 360 executable. This prints one function's instructions
with their addresses, which is how the v1.7.x add-on work found, among other
things, the vehicle pool, the extras mask and the per-bone matrix accessor.

usage: ppc_asm.py <generated dir> <function address, hex> [max lines]
       ppc_asm.py <generated dir> --find '<instruction regex>'   (which functions use it)

The first run indexes the functions (a few seconds); the index is kept in
~/.cache/gta4-ppc-asm/.
"""
import glob
import hashlib
import os
import re
import sys


def index_path(gen):
    key = hashlib.sha1(os.path.abspath(gen).encode()).hexdigest()[:12]
    d = os.path.join(os.path.expanduser('~'), '.cache', 'gta4-ppc-asm')
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, key + '.idx')


def load_index(gen):
    path = index_path(gen)
    if not os.path.exists(path):
        with open(path, 'w') as out:
            for f in sorted(glob.glob(os.path.join(gen, 'gta4_recomp.*.cpp'))):
                with open(f, errors='replace') as fh:
                    for n, line in enumerate(fh):
                        if line.startswith('DEFINE_REX_FUNC(sub_'):
                            out.write('%s %s %d\n' % (line[20:28], os.path.basename(f), n))
    index = {}
    for line in open(path):
        a, f, n = line.split()
        index[a] = (f, int(n))
    return index


def disassemble(gen, index, addr, limit):
    f, n = index[addr]
    pc = int(addr, 16)
    out = []
    with open(os.path.join(gen, f), errors='replace') as fh:
        lines = fh.read().split('\n')
    for line in lines[n + 1:]:
        if line.startswith('DEFINE_REX_FUNC('):
            break
        s = line.strip()
        if s.startswith('// ') and not s.startswith('// ---'):
            out.append('%08X  %s' % (pc, s[3:]))
            pc += 4
        elif re.match(r'^loc_[0-9A-F]+:', s):
            pc = int(s[4:12], 16)
            out.append('loc_%08X:' % pc)
        if len(out) >= limit:
            break
    return out


def main():
    gen = sys.argv[1]
    index = load_index(gen)
    if sys.argv[2] == '--find':
        pattern = re.compile(sys.argv[3])
        hits = {}
        for f in sorted(glob.glob(os.path.join(gen, 'gta4_recomp.*.cpp'))):
            current = None
            with open(f, errors='replace') as fh:
                for line in fh:
                    if line.startswith('DEFINE_REX_FUNC(sub_'):
                        current = line[20:28]
                    elif current and line.lstrip().startswith('// ') and pattern.search(line):
                        hits[current] = hits.get(current, 0) + 1
        for a, k in sorted(hits.items()):
            print('sub_%s  %d' % (a, k))
        return
    addr = sys.argv[2].upper().replace('0X', '').replace('SUB_', '')
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    print('\n'.join(disassemble(gen, index, addr, limit)))


if __name__ == '__main__':
    main()
