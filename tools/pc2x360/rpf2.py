"""List, extract and rebuild a GTA IV RPF2 archive (the console's
playerped.rpf, common.rpf, xbox360.rpf), encrypted or not (v1.7.46).

usage: rpf2.py <archive.rpf> <aes_key.bin> list
       rpf2.py <archive.rpf> <aes_key.bin> extract <out_dir>
       rpf2.py <archive.rpf> <aes_key.bin> add <out.rpf> <file> [<file> ...]
           the archive written again with the files added to its root
           directory (a file whose name is already there replaces it)

Header (20 bytes, little-endian): 'RPF2', table size, entry count, 0,
encrypted. The table at 0x800: 16 bytes per entry, then the names (each
0-terminated); AES-256-ECB, 16 passes, when encrypted. A directory entry:
name offset, 0, first child | 0x80000000, child count. A file entry: name
offset, size, offset (a resource: offset | its type in the low byte), and
the size in the archive | 0x40000000 when deflated - or, for a resource,
its RSC flags word (whose top two bits are set). A resource is stored as
the file the console reads: a 12-byte little-endian RSC header, then the
LZX body. Directories list their children in name order (lower case), the
order the game searches them in. Data starts on the 2048-byte block after
the table, each file on a block of its own.
"""
import os
import struct
import sys
import zlib

MAGIC = 0x32465052
BLOCK = 0x800
TOC_AT = 0x800


def aes(data, key, encrypt):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    whole = len(data) // 16 * 16
    out = bytes(data[:whole])
    for _ in range(16):
        c = Cipher(algorithms.AES(key), modes.ECB())
        op = c.encryptor() if encrypt else c.decryptor()
        out = op.update(out) + op.finalize()
    return out + bytes(data[whole:])


class Entry:
    __slots__ = ('name', 'path', 'directory', 'size', 'offset', 'stored', 'compressed', 'resource', 'rtype',
                 'flags', 'children', 'first', 'count')

    def __init__(self):
        self.children = []
        self.directory = False
        self.resource = False
        self.compressed = False
        self.rtype = 0
        self.flags = 0


def read(path, key):
    """The archive: (header dict, root Entry, flat list of file entries)."""
    with open(path, 'rb') as f:
        head = f.read(20)
        magic, toc_size, count, _, encrypted = struct.unpack('<5I', head)
        assert magic == MAGIC, 'not an RPF2 archive'
        f.seek(TOC_AT)
        toc = f.read(toc_size)
    if encrypted:
        toc = aes(toc, key, False)
    names_at = count * 16
    entries = []
    for i in range(count):
        w0, w1, w2, w3 = struct.unpack_from('<4I', toc, i * 16)
        e = Entry()
        end = toc.index(b'\0', names_at + w0)
        e.name = toc[names_at + w0:end].decode('latin-1')
        if w2 & 0x80000000:
            e.directory = True
            e.first, e.count = w2 & 0x7FFFFFFF, w3 & 0x0FFFFFFF
        else:
            e.size = w1
            e.resource = (w3 & 0xC0000000) == 0xC0000000
            if e.resource:
                e.rtype = w2 & 0xFF
                e.offset = w2 & 0x7FFFFF00
                e.stored = w1
                e.flags = w3
            else:
                e.offset = w2
                e.stored = w3 & 0xBFFFFFFF
                e.compressed = bool(w3 & 0x40000000)
        entries.append(e)
    files = []

    def link(i, base):
        e = entries[i]
        e.path = base
        if e.directory:
            for k in range(e.first, e.first + e.count):
                c = entries[k]
                c.path = (base + '/' + c.name) if base else c.name
                e.children.append(c)
                link(k, c.path)
        else:
            files.append(e)
    link(0, '')
    return dict(toc_size=toc_size, count=count, encrypted=bool(encrypted)), entries[0], files


def data_of(path, e):
    with open(path, 'rb') as f:
        f.seek(e.offset)
        raw = f.read(e.stored)
    if e.compressed:
        return zlib.decompress(raw, -15)
    return raw


