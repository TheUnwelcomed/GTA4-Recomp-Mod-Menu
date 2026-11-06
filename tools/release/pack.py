"""Packs a release zip of the PS5 app: the packaged app folder (what
ps5/tools/ps5_package.sh wrote, out/ps5/ps5-app/PPSA94004) under its title id,
its addons/ replaced by the add-on payload (the vehicles and map sets that
are not in this repository), and the install notes at the zip's root. Nothing
is staged on disk: the zip is written straight from the two folders.

usage: python3 -I pack.py [--keep-key] [--without=<addon,...>] [--only=<addon,...>]
                          <app dir> <addons dir> <INSTALL.txt> <out.zip>
  e.g. pack.py out/ps5/ps5-app/PPSA94004 /path/to/addons INSTALL.txt
       LibertyRecompMenu-v1.7.37-PPSA94004.zip

A release over the upload limit goes in two zips (v1.7.45's snow, v1.7.47's
Chargers): --without names the add-on folders left out of the app's zip, and
--only makes the second zip of just those folders (no app; the notes still
at the root), to be copied over the same PPSA94004.

The zip is checked (testzip) after writing; the exit status says whether it
is sound. The app's AES key (Resources/aes_key.bin, the user's own) is left
out unless --keep-key is given (a zip going back to its owner only).
"""
import os
import sys
import zipfile

SKIP = {'aes_key.bin'}


def add_tree(z, root, prefix, skip_dirs=(), only_dirs=None):
    n = 0
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        dirnames[:] = sorted(d for d in dirnames if not (rel == '.' and d in skip_dirs) and
                             not (rel == '.' and only_dirs is not None and d not in only_dirs))
        if rel == '.' and only_dirs is not None:
            filenames = []  # (the second zip: the folders named, nothing at the root)
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
    without, only = (), None
    while args and args[0].startswith('--'):
        opt = args.pop(0)
        if opt == '--keep-key':
            SKIP.clear()
        elif opt.startswith('--without='):
            without = tuple(x for x in opt[len('--without='):].split(',') if x)
        elif opt.startswith('--only='):
            only = set(x for x in opt[len('--only='):].split(',') if x)
        else:
            sys.exit('unknown option ' + opt)
    app, addons, install, out = args[:4]
    title = os.path.basename(os.path.normpath(app))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        z.write(install, os.path.basename(install))
        n = 0 if only is not None else add_tree(z, app, title + '/', skip_dirs=('addons',))
        m = add_tree(z, addons, title + '/addons/', skip_dirs=without, only_dirs=only)
        if only is not None:
            missing = sorted(d for d in only if not os.path.isdir(os.path.join(addons, d)))
            if missing:
                sys.exit('add-ons not found: ' + ', '.join(missing))
    with zipfile.ZipFile(out) as z:
        bad = z.testzip()
        count = len(z.namelist())
    print('%s: %d app files + %d add-on files, %d entries, %.1f MB, %s' % (
        out, n, m, count, os.path.getsize(out) / 1e6, 'BAD: ' + bad if bad else 'sound'))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
