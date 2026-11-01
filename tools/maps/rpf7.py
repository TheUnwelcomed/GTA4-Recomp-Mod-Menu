"""List, check and extract a GTA V RPF7 archive (unencrypted, "OPEN"), with
the RPFs nested in it.

usage: python3 -I rpf7.py <archive.rpf> [<out_dir>]

Every entry is checked: a resource (RSC7) must inflate to exactly the size its
page flags give, a compressed file to its stated size, a nested RPF must parse.
With out_dir, files are written under it (resources keep their 16-byte RSC7
header, then the inflated data: the layout the RSC7 tools expect when they
read "<name>.<ext>" from disk is the compressed one, so the inflated copy is
written beside it as <name>.<ext>.raw).
"""
import os
import struct
import sys
import zlib

MAGIC = 0x52504637
OPEN = 0x4E45504F


def page_size(flags):
    s = (((flags >> 27) & 0x1) << 0) + (((flags >> 26) & 0x1) << 1) + (((flags >> 25) & 0x1) << 2) \
        + (((flags >> 24) & 0x1) << 3) + (((flags >> 17) & 0x7F) << 4) + (((flags >> 11) & 0x3F) << 5) \
        + (((flags >> 7) & 0xF) << 6) + (((flags >> 5) & 0x3) << 7) + (((flags >> 4) & 0x1) << 8)
    return (0x200 << (flags & 0xF)) * s


def inflate(data):
    d = zlib.decompressobj(-15)
    out = d.decompress(data)
    out += d.flush()
    return out, d.eof


class Problem(Exception):
    pass


def walk(blob, base, prefix, out, stats, depth=0):
    magic, count, names_len, enc = struct.unpack_from('<IIII', blob, base)
    if magic != MAGIC:
        raise Problem('%s: not RPF7 (%08x)' % (prefix or '/', magic))
    if enc != OPEN:
        raise Problem('%s: encrypted (%08x), not handled' % (prefix or '/', enc))
    toc = base + 16
    names = blob[toc + count * 16: toc + count * 16 + names_len]

    def name_at(off):
        end = names.index(b'\0', off)
        return names[off:end].decode('utf-8', 'replace')

    entries = []
    for i in range(count):
        e = blob[toc + i * 16: toc + i * 16 + 16]
        y, x = struct.unpack_from('<II', e, 0)
        if x == 0x7FFFFF00:
            off, _, first, n = struct.unpack_from('<IIII', e, 0)
            entries.append(('dir', name_at(off), first, n))
        elif x & 0x80000000 == 0:
            buf, usize, encflag = struct.unpack_from('<QII', e, 0)
            entries.append(('bin', name_at(buf & 0xFFFF), (buf >> 16) & 0xFFFFFF, (buf >> 40) & 0xFFFFFF,
                            usize, encflag))
        else:
            noff = struct.unpack_from('<H', e, 0)[0]
            fsize = e[2] | e[3] << 8 | e[4] << 16
            foff = (e[5] | e[6] << 8 | e[7] << 16) & 0x7FFFFF
            sysf, gfxf = struct.unpack_from('<II', e, 8)
            entries.append(('res', name_at(noff), fsize, foff, sysf, gfxf))

    def visit(index, path):
        kind = entries[index][0]
        if kind == 'dir':
            _, name, first, n = entries[index]
            for j in range(first, first + n):
                child = entries[j][1]
                visit(j, path + '/' + child if path else child)
            return
        full = prefix + '/' + path if prefix else path
        if kind == 'bin':
            _, name, csize, foff, usize, encflag = entries[index]
            start = base + foff * 512
            if encflag:
                stats['problems'].append('%s: encrypted file entry' % full)
                return
            if csize:
                data, ok = inflate(blob[start:start + csize])
                if not ok or len(data) != usize:
                    stats['problems'].append('%s: inflated %d of %d (eof %s)' % (full, len(data), usize, ok))
            else:
                data = blob[start:start + usize]
            if data[:4] == b'7FPR':
                stats['files'].append((full, 'rpf', len(data), ''))
                try:
                    walk(data, 0, full, out, stats, depth + 1)
                except Problem as p:
                    stats['problems'].append(str(p))
                return
            stats['files'].append((full, 'bin', len(data), ''))
            if out:
                dest = os.path.join(out, *full.split('/'))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                open(dest, 'wb').write(data)
            return
        _, name, fsize, foff, sysf, gfxf = entries[index]
        start = base + foff * 512
        if fsize == 0xFFFFFF:  # the real size is in the stored header
            h = blob[start:start + 16]
            fsize = (h[7] << 0) | (h[14] << 8) | (h[5] << 16) | (h[2] << 24)
        stored = blob[start:start + fsize]
        if stored[:4] != b'RSC7':
            stats['problems'].append('%s: no RSC7 header' % full)
            return
        version = ((sysf >> 28) & 0xF) << 4 | ((gfxf >> 28) & 0xF)
        want = page_size(sysf) + page_size(gfxf)
        data, ok = inflate(stored[16:])
        if not ok or len(data) != want:
            stats['problems'].append('%s: inflated %d of %d (eof %s)' % (full, len(data), want, ok))
        stats['files'].append((full, 'res', len(data), 'v%d sys %d gfx %d' % (version, page_size(sysf),
                                                                         page_size(gfxf))))
        if out:
            dest = os.path.join(out, *full.split('/'))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, 'wb').write(stored)
            open(dest + '.raw', 'wb').write(stored[:16] + data)

    visit(0, '')


def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    blob = open(path, 'rb').read()
    stats = {'files': [], 'problems': []}
    walk(blob, 0, '', out, stats)
    exts = {}
    for full, kind, size, info in stats['files']:
        ext = os.path.splitext(full)[1].lower() or '(none)'
        c = exts.setdefault(ext, [0, 0])
        c[0] += 1
        c[1] += size
    for full, kind, size, info in stats['files']:
        print('%-4s %10d  %s  %s' % (kind, size, full, info))
    print()
    for ext, (n, size) in sorted(exts.items(), key=lambda kv: -kv[1][1]):
        print('%-8s %5d files %12d bytes' % (ext, n, size))
    print('problems: %d' % len(stats['problems']))
    for p in stats['problems'][:40]:
        print('  ' + p)


if __name__ == '__main__':
    main()