def stored_of(path, e):
    with open(path, 'rb') as f:
        f.seek(e.offset)
        return f.read(e.stored)


def rsc_header_flags(data):
    """A resource file's (type, flags) from its little-endian RSC header."""
    magic, rtype, flags = struct.unpack_from('<3I', data, 0)
    assert magic == 0x05435352, 'not an RSC file'
    return rtype, flags


def write(out_path, source, root, files, key, encrypted, added):
    """The archive again, with `added` ({name: bytes} at the root) in place
    of or beside the root's files; every entry's stored bytes carried over
    unchanged (the originals from `source`)."""
    items = []   # (lower name, Entry or None, name, data bytes)
    for c in root.children:
        if c.directory:
            raise SystemExit('a nested directory is not rebuilt (%s)' % c.name)
        if c.name.lower() in {n.lower() for n in added}:
            continue
        items.append((c.name.lower(), c, c.name, stored_of(source, c)))
    for name, data in added.items():
        items.append((name.lower(), None, name, data))
    items.sort(key=lambda t: t[0])
    # the table: the root, then its children; the names
    names = bytearray()
    name_off = {}
    for lower, e, name, data in [('', root, root.name, None)] + items:
        name_off[name] = len(names)
        names += name.encode('latin-1') + b'\0'
    count = 1 + len(items)
    toc = bytearray(16 * count)
    struct.pack_into('<4I', toc, 0, name_off[root.name], 0, 1 | 0x80000000, len(items))
    toc_size = len(toc) + len(names)
    data_at = (TOC_AT + toc_size + BLOCK - 1) // BLOCK * BLOCK
    with open(out_path, 'wb') as f:
        f.write(b'\0' * data_at)
        at = data_at
        for k, (lower, e, name, data) in enumerate(items):
            f.seek(at)
            f.write(data)
            if e is not None:
                if e.resource:
                    w1, w2, w3 = e.stored, at | e.rtype, e.flags
                else:
                    w1, w2, w3 = e.size, at, e.stored | (0x40000000 if e.compressed else 0)
            else:
                rtype, flags = rsc_header_flags(data)
                assert flags & 0xC0000000 == 0xC0000000
                assert at & 0xFF == 0
                w1, w2, w3 = len(data), at | rtype, flags
            struct.pack_into('<4I', toc, 16 * (k + 1), name_off[name], w1, w2, w3)
            at = (at + len(data) + BLOCK - 1) // BLOCK * BLOCK
        f.seek(at - 1)
        f.write(b'\0')
        table = bytes(toc) + bytes(names)
        if encrypted:
            table = table + b'\0' * ((16 - len(table) % 16) % 16)
            table = aes(table, key, True)
        f.seek(0)
        f.write(struct.pack('<5I', MAGIC, len(table), count, 0, 1 if encrypted else 0))
        f.seek(TOC_AT)
        f.write(table)
    return count


def main():
    path, key_path, cmd = sys.argv[1:4]
    key = open(key_path, 'rb').read()
    head, root, files = read(path, key)
    if cmd == 'list':
        print('# %s: %d entries, table %d bytes, %s' % (path, head['count'], head['toc_size'],
                                                       'encrypted' if head['encrypted'] else 'plain'))
        for e in files:
            print('%-40s %9d %s' % (e.path, e.size, ('rsc type %d flags %08X' % (e.rtype, e.flags)) if e.resource
                                    else ('deflated %d' % e.stored if e.compressed else 'plain')))
    elif cmd == 'extract':
        out = sys.argv[4]
        os.makedirs(out, exist_ok=True)
        for e in files:
            dest = os.path.join(out, e.path.replace('/', os.sep))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, 'wb').write(data_of(path, e))
        print('%d files' % len(files))
    elif cmd == 'add':
        out = sys.argv[4]
        added = {os.path.basename(p): open(p, 'rb').read() for p in sys.argv[5:]}
        n = write(out, path, root, files, key, head['encrypted'], added)
        print('%s: %d entries (%d added)' % (out, n, len(added)))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
