"""Packs a release zip of the PS5 app: the packaged app folder (what
ps5/tools/ps5_package.sh wrote, out/ps5/ps5-app/PPSA94004) under its title id,
its addons/ replaced by the add-on payload (the vehicles and map sets that
are not in this repository), and the install notes at the zip's root. Nothing
is staged on disk: the zip is written straight from the two folders.

usage: python3 -I pack.py [--keep-key] <app dir> <addons dir> <INSTALL.txt> <out.zip>
  e.g. pack.py out/ps5/ps5-app/PPSA94004 /path/to/addons INSTALL.txt
       LibertyRecompMenu-v1.7.37-PPSA94004.zip

The zip is checked (testzip) after writing; the exit status says whether it
is sound. The app's AES key (Resources/aes_key.bin, the user's own) is left
out unless --keep-key is given (a zip going back to its owner only).
"""
import os
import sys
import zipfile

SKIP = {'aes_key.bin'}


def add_tree(z, root, prefix, skip_dirs=()):
    n = 0
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        dirnames[:] = sorted(d for d in dirnames if not (rel == '.' and d in skip_dirs))
        for f in sorted(filenames):
            if f in SKIP:
                print('skipped', os.path.join(dirpath, f))
                continue
            path = os.path.join(dirpath, f)
            arc = prefix if rel == '.' else prefix + rel + '/'
            z.write(path, arc + f)
            n += 1
    return n


def main():
    args = sys.argv[1:]
    if args and args[0] == '--keep-key':
        SKIP.clear()
        args = args[1:]
    app, addons, install, out = args[:4]
    title = os.path.basename(os.path.normpath(app))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        z.write(install, os.path.basename(install))
        n = add_tree(z, app, title + '/', skip_dirs=('addons',))
        m = add_tree(z, addons, title + '/addons/')
    with zipfile.ZipFile(out) as z:
        bad = z.testzip()
        count = len(z.namelist())
    print('%s: %d app files + %d add-on files, %d entries, %.1f MB, %s' % (
        out, n, m, count, os.path.getsize(out) / 1e6, 'BAD: ' + bad if bad else 'sound'))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
